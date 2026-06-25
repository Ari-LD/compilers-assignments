#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <llvm-19/llvm/Analysis/LazyCallGraph.h>
#include <llvm-19/llvm/Analysis/LoopAnalysisManager.h>
#include <llvm-19/llvm/IR/Analysis.h>

#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/CFG.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Instructions.h>
#include <llvm-19/llvm/IR/IntrinsicInst.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>

#include "llvm/ADT/SetVector.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <map>
#include <vector>

using namespace llvm;

namespace {

struct LoopFusion : PassInfoMixin<LoopFusion> {

  std::map<Loop *, const SCEV *> loopsTripCountMap;

  /**
   * @brief returns the correct entry Basic Block based on whether the loop is
   * guarded or not
   *
   * @param L
   * @return BasicBlock*
   */
  BasicBlock *getLoopEntry(Loop *L) {
    return L->isGuarded() ? L->getLoopGuardBranch()->getParent()
                          : L->getLoopPreheader();
  }

  /**
   * @brief return the correct exit basic block based on whether the loop is
   * guarded or not
   *
   * @param L
   * @return BasicBlock*
   */
  BasicBlock *getLoopExit(Loop *L) {
    if (L->isGuarded()) {
      BranchInst *GuardBr = L->getLoopGuardBranch();
      BasicBlock *Preheader = L->getLoopPreheader();

      // checks both successors of the guard block, one should be the preheader
      // of the loop while the other is the exit block.
      if (GuardBr->getSuccessor(0) == Preheader) {
        return GuardBr->getSuccessor(1);
      } else {
        return GuardBr->getSuccessor(0);
      }
    }

    return L->getExitBlock();
  }

  /**
   * @brief checks if the guard blocks of the two loops are equivalent
   *
   * @param l1GuardCond
   * @param l2GuardCond
   * @return true
   * @return false
   */
  bool areConditionsEquivalent(BranchInst *l1GuardCond,
                               BranchInst *l2GuardCond) {

    Value *Cond1 = l1GuardCond->getCondition();
    Value *Cond2 = l2GuardCond->getCondition();

    // in case they share the same variable
    if (Cond1 == Cond2)
      return true;

    if (auto *Inst1 = dyn_cast<Instruction>(Cond1)) {
      if (auto *Inst2 = dyn_cast<Instruction>(Cond2)) {
        return Inst1->isIdenticalTo(Inst2);
      }
    }

    return false;
  }

  /**
   * @brief checks if the block is empty (it contains only a branch or
   * if it has a comparison instruction for guarded loops
   *
   * @param BB
   * @param BI
   * @param isGuard
   * @return true
   * @return false
   */
  bool isBlockEmpty(BasicBlock *BB, BranchInst *BI, bool isGuarded) {
    Instruction *FirstInst = BB->getFirstNonPHIOrDbg();

    // the block is empty
    if (FirstInst == BI) {
      return true;
    }

    // for guarded blocks, an additional comparison instruction might be present
    if (isGuarded && BI->isConditional()) {
      if (FirstInst == dyn_cast<Instruction>(BI->getCondition())) {
        if (FirstInst->getNextNonDebugInstruction() == BI) {
          return true;
        }
      }
    }

    return false;
  }

  /**
   * @brief helper function to get if an instruction is defined in the given
   * loop
   *
   * @param V
   * @param L
   * @return true
   * @return false
   */
  bool isDefinedInLoop(Value *V, Loop *L) {
    if (auto *I = dyn_cast<Instruction>(V))
      return L->contains(I->getParent());
    return false;
  }

