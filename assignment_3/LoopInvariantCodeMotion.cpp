#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <llvm-19/llvm/IR/Analysis.h>

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

#define DEBUG true

namespace {

struct LoopInvariantCodeMotion : PassInfoMixin<LoopInvariantCodeMotion> {

  /**
   * @brief checks if the variable is dead after exiting the loop
   *
   * @param I
   * @param LL
   * @return true
   * @return false
   */
  bool isDeadAfterLoop(Instruction *I, Loop *LL) {
    //scorro gli usi dell'istruzione e prendo il basic block dell'istruzione che ha l'uso
    for (User *U : I->users()) {
      Instruction *userInst = cast<Instruction>(U);
      BasicBlock *userBB = userInst->getParent();

      if (!LL->contains(userBB)) {  //se l'uso non è dentro al loop vuol dire che è dopo quindi non è dead
        return false;
      }
    }

    return true;
  }

  /**
   * @brief checks that the instruction is in a block that dominates every
   * loop's exit
   *
   * @param I
   * @param LL
   * @param DT
   * @return true
   * @return false
   */
  bool dominatesExits(Instruction *I, Loop *LL, DominatorTree &DT) {
    SmallVector<BasicBlock *> exitBlocks;
    LL->getExitBlocks(exitBlocks);  //torna gli exit block

    bool dominates = true;

    for (auto exitBlock : exitBlocks) { //scorro gli exit block
      if (!DT.dominates(I->getParent(), exitBlock)) { //controllo che il BB dell'istruzione domina ogni uscita, se non ne domina anche solo una, fallisce 
        dominates = false;
        break;
      }
    }

    return dominates;
  }

  /**
   * @brief debug printing for each loop, reporting loop invariant and moved
   * instructions
   *
   * @param invariantInstrs
   * @param motionInstrs
   */
  void printDebug(std::unordered_set<Instruction *> &invariantInstrs,
                  std::vector<Instruction *> &motionInstrs) {
    outs() << "Loop Invariant Instructions" << "\n";
    for (auto instr : invariantInstrs) {
      instr->print(outs());
      outs() << "\n";
    }

    outs() << "Code Motion Instructions" << "\n";
    for (auto instr : motionInstrs) {
      instr->print(outs());
      outs() << "\n";
    }
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    
    //Mi salvo gli handle delle varie analisi

    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    bool modified = false;

    // ritorna i top level loop in preorder
    for (Loop *LL : LI.getLoopsInPreorder()) {
      // Verifica che il loop sia in forma canonica (Loop Simplify Form), ossia che
      // possieda un unico preheader (essenziale per spostarvi il codice invariant),
      // un unico backedge e blocchi di uscita dedicati.
      if (!LL->isLoopSimplifyForm())
        continue;

        /* 
          SCEV permette di sapere quante volte itero un loop, e se è = 0 non ha senso ottimizzarlo poichè non ci entriamo
        */
      SCEV const *backEdgeCount = SE.getBackedgeTakenCount(LL);
      if (const SCEVConstant *tripCount =
              dyn_cast<SCEVConstant>(backEdgeCount)) {
        if (tripCount->getAPInt().getSExtValue() == 0)
          continue;
      }

      //creo 2 set per ordine così è più pulito, per le invarianti e per quelle da muovere nel pre-header 
      std::unordered_set<Instruction *> invariantSet;
      std::vector<Instruction *> toMove;

      /* 
      ti ritorna i blocchi dentro ad un loop in reverse post order, così da visitare tutti i predecessori prima di visitare un blocco
      */
      LoopBlocksRPO LBRPO(LL);
      LBRPO.perform(&LI);

      // Loop Invariant
      for (BasicBlock *BB : LBRPO) {
        for (Instruction &I : *BB) {
          // le istruzioni che hanno side effetcs non sono invariant
          if(I.mayHaveSideEffects())
            continue;

          // le PHI e le Br non sono invariant
          if (I.getOpcode() == Instruction::PHI ||
              I.getOpcode() == Instruction::Br)
            continue;

          //  le istruzioni che leggono o scrivono in memoria (load e store) non sono invariant
          if (I.mayHaveSideEffects())
          if (I.mayReadOrWriteMemory()) //es load or store
            continue;

          bool isInvariant = true;

          //scorro gli operandi dell'istruzione
          for (Use &Op : I.operands()) {  
            Value *operandValue = Op.get();

            if (auto *op_instr = dyn_cast<Instruction>(operandValue)) { //provo a castarlo ad instruction, se non lo è vuol dire che è costante quindi invariant
              if (LL->contains(op_instr->getParent()) &&
                  invariantSet.count(op_instr) == 0) {  //controllo se è definita nel loop, e se lo è, controllo che l'istruzione che la definisce non sia già stata marcata come invariant e quindi non si trova nel set e che quindi non è invariant
                isInvariant = false;
                break;
              }
            }
          }

          if (isInvariant) {
            invariantSet.insert(&I);
          }
        }
      }

      // Code Motion
      // altro set per le istruzioni che sono state già spostate, così da non spostarle più volte
      std::unordered_set<Instruction *> isMoved;

      for (BasicBlock *BB : LBRPO) {
        for (Instruction &I : *BB) {
          // controllo che l'istruzione sia invariant e che domini tutti gli exit block o che sia dead dopo il loop
          if (invariantSet.count(&I) > 0 &&
              (dominatesExits(&I, LL, DT) || isDeadAfterLoop(&I, LL))) {  
                // controllo che tutte le dipendenze dell'istruzione siano già state spostate, così da non spostare un'istruzione prima di aver spostato le sue dipendenze
                // quindi itero sugli operandi dell'istruzione e controllo se sono già stati spostati, se non lo sono vuol dire che non posso spostare l'istruzione
                bool depsMoved = true;
            for (Use &Op : I.operands()) {
              if (auto *op_instr = dyn_cast<Instruction>(Op.get())) {
                if (LL->contains(op_instr->getParent()) &&
                    isMoved.count(op_instr) == 0) {
                  depsMoved = false;
                  break;
                }
              }
            }
            // se tutte le dipendenze sono state spostate, allora posso spostare l'istruzione
            if (depsMoved) {
              toMove.push_back(&I);
              isMoved.insert(&I);
            }
          }
        }
      }

      //prendo il pre header e il suo terminatore poichè voglio spostare le istruzioni prima del terminatore e nel pre header
      auto preheader = LL->getLoopPreheader();
      auto lastInstr = preheader->getTerminator();

      //muovo prima del terminatore
      for (auto instr : toMove) {
        instr->moveBefore(lastInstr);
        modified = true;
      }

      if (DEBUG) {
        printDebug(invariantSet, toMove);
      }
    }

    if (modified)
      return PreservedAnalyses::none();

    return PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};
} // namespace

llvm::PassPluginLibraryInfo getLoopPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopInvariantCodeMotion",
          LLVM_VERSION_STRING, [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "LI-CM") {
                    FPM.addPass(LoopInvariantCodeMotion());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getLoopPassPluginInfo();
}
