#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include <algorithm>
#include <llvm-19/llvm/IR/Analysis.h>

#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopIterator.h"

#include <unordered_map>
#include <unordered_set>
#include <map>
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
    bool isDeadAfterLoop(Instruction* I, Loop* LL) {
        for (User* U : I->users()) {
            Instruction* userInst = cast<Instruction>(U);
            BasicBlock* userBB = userInst->getParent();


            if (!LL->contains(userBB)) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief checks that the instruction is in a block that dominates every loop's exit
     * 
     * @param I 
     * @param LL 
     * @param DT 
     * @return true 
     * @return false 
     */
    bool dominatesExits(Instruction* I, Loop* LL, DominatorTree& DT){
        SmallVector<BasicBlock*> exitBlocks;
        LL->getExitBlocks(exitBlocks);

        bool dominates = true;
                        
        for(auto exitBlock : exitBlocks) {
            if(!DT.dominates(I->getParent(), exitBlock)){
                dominates = false;
                break; 
            }
        }

        return dominates;
    }

    /**
     * @brief debug printing for each loop, reporting loop invariant and moved instructions
     * 
     * @param invariantInstrs 
     * @param motionInstrs 
     */
    void printDebug(std::unordered_set<Instruction*>& invariantInstrs, std::vector<Instruction*>& motionInstrs){
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

        LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        bool modified = false;
        for (Loop *LL : LI.getLoopsInPreorder()) {
            if(!LL->isLoopSimplifyForm()) continue;
            
            std::unordered_set<Instruction*> invariantSet;
            std::vector<Instruction*> toMove;

            LoopBlocksRPO LBRPO(LL);
            LBRPO.perform(&LI);

            // Loop Invariant
            for (BasicBlock *BB : LBRPO) {
                for (Instruction& I : *BB) {
                    if (!isSafeToSpeculativelyExecute(&I)) continue;

                    if (I.getOpcode() == Instruction::PHI) continue;

                    bool isInvariant = true;
                    
                    for (Use &Op : I.operands()) {
                        Value *operandValue = Op.get();
                        
                        if (auto *op_instr = dyn_cast<Instruction>(operandValue)) {
                            if (LL->contains(op_instr->getParent()) && invariantSet.count(op_instr) == 0){
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
            std::unordered_set<Instruction*> isMoved;
            
            for (BasicBlock* BB : LBRPO) {
                for (Instruction& I : *BB) {
                    
                    if (invariantSet.count(&I) > 0 && (dominatesExits(&I, LL, DT) || isDeadAfterLoop(&I, LL))) {
                        bool depsMoved = true;
                        for (Use &Op : I.operands()) {
                            if (auto *op_instr = dyn_cast<Instruction>(Op.get())) {
                                if (LL->contains(op_instr->getParent()) && isMoved.count(op_instr) == 0) {
                                    depsMoved = false;
                                    break;
                                }
                            }
                        }

                        if (depsMoved) {
                            toMove.push_back(&I);
                            isMoved.insert(&I);   
                        }
                    }
                }
            }

            auto preheader = LL->getLoopPreheader();
            auto lastInstr = preheader->getTerminator();

            for(auto instr : toMove){
                instr->moveBefore(lastInstr);
                modified = true;
            }

            if(DEBUG){
                printDebug(invariantSet, toMove);
            }
        }
          
        if(modified)
            return PreservedAnalyses::none();

        return PreservedAnalyses::all();
    }



    static bool isRequired() { return true; }
}; 
}

llvm::PassPluginLibraryInfo getLoopPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopInvariantCodeMotion", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
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