  /**
   * @brief helper function to get if an instruction is used in the given loop
   * can also check if the instruction writes to memory instead of just reading
   * it based on the given boolean
   *
   * @param I
   * @param L
   * @param checkForMemoryWrite
   * @return true
   * @return false
   */
  bool isUsedInLoop(Instruction *I, Loop *L, bool checkForMemoryWrite) {
    for (User *U : I->users()) {
      if (auto *UserInst = dyn_cast<Instruction>(U)) {
        if (L->contains(UserInst->getParent())) {
          if (checkForMemoryWrite) {
            if (UserInst->mayWriteToMemory())
              return true;
          } else
            return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief checks if two loops are adjacent by checking that there are no
   * other basic blocks between the two loops, or in other words that the
   * exit block of the first loop coincides with the entry block of
   * the second loop
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool areAdjacent(Loop *L1, Loop *L2, SetVector<Instruction *> &toMoveBeforeL1,
                   SetVector<Instruction *> &toMoveAfterL2) {
    BasicBlock *ExitL1 = getLoopExit(L1);
    BasicBlock *EntryL2 = getLoopEntry(L2);
    toMoveBeforeL1.clear();
    toMoveAfterL2.clear();

    bool isGuarded = L1->isGuarded();

    if (!ExitL1 || !EntryL2)
      return false;

    // both must be guarded or unguarded
    if (isGuarded != L2->isGuarded())
      return false;

    // if guarded, both must be equivalent
    if (isGuarded && !areConditionsEquivalent(L1->getLoopGuardBranch(),
                                              L2->getLoopGuardBranch())) {
      return false;
    }

    BranchInst *BI1 = dyn_cast<BranchInst>(ExitL1->getTerminator());

    if (!BI1)
      return false;

    // first case, exit and entry correspond
    if (ExitL1 == EntryL2) {

      // if there's an instruction or more between the loops, try to move
      // it/them
      if (!isBlockEmpty(ExitL1, BI1, isGuarded)) {
        if (!canMoveInstructionsInBetweenLoops(L1, L2, ExitL1, BI1,
                                               toMoveBeforeL1, toMoveAfterL2)) {
          outs() << "Error: there are unmovable instructions between the "
                    "loops, so they can't be made adjacent.\n";
          return false;
        }
      }

      return true;
    }

    // second case, exit and entry do not correspond, but they could still be
    // adjacent if there aren't unmovable instructions in the middle

    BranchInst *BI2 = dyn_cast<BranchInst>(EntryL2->getTerminator());
    if (!BI2 || !BI1->isUnconditional())
      return false;

    // a redundant "trampoline" block could be in the middle
    BasicBlock *NextBB = BI1->getSuccessor(0);
    if (NextBB != EntryL2) {
      BranchInst *NextBI = dyn_cast<BranchInst>(NextBB->getTerminator());
      if (!NextBI || !NextBI->isUnconditional() ||
          NextBI->getSuccessor(0) != EntryL2 ||
          !isBlockEmpty(NextBB, NextBI, false)) {
        return false;
      }
      // BI1->setSuccessor(0, EntryL2);
    }

    // finally we check for instructions between ExitL1 and EntryL2
    if (!isBlockEmpty(ExitL1, BI1, false) ||
        !isBlockEmpty(EntryL2, BI2, isGuarded)) {
      if (!canMoveInstructionsInBetweenLoops(L1, L2, ExitL1, BI1,
                                             toMoveBeforeL1, toMoveAfterL2,
                                             EntryL2, BI2)) {
        outs() << "Error: there are unmovable instructions between the "
                  "loops, so they can't be made adjacent.\n";
        return false;
      }
    }

    return true;
  }

  /**
   * @brief saves every instruction between two loops L1 and L2 in a vector
   * it then checks for each instruction, based on it's dependencies with L1 and
   * L2, if it can be moved before L1, after L2, or can't. if even just one
   * instruction can't be moved then we stop as we can't fuse two loops if even
   * just one instruction is in between, this is because the only way an
   * instruction can't be moved is if it's using something from L1 and if L2 is
   * using the result/variable of that instruction so fusing two loops with
   * instructions in between requires to move them out of the way first, in a
   * case like this unmovable instruction that can't happen and the fusion is
   * unfeasible
   *
   * @param L1
   * @param L2
   * @param ExitL1
   * @param BI1
   * @param EntryL2
   * @param BI2
   * @return true
   * @return false
   */
  bool canMoveInstructionsInBetweenLoops(
      Loop *L1, Loop *L2, BasicBlock *ExitL1, BranchInst *BI1,
      SetVector<Instruction *> &toMoveBeforeL1,
      SetVector<Instruction *> &toMoveAfterL2, BasicBlock *EntryL2 = nullptr,
      BranchInst *BI2 = nullptr) {

    std::vector<Instruction *> toCheckForCodeMotion;

    for (Instruction &I : *ExitL1) {
      if (!isa<PHINode>(&I) && &I != BI1 && !isa<DbgInfoIntrinsic>(&I)) {
        toCheckForCodeMotion.push_back(&I);
      }
    }

    // I don't need to check for EntryL2 != ExitL1 as I do that in areAdjacent
    // before calling this function, if they're equal then I don't give EntryL2
    // as a parameter and it will be nullptr meaning I just need to check that
    // instead (should be equivalent either way)
    if (EntryL2 != nullptr && EntryL2 != L2->getHeader()) {
      for (Instruction &I : *EntryL2) {
        if (!isa<PHINode>(&I) && &I != BI2 && !isa<DbgInfoIntrinsic>(&I)) {
          toCheckForCodeMotion.push_back(&I);
        }
      }
    }

    for (Instruction *I : toCheckForCodeMotion) {
      bool neededBeforeL1 = false;
      bool neededAfterL2 = false;

      if (I->mayHaveSideEffects() || I->mayReadOrWriteMemory()) {
        return false;
      }
      // checks if the instruction is needed after L2
      // if it is then we are forced to move it before L1
      if (isUsedInLoop(I, L2, false))
        neededBeforeL1 = true;

      // checks that the instruction is using something from the previous loop,
      // L1, if it is then we are forced to move it after L2
      for (Value *Op : I->operands()) {
        if (isDefinedInLoop(Op, L1))
          neededAfterL2 = true;
        // instructions dependent on each other must be on the same side
        else if (auto *OpInst = dyn_cast<Instruction>(Op)) {
          if (toMoveAfterL2.count(OpInst))
            neededAfterL2 = true;
          else if (toMoveBeforeL1.count(OpInst))
            neededBeforeL1 = true;
        }
        // edge case, the instruction could be using a phi node contained within
        // the same block, so we cannot move the instruction safely
        else if (auto phiInstr = dyn_cast<PHINode>(Op)) {
          if (phiInstr->getParent() == I->getParent()) {
            return false;
          }
        }
      }

      // if it turns out that we need to move the instruction both after L2 and
      // before L1 because of it's dependencies, then we can't move it at all,
      // if we find even just one instruction that can't be moved then we can
      // stop altogether as the loop fusion won't be feasible unless every
      // instruction is moved. it was decided to start up the bools as true and
      // make them false in the previous code to avoid an if check, by starting
      // them as true and making them false under those conditions we can just
      // check for when there are dependencies from both loops and then just
      // from one, instead starting from false -> true we would have had this
      // check with both as true, the checks for both by themselves as the
      // single true bool, and if there are no dependencies from either side
      // then both would be false requiring a fourth check,/ this way we use
      // boolean logic to save a needless check
      if (neededBeforeL1 && neededAfterL2)
        return false;

      if (neededBeforeL1)
        toMoveBeforeL1.insert(I);
      else if (neededAfterL2)
        toMoveAfterL2.insert(I);
      else
        toMoveBeforeL1.insert(I); // by default if there are no dependencies we
                                  // move the instructions before L1
    }

    return true;
  }

  /**
   * @brief Moves given instructions before the first loop or after the second
   * loop
   *
   * @param L1 first loop
   * @param L2 second loop
   * @param toMoveBeforeL1 instructions to move before the first loop
   * @param toMoveAfterL2 instructions to move after the second loop
   */
  void moveInstructionsInBetweenLoops(Loop *L1, Loop *L2,
                                      SetVector<Instruction *> &toMoveBeforeL1,
                                      SetVector<Instruction *> &toMoveAfterL2) {
    BasicBlock *EntryL1 = getLoopEntry(L1);
    if (EntryL1) {
      Instruction *whereToMoveTo = EntryL1->getTerminator();
      for (Instruction *I : toMoveBeforeL1)
        I->moveBefore(whereToMoveTo);
    }

    BasicBlock *ExitL2 = getLoopExit(L2);
    if (ExitL2) {
      Instruction *whereToMoveTo = ExitL2->getFirstNonPHI();
      for (Instruction *I : toMoveAfterL2)
        I->moveBefore(whereToMoveTo);
    }
  }

  /**
   * @brief checks if two loops iterate the same number of times
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool hasSameTripCount(Loop *L1, Loop *L2) {
    const SCEV *TC1 = loopsTripCountMap[L1];
    const SCEV *TC2 = loopsTripCountMap[L2];

    if (isa<SCEVCouldNotCompute>(TC1) || isa<SCEVCouldNotCompute>(TC2)) {
      return false;
    }

    return TC1 == TC2;
  }

  /**
   * @brief Check if two loops are control flow equivalent (CFE)
   *
   * @param L1 first loop
   * @param L2 second loop
   * @param DT DomTree
   * @param PDT PostDomTree
   * @return true
   * @return false
   */

  bool areControlFlowEquivalent(Loop *L1, Loop *L2, DominatorTree &DT,
                                PostDominatorTree &PDT) {

    auto Pre1 = getLoopEntry(L1);
    auto Pre2 = getLoopEntry(L2);

    if (!Pre1 || !Pre2) {
      return false;
    }

    return (DT.dominates(Pre1, Pre2) && PDT.dominates(Pre2, Pre1));
  }

  /**
   * @brief Helper function, it gets the Value operand used as the pointer in
   * the store/load instruction
   *
   *
   * @param Inst
   * @return Value*
   */
  Value *getPointerOperand(Instruction *Inst) {
    if (LoadInst *LI = dyn_cast<LoadInst>(Inst))
      return LI->getPointerOperand();
    if (StoreInst *SI = dyn_cast<StoreInst>(Inst))
      return SI->getPointerOperand();
    return nullptr;
  }

  /**
   * @brief checks that there are no negative distance dependencies
   * between two loops, or in other words L2 can't have an instruction
   * at iteration m that uses a value computed by L1 at a future
   * iteration m+n (where n > 0)
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
bool hasNegativeDependencies(Loop *L1, Loop *L2, ScalarEvolution &SE) {

  std::vector<Instruction *> opsL1, opsL2;

  for (BasicBlock *BB : L1->getBlocks())
    for (Instruction &I : *BB)
      if (I.mayReadOrWriteMemory())
        opsL1.push_back(&I);

  for (BasicBlock *BB : L2->getBlocks())
    for (Instruction &I : *BB)
      if (I.mayReadOrWriteMemory())
        opsL2.push_back(&I);

  if (opsL1.empty() || opsL2.empty())
    return false;

  for (Instruction *I1 : opsL1) {
    for (Instruction *I2 : opsL2) {

      bool isWAR = I1->mayReadFromMemory() && I2->mayWriteToMemory();
      bool isRAW = I1->mayWriteToMemory()  && I2->mayReadFromMemory();

      if (!isWAR && !isRAW)
        continue;

      Value *Ptr1 = getPointerOperand(I1);
      Value *Ptr2 = getPointerOperand(I2);

      if (!Ptr1 || !Ptr2)
        return true;

      if (getUnderlyingObject(Ptr1) != getUnderlyingObject(Ptr2))
        continue;

      /*
        Una SCEV di un puntatore che varia dentro un loop viene rappresentata da LLVM come una SCEVAddRecExpr:
        { Start, +, Stride }<Loop>, ovvero al passo i-esimo del loop il puntatore punta a Start + i*Stride.
        Start = il valore del puntatore alla prima iterazione (i=0)
        Stride = di quanto avanza il puntatore ad ogni iterazione

        Per ogni coppia (I1 appartiene a L1, I2 appartiene a L2) che accede allo stesso array base, estraiamo la SCEV del puntatore di ciascuno e ne prendiamo start e stride:
        c[i]   -> Start = c,    Stride = 4
        c[i+1] -> Start = c+4,  Stride = 4
        Se i due stride sono uguali (pattern di accesso identico, solo sfasati), la dipendenza è determinata interamente dalla differenza degli start:

        RAW: se Start2 - Start1 > 0, L2 legge avanti -> dipendenza negativa -> blocca
        WAR: se Start1 - Start2 > 0, L1 legge avanti -> dipendenza negativa -> blocca

        Se gli stride sono diversi o sconosciuti, non possiamo provare l'assenza di hazard -> blocchiamo per sicurezza.      
      */

      const SCEV *S1 = SE.getSCEV(Ptr1);
      const SCEV *S2 = SE.getSCEV(Ptr2);

      // Estrai start e stride di ciascuno
      const SCEV *Start1 = S1, *Stride1 = nullptr;
      const SCEV *Start2 = S2, *Stride2 = nullptr;

      if (auto *AR1 = dyn_cast<SCEVAddRecExpr>(S1)) {
        Start1  = AR1->getStart();
        Stride1 = AR1->getStepRecurrence(SE);
      }
      if (auto *AR2 = dyn_cast<SCEVAddRecExpr>(S2)) {
        Start2  = AR2->getStart();
        Stride2 = AR2->getStepRecurrence(SE);
      }

      // Se gli stride sono uguali (stessi pattern di accesso),
      // la dipendenza è determinata interamente dalla differenza degli start.
      // RAW: L1 scrive start1+i, L2 legge start2+i.
      //      Se start2 > start1 → L2 legge avanti rispetto a L1
      //      → dipendenza loop-carried negativa → blocca.
      // WAR: L1 legge start1+i, L2 scrive start2+i.
      //      Se start1 > start2 → L1 legge avanti rispetto a L2 → blocca.
      if (Stride1 && Stride2 && Stride1 == Stride2) {
        const SCEV *diffStart = isRAW
            ? SE.getMinusSCEV(Start2, Start1)
            : SE.getMinusSCEV(Start1, Start2);

        if (SE.isKnownPositive(diffStart))
          return true;
      } else {
        // Stride diversi o non noti: non possiamo provare assenza di hazard
        const SCEV *diffStart = SE.getMinusSCEV(Start2, Start1);
        if (SE.isKnownNegative(diffStart) || SE.isKnownPositive(diffStart))
          return true;
      }
    }
  }

  return false;
}

  /**
   * @brief checks for scalar dependencies between two loops
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool hasScalarDependencies(Loop *L1, Loop *L2) {
    for (BasicBlock *BB : L1->getBlocks()) {
      for (Instruction &I : *BB) {
        if (isUsedInLoop(&I, L2, false))
          return true;
      }
    }
    return false;
  }

  bool isLoopDoWhile(Loop *L1) {
    if (auto branchHeader =
            dyn_cast<BranchInst>(L1->getHeader()->getTerminator()))
      return branchHeader->getNumSuccessors() <= 1;

    return false;
  }
  /**
   * @brief updates the phi nodes modifying the label of OldPred with the
   * NewPred label
   *
   * @param TargetBB
   * @param OldPred
   * @param NewPred
   */
  void updatePhiNodes(BasicBlock *TargetBB, BasicBlock *OldPred,
                      BasicBlock *NewPred) {
    for (PHINode &PN : TargetBB->phis()) {
      int blockIndex = PN.getBasicBlockIndex(OldPred);
      if (blockIndex >= 0) {
        PN.setIncomingBlock(blockIndex, NewPred);
      }
    }
  }

  /**
   * @brief fuses normal loops
   *
   * @param L1
   * @param L2
   * @param LI
   * @return true
   * @return false
   */
  bool fuseLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1HeaderTerminator = L1Header->getTerminator();
    auto L1InductionVar = L1->getCanonicalInductionVariable();
    auto L1Latch = L1->getLoopLatch();
    auto L1ExitBlock = getLoopExit(L1);

    auto L2Header = L2->getHeader();
    auto L2HeaderTerminator = L2Header->getTerminator();
    auto L2ExitBlock = getLoopExit(L2);
    auto L2EntryBlock = getLoopEntry(L2);
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2Latch = L2->getLoopLatch();

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());

    BasicBlock *L2BodyEntry =
        (L2HeaderTerminator->getSuccessor(0) == L2ExitBlock)
            ? L2HeaderTerminator->getSuccessor(1)
            : L2HeaderTerminator->getSuccessor(0);

    // we manage the phi nodes present in the second header and the induction
    // var make_early_inc_range increments before, so we are sure to not
    // invalidate the pointer
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      if (&PN == L2InductionVar) {
        // induction var is replace by the one in the first loop
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        // other phi nodes are moved in L1Header after the others
        Instruction *InsertPt = L1Header->getFirstNonPHI();
        PN.moveBefore(InsertPt);

        int entryIdx = PN.getBasicBlockIndex(L2EntryBlock);
        if (entryIdx >= 0) {
          PN.setIncomingBlock(entryIdx, getLoopEntry(L1));
        }

        int latchIdx = PN.getBasicBlockIndex(L2Latch);
        if (latchIdx >= 0) {
          PN.setIncomingBlock(latchIdx, L1Latch);
        }
      }
    }

    // the exit block of L1 is now the exit block of L2
    if (L1HeaderTerminator->getSuccessor(0) == L1ExitBlock) {
      L1HeaderTerminator->setSuccessor(0, L2ExitBlock);
    } else {
      L1HeaderTerminator->setSuccessor(1, L2ExitBlock);
    }
    updatePhiNodes(L2ExitBlock, L2Header, L1Header);

    // the blocks in L1 that pointed to the L1 Latch now go to the body of L2
    for (BasicBlock *PredL1 : L1LatchPreds) {
      auto predTerminator = PredL1->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L1Latch) {
          predTerminator->setSuccessor(i, L2BodyEntry);
          updatePhiNodes(L2BodyEntry, L2Header, PredL1);
        }
      }
    }

    // the blocks in L2 that pointed to the L2 Latch now go to the Latch of L1
    for (BasicBlock *PredL2 : L2LatchPreds) {
      auto predTerminator = PredL2->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L2Latch) {
          predTerminator->setSuccessor(i, L1Latch);
          for (BasicBlock *OldPredL1 : L1LatchPreds) {
            updatePhiNodes(L1Latch, OldPredL1, PredL2);
          }
        }
      }
    }
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector();

    // should manage the subloops
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    /* Should move blocks that belong to L2 to L1, except the header and the
     latch */
    std::vector<BasicBlock *> blocksToMove(L2->block_begin(), L2->block_end());
    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Header && BB != L2Latch) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    LI.erase(L2);
    return true;
  }

  /**
   * @brief fuses guarded loops
   *
   * @param L1
   * @param L2
   * @param LI
   * @return true
   * @return false
   */
  bool fuseGuardedLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1Latch = L1->getLoopLatch();
    auto L1Preheader = L1->getLoopPreheader();
    auto L1InductionVar = L1->getCanonicalInductionVariable();

    auto L2Header = L2->getHeader();
    auto L2Latch = L2->getLoopLatch();
    auto L2Preheader = L2->getLoopPreheader();
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2GuardBr = L2->getLoopGuardBranch();
    auto L2GuardBB = L2GuardBr->getParent();

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    // the exit of the second loop guard
    BasicBlock *L2Bypass = (L2GuardBr->getSuccessor(0) == L2Preheader)
                               ? L2GuardBr->getSuccessor(1)
                               : L2GuardBr->getSuccessor(0);

    BasicBlock *L2BodyEntry = L2Header;

    if (!L2BodyEntry) {
      outs() << "l2 body entry not found\n";
      return false;
    }

    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());

    // phi nodes are managed like in the normal fusion
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      if (&PN == L2InductionVar) {
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        Instruction *InsertPt = L1Header->getFirstNonPHI();
        PN.moveBefore(InsertPt);

        int entryIdx = PN.getBasicBlockIndex(L2Preheader);
        if (entryIdx >= 0) {
          PN.setIncomingBlock(entryIdx, L1Preheader);
        }

        int latchIdx = PN.getBasicBlockIndex(L2Latch);
        if (latchIdx >= 0) {
          PN.setIncomingBlock(latchIdx, L1Latch);
        }
      }
    }

    // blocks pointing to the L2 Guard now point to the exit
    std::vector<BasicBlock *> L2GuardPreds(predecessors(L2GuardBB).begin(),
                                           predecessors(L2GuardBB).end());
    for (BasicBlock *Pred : L2GuardPreds) {
      auto *Term = Pred->getTerminator();
      for (unsigned i = 0; i < Term->getNumSuccessors(); i++) {
        if (Term->getSuccessor(i) == L2GuardBB) {
          Term->setSuccessor(i, L2Bypass);
          updatePhiNodes(L2Bypass, L2GuardBB, Pred);
        }
      }
    }

    // blocks pointing to the L1 Latch now point to the L2 Body
    for (BasicBlock *PredL1 : L1LatchPreds) {
      auto predTerminator = PredL1->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L1Latch) {
          predTerminator->setSuccessor(i, L2BodyEntry);
          updatePhiNodes(L2BodyEntry, L2Header, PredL1);
        }
      }
    }

    // finally blocks pointing to the L2 Latch now point to the L1 Latch
    for (BasicBlock *PredL2 : L2LatchPreds) {
      auto predTerminator = PredL2->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L2Latch) {
          predTerminator->setSuccessor(i, L1Latch);
          for (BasicBlock *OldPredL1 : L1LatchPreds) {
            updatePhiNodes(L1Latch, OldPredL1, PredL2);
          }
        }
      }
    }

    // Updates loops info
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector();
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    std::vector<BasicBlock *> blocksToMove;
    for (BasicBlock *BB : L2->getBlocks()) {
      if (LI.getLoopFor(BB) == L2) {
        blocksToMove.push_back(BB);
      }
    }

    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Latch && BB != L2Preheader && BB != L2GuardBB) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    LI.erase(L2);
    return true;
  }

  /**
   * @brief fuse function for when we have two do while loops
   *
   * @param L1
   * @param L2
   * @param LI
   * @return true
   * @return false
   */
  bool fuseDoWhile(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1Latch = L1->getLoopLatch();
    auto L1Preheader = L1->getLoopPreheader();
    auto L1InductionVar = L1->getCanonicalInductionVariable();
    auto L1LatchBr = dyn_cast<BranchInst>(L1Latch->getTerminator());

    auto L2Header = L2->getHeader();
    auto L2Latch = L2->getLoopLatch();
    auto L2Preheader = L2->getLoopPreheader();
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2LatchBr = dyn_cast<BranchInst>(L2Latch->getTerminator());
    // auto L2GuardBr = L2->getLoopGuardBranch();
    // auto L2GuardBB = L2GuardBr->getParent();

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    // the exit of the second loop guard
    BasicBlock *L2Bypass = (L2LatchBr->getSuccessor(0) == L2Header)
                               ? L2LatchBr->getSuccessor(1)
                               : L2LatchBr->getSuccessor(0);

    BasicBlock *L2BodyEntry = L2Header;

    if (!L2BodyEntry) {
      outs() << "l2 body entry not found\n";
      return false;
    }

    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());

    // phi nodes are managed like in the normal fusion
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      if (&PN == L2InductionVar) {
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        Instruction *InsertPt = L1Header->getFirstNonPHI();
        PN.moveBefore(InsertPt);

        int entryIdx = PN.getBasicBlockIndex(L2Preheader);
        if (entryIdx >= 0) {
          PN.setIncomingBlock(entryIdx, L1Preheader);
        }

        int latchIdx = PN.getBasicBlockIndex(L2Latch);
        if (latchIdx >= 0) {
          PN.setIncomingBlock(latchIdx, L1Latch);
        }
      }
    }

    // the exit block of L1 is now the exit block of L2
    if (L1LatchBr->getSuccessor(0) == L1Header) {
      L1LatchBr->setSuccessor(1, L2Bypass);
    } else {
      L1LatchBr->setSuccessor(0, L2Bypass);
    }
    updatePhiNodes(L2Bypass, L2Latch, L1Latch);

    // blocks pointing to the L1 Latch now point to the L2 Body
    for (BasicBlock *PredL1 : L1LatchPreds) {
      auto predTerminator = PredL1->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L1Latch) {
          predTerminator->setSuccessor(i, L2BodyEntry);
          updatePhiNodes(L2BodyEntry, L2Header, PredL1);
        }
      }
    }

    // finally blocks pointing to the L2 Latch now point to the L1 Latch
    for (BasicBlock *PredL2 : L2LatchPreds) {
      auto predTerminator = PredL2->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L2Latch) {
          predTerminator->setSuccessor(i, L1Latch);
          for (BasicBlock *OldPredL1 : L1LatchPreds) {
            updatePhiNodes(L1Latch, OldPredL1, PredL2);
          }
        }
      }
    }

    // Updates loops info
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector();
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    std::vector<BasicBlock *> blocksToMove;
    for (BasicBlock *BB : L2->getBlocks()) {
      if (LI.getLoopFor(BB) == L2) {
        blocksToMove.push_back(BB);
      }
    }

    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Latch && BB != L2Preheader) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    LI.erase(L2);
    return true;
  }
  /**
   * @brief main function for loop fusion, it processes loops at each nest level
   * by starting from the outer loops and exploring recursively the inner loops
   *
   * @param siblings
   * @param DT
   * @param PDT
   * @param SE
   * @param LI
   * @param F
   * @return true
   * @return false
   */
  bool processNestLevelLoops(std::vector<Loop *> &siblings, DominatorTree &DT,
                             PostDominatorTree &PDT, ScalarEvolution &SE,
                             LoopInfo &LI, Function &F) {
    std::vector<Loop *> candidateLoops;
    bool fused = false;

    // filtering loops that are not candidate for LF
    for (Loop *L : siblings) {
      if (L->isLoopSimplifyForm()) {
        candidateLoops.push_back(L);
      }
    }

    std::vector<std::vector<Loop *>> cfeGroups;
    if (candidateLoops.size() >= 2) {
      // make sure the loops are ordered from first to last
      std::sort(candidateLoops.begin(), candidateLoops.end(),
                [&](Loop *L1, Loop *L2) {
                  return DT.dominates(getLoopEntry(L1), getLoopEntry(L2));
                });

      for (auto &loop : candidateLoops) {
        bool addedToGroup = false;

        for (auto &group : cfeGroups) {
          if (areControlFlowEquivalent(group.front(), loop, DT, PDT)) {
            group.push_back(loop);
            addedToGroup = true;
            break;
          }
        }

        if (!addedToGroup) {
          cfeGroups.push_back({loop});
        }
      }
    }

    // debug output
    for (auto &group : cfeGroups) {
      if (group.size() >= 2) {
        outs() << "found CFE with size " << group.size() << "\n";
      }
    }

    for (auto &group : cfeGroups) {
      int baseIndex = 0;
      auto &baseLoop = group[baseIndex];
      while (group.size() >= 2 && baseIndex < group.size() - 1) {
        auto nextLoop = group[baseIndex + 1];

        if (!hasSameTripCount(baseLoop, nextLoop)) {
          outs() << "Failed : Could not compute or different trip counts\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (hasNegativeDependencies(baseLoop, nextLoop, SE)) {
          outs() << "Failed: negative dependencies found\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (hasScalarDependencies(baseLoop, nextLoop)) {
          outs() << "Failed: scalar dependencies found\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        SetVector<Instruction *> toMoveBeforeL1;
        SetVector<Instruction *> toMoveAfterL2;

        if (!areAdjacent(baseLoop, nextLoop, toMoveBeforeL1, toMoveAfterL2)) {
          outs() << "Failed: loops are not adjacent\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (!toMoveAfterL2.empty() || !toMoveBeforeL1.empty()) {
          moveInstructionsInBetweenLoops(baseLoop, nextLoop, toMoveBeforeL1,
                                         toMoveAfterL2);
        }

        outs() << "All checks completed, trying to fuse...\n";
        bool fusionSuccess = false;
        if (baseLoop->isGuarded()) {
          fusionSuccess = fuseGuardedLoops(baseLoop, nextLoop, LI);
        } else if (isLoopDoWhile(baseLoop) && isLoopDoWhile(nextLoop)) {
          fusionSuccess = fuseDoWhile(baseLoop, nextLoop, LI);
        } else {
          fusionSuccess = fuseLoops(baseLoop, nextLoop, LI);
        }
        if (fusionSuccess) {
          outs() << "Loops successfully fused\n";
          group.erase(group.begin() + baseIndex + 1);
          fused = true;
          removeUnreachableBlocks(F);

          siblings.erase(
              std::remove(siblings.begin(), siblings.end(), nextLoop),
              siblings.end());

          DT.recalculate(F);
          PDT.recalculate(F);
        } else {
          outs() << "Error while trying to fuse\n";
          baseIndex++;
          baseLoop = group[baseIndex];
        }
      }
    }

    bool childrenFused = false;
    for (Loop *L : siblings) {
      std::vector<Loop *> children = L->getSubLoopsVector();
      if (processNestLevelLoops(children, DT, PDT, SE, LI, F)) {
        childrenFused = true;
      }
    }

    return fused || childrenFused;
  }

  /**
   * @brief separates instructions from the latch, we use it especially for
   * while loops to make sure instruction inserted in the first loop latch are
   * not put after the second loop body when fused
   *
   * @param L
   * @param DT
   * @param LI
   */
  void prepareLoopLatch(Loop *L, DominatorTree &DT, LoopInfo &LI) {
    auto latch = L->getLoopLatch();
    auto header = L->getHeader();
    if (latch == header || latch->size() > 2) {
      // latch terminator is inserted in a different block
      latch = SplitBlock(latch, latch->getTerminator(), &DT, &LI);
    }
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);

    for (auto L : LI.getLoopsInPreorder()) {
      if (!L->isLoopSimplifyForm())
        continue;
      auto backedgeLoop = SE.getBackedgeTakenCount(L);
      loopsTripCountMap[L] = backedgeLoop;
      prepareLoopLatch(L, DT, LI);
    }

    bool changed =
        processNestLevelLoops(LI.getTopLevelLoopsVector(), DT, PDT, SE, LI, F);

    return (changed ? PreservedAnalyses::none() : PreservedAnalyses::all());
  }

  static bool isRequired() { return true; }
};
} // namespace

llvm::PassPluginLibraryInfo getLoopPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopFusion", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "LF") {
                    FPM.addPass(LoopFusion());
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
