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
bool areConditionsEquivalent(ScalarEvolution &SE, BranchInst *l1GuardCond,
                               BranchInst *l2GuardCond) {

    //prendo le condizioni dei due branch condizionali che proteggono i due loop. Se sono lo stesso oggetto, allora sono equivalenti.

    Value *Cond1 = l1GuardCond->getCondition();
    Value *Cond2 = l2GuardCond->getCondition();

    if (Cond1 == Cond2)
      return true;

    //casto le condizioni a ICmpInst (istruzioni di confronto intero). Se non sono confronti interi, ritorno false.
    auto *Cmp0 = dyn_cast<ICmpInst>(Cond1); 
    auto *Cmp1 = dyn_cast<ICmpInst>(Cond2);
    if (!Cmp0 || !Cmp1)
      return false;

    /*
    prendo l'operando sinistro e destro di ciascun confronto e li trasformo in SCEV (Scalar Evolution Expressions) per poterli confrontare.
    */
    const SCEV *LHS0 = SE.getSCEV(Cmp0->getOperand(0));
    const SCEV *RHS0 = SE.getSCEV(Cmp0->getOperand(1));
    const SCEV *LHS1 = SE.getSCEV(Cmp1->getOperand(0));
    const SCEV *RHS1 = SE.getSCEV(Cmp1->getOperand(1));

    //oltre agli operandi prendo l'operatore del confronto (es. <, >, ==) per poter confrontare anche quello.
    auto Pred0 = Cmp0->getPredicate();
    auto Pred1 = Cmp1->getPredicate();

    if (Pred0 == Pred1 && LHS0 == LHS1 && RHS0 == RHS1)
      return true;

    /*
    se le condizioni non sono equivalenti, provo a scambiare l'operatore del secondo confronto e a invertire gli operandi.
    */
    Pred1 = ICmpInst::getSwappedPredicate(Cmp1->getPredicate());
    if (Pred0 == Pred1 && LHS0 == RHS1 && RHS0 == LHS1)
      return true;

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
   /*
   La funzione verifica se due loop L1 e L2 sono adiacenti, cioè se non ci sono basic block "di mezzo" con istruzioni non spostabili. 
   Se ci sono istruzioni mobili tra i loop, le raccoglie in toMoveBeforeL1 o toMoveAfterL2 per poterle spostare in seguito (prerequisito per la loop fusion).
   */
  bool areAdjacent(ScalarEvolution &SE, Loop *L1, Loop *L2, SetVector<Instruction *> &toMoveBeforeL1,
                   SetVector<Instruction *> &toMoveAfterL2) {

    /*
        Recupera l'exit block di L1 e l'entry block di L2. Pulisce i set di istruzioni da spostare. 
    */
    BasicBlock *ExitL1 = getLoopExit(L1);
    BasicBlock *EntryL2 = getLoopEntry(L2);
    toMoveBeforeL1.clear();
    toMoveAfterL2.clear();
    
    //Controlla se L1 è guarded (ha un branch condizionale che protegge l'ingresso al loop).
    bool isGuarded = L1->isGuarded();

    //Se uno dei due blocchi non esiste -> impossibile procedere.
    if (!ExitL1 || !EntryL2)
      return false;

    // both must be guarded or unguarded
    //Entrambi i loop devono essere dello stesso tipo (entrambi guarded o entrambi no).
    if (isGuarded != L2->isGuarded())
      return false;

    // if guarded, both must be equivalent
    //Se guarded, le condizioni di guardia devono essere equivalenti (altrimenti la fusion cambierebbe la semantica).
    if (isGuarded && !areConditionsEquivalent(SE, L1->getLoopGuardBranch(),
                                              L2->getLoopGuardBranch())) {
      return false;
    }

    //Il terminatore dell'exit block deve essere un BranchInst. Se non lo è (es. è un ReturnInst), i loop non sono fusibili.
    BranchInst *BI1 = dyn_cast<BranchInst>(ExitL1->getTerminator());

    if (!BI1)
      return false;

    // first case, exit and entry correspond
    // Caso 1: Exit di L1 coincide con Entry di L2
    if (ExitL1 == EntryL2) {

      // if there's an instruction or more between the loops, try to move
      // it/them
      /*
        Se il blocco condiviso è vuoto -> adiacenti subito. Se ha istruzioni -> prova a capire se sono spostabili. Se non lo sono -> non adiacenti.
      */
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

    // Caso 2: Exit e Entry sono blocchi distinti
    BranchInst *BI2 = dyn_cast<BranchInst>(EntryL2->getTerminator());
    if (!BI2 || !BI1->isUnconditional())  //Per procedere, BI1 deve essere unconditional (L1 esce sempre nello stesso posto) e EntryL2 deve avere anch'esso un branch.
      return false;

    // a redundant "trampoline" block could be in the middle
    /*
      Controllo del "trampoline block": Potrebbe esserci un blocco intermedio vuoto ("trampoline") tra ExitL1 e EntryL2:
      Questo è accettabile solo se NextBB ha un branch unconditional verso EntryL2 ed è completamente vuoto. Altrimenti -> non adiacenti.
    */
    BasicBlock *NextBB = BI1->getSuccessor(0);  //il successore del branch di ExitL1
    if (NextBB != EntryL2) {
      BranchInst *NextBI = dyn_cast<BranchInst>(NextBB->getTerminator());
      if (!NextBI || !NextBI->isUnconditional() ||
          NextBI->getSuccessor(0) != EntryL2 ||
          !isBlockEmpty(NextBB, NextBI, false)) {
        return false;
      }
      
    }

    // finally we check for instructions between ExitL1 and EntryL2
    /*
      Verifico che né ExitL1 né EntryL2 contengano istruzioni non spostabili. 
      Nota che qui vengono passati entrambi i blocchi a canMoveInstructions, a differenza del caso 1 dove si passa solo ExitL1
       perché ora i blocchi di mezzo sono due distinti.
    */
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
   /*
    La funzione determina se le istruzioni che si trovano tra due loop L1 e L2 possono essere spostate fuori dalla loro posizione attuale,
     per rendere i loop adiacenti e quindi fusibili.
    Il problema: per fondere L1 e L2, non ci deve essere nulla in mezzo. Se c'è del codice intermedio, 
    va spostato o prima di L1 o dopo L2. Se anche una sola istruzione non è spostabile né da un lato né dall'altro, la fusione non è fattibile.
   */
  bool canMoveInstructionsInBetweenLoops(
      Loop *L1, Loop *L2, BasicBlock *ExitL1, BranchInst *BI1,
      SetVector<Instruction *> &toMoveBeforeL1,
      SetVector<Instruction *> &toMoveAfterL2, BasicBlock *EntryL2 = nullptr,
      BranchInst *BI2 = nullptr) {  //Gli passo l'exit block del primo loop e l'entry block del secondo loop, così da poter controllare le istruzioni tra i due loop

    // salvo tutte le istruzioni tra i due loop in un vettore, così da poterle controllare una ad una
    std::vector<Instruction *> toCheckForCodeMotion;

    // controllo le istruzioni nel blocco di uscita del primo loop, escludendo le PHI nodes, il branch e le istruzioni di debug
    for (Instruction &I : *ExitL1) {
      if (!isa<PHINode>(&I) && &I != BI1 && !isa<DbgInfoIntrinsic>(&I)) {
        toCheckForCodeMotion.push_back(&I);
      }
    }

    // I don't need to check for EntryL2 != ExitL1 as I do that in areAdjacent
    // before calling this function, if they're equal then I don't give EntryL2
    // as a parameter and it will be nullptr meaning I just need to check that
    // instead (should be equivalent either way)

    /* Faccio la stessa cosa per il blocco di entrata del secondo loop, escludendo le PHI nodes, il branch e le istruzioni di debug.
    Non controllo che EntryL2 sia diverso da ExitL1 perché lo faccio in areAdjacent prima di chiamare questa funzione, 
    se sono uguali allora non passo EntryL2 come parametro e sarà nullptr, quindi controllerò solo quello (dovrebbe essere equivalente in entrambi i casi)
    */
    if (EntryL2 != nullptr && EntryL2 != L2->getHeader()) {
      for (Instruction &I : *EntryL2) {
        if (!isa<PHINode>(&I) && &I != BI2 && !isa<DbgInfoIntrinsic>(&I)) {
          toCheckForCodeMotion.push_back(&I);
        }
      }
    }

    // controllo ogni istruzione tra i due loop per vedere se può essere spostata prima di L1 o dopo L2, o se non può essere spostata affatto
    for (Instruction *I : toCheckForCodeMotion) {
      // inizializzo due booleani per tenere traccia se l'istruzione deve essere spostata prima di L1 o dopo L2
      bool neededBeforeL1 = false;
      bool neededAfterL2 = false;

      // se l'istruzione ha side effects o legge/scrive in memoria, non può essere spostata
      if (I->mayHaveSideEffects() || I->mayReadOrWriteMemory()) {
        return false;
      }
      // checks if the instruction is needed after L2
      // if it is then we are forced to move it before L1

      // Se il risultato di I è usato dentro L2 -> L'istruzione deve essere disponibile prima che L2 inizi -> va messa prima di L1 (che precede L2).
      if (isUsedInLoop(I, L2, false))
        neededBeforeL1 = true;

      // checks that the instruction is using something from the previous loop,
      // L1, if it is then we are forced to move it after L2

      // Se un operando di I è definito dentro L1 -> L'istruzione dipende da un valore prodotto da L1, 
      // quindi può essere eseguita solo dopo che L1 termina -> va messa dopo L2.
      for (Value *Op : I->operands()) {
        if (isDefinedInLoop(Op, L1))
          neededAfterL2 = true;
        // Propagazione delle dipendenze tra istruzioni intermedie: se I dipende da un'altra istruzione già classificata come "va dopo L2",
        // allora anche I deve andare dopo L2 (e viceversa). Serve a mantenere la coerenza dell'ordine tra istruzioni che dipendono l'una dall'altra.
        else if (auto *OpInst = dyn_cast<Instruction>(Op)) {
          if (toMoveAfterL2.count(OpInst))
            neededAfterL2 = true;
          else if (toMoveBeforeL1.count(OpInst))
            neededBeforeL1 = true;
        }
        // edge case, the instruction could be using a phi node contained within
        // the same block, so we cannot move the instruction safely
        // Edge case: se I usa un PHINode dello stesso blocco, non si può spostare perché il PHI è legato alla struttura del blocco corrente.
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

      // Se si scopre che dobbiamo spostare l'istruzione sia dopo L2 che
      // prima di L1 a causa delle sue dipendenze, allora non possiamo spostarla affatto.
      // Se troviamo anche solo un'istruzione che non può essere spostata, possiamo
      // fermarci del tutto, poiché la fusione dei cicli non sarà fattibile a meno che non vengano spostate tutte le
      // istruzioni. Abbiamo deciso di inizializzare i booleani come true e
      // renderli false nel codice precedente per evitare un controllo if. Inizializzandoli
      // come true e rendendoli false in tali condizioni, possiamo semplicemente
      // controllare quando ci sono dipendenze da entrambi i cicli e poi solo
      // da uno dei due. Invece di partire da false -> true, avremmo avuto questo
      // controllo con entrambi come true, i controlli per entrambi singolarmente come
      // singolo booleano true, e se non ci sono dipendenze da nessuno dei due lati
      // allora entrambi sarebbero false, richiedendo un quarto controllo. In questo modo utilizziamo
      // la logica booleana per evitare un controllo superfluo.
      if (neededBeforeL1 && neededAfterL2)
        return false;

      // Altrimenti si inserisce l'istruzione nel set appropriato. Il default (nessuna dipendenza da nessun loop) è spostarla prima di L1, scelta conservativa.
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

    /*prendo il blocco di entrata di L1 e muovo le istruzioni da spostare prima di L1, 
    e prendo il blocco di uscita di L2 e muovo le istruzioni da spostare dopo L2
    */
    BasicBlock *EntryL1 = getLoopEntry(L1);
    if (EntryL1) {
      Instruction *whereToMoveTo = EntryL1->getTerminator();  //le metto prima dell'ultima istruzione del blocco 
      for (Instruction *I : toMoveBeforeL1)
        I->moveBefore(whereToMoveTo);
    }

    BasicBlock *ExitL2 = getLoopExit(L2);
    if (ExitL2) {
      Instruction *whereToMoveTo = ExitL2->getFirstNonPHI();  //le metto prima della prima istruzione non PHI del blocco, perchè la phi è all'inizio del blocco
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
    // Ottengo le SCEV (Scalar Evolution Expressions) che rappresentano il numero di iterazioni dei loop L1 e L2, 
    // grazie alla mappa loopsTripCountMap che associa ogni loop alla sua SCEV.
    const SCEV *TC1 = loopsTripCountMap[L1];
    const SCEV *TC2 = loopsTripCountMap[L2];

    // Se una delle due SCEV non può essere calcolata (ad esempio, se il numero di iterazioni non è determinabile staticamente), 
    // ritorno false, indicando che non possiamo garantire che i due loop abbiano lo stesso numero di iterazioni.
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

    // Ottengo i blocchi di ingresso dei due loop. Questi blocchi rappresentano il punto in cui l'esecuzione entra nel loop.
    auto Pre1 = getLoopEntry(L1);
    auto Pre2 = getLoopEntry(L2);

    // Se uno dei due blocchi di ingresso non esiste, ritorno false, indicando che i loop non sono equivalenti dal punto di vista del flusso di controllo.
    if (!Pre1 || !Pre2) {
      return false;
    }

    // Controllo se Pre1 domina Pre2 e se Pre2 domina Pre1 (post dominanza). 
    // Se entrambe le condizioni sono vere, significa che i due blocchi di ingresso sono equivalenti dal punto di vista del flusso di controllo.
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

  /*
  controllo che le istruzioni siano istruzioni di memoria (load/store)
  */

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

      /*
      Controllo i 3 casi principali che potrebbero causarmi problemi;

      Il caso read after read non è un problema poichè anche se ci potrebbe essere dipendenza negativa, non ha impatto negativo sulla memoria poichè
      non modifico niente, non è distruttivo, non sovrascrivo valori usati da altri.
      */

      bool isWAR = I1->mayReadFromMemory() && I2->mayWriteToMemory(); //il primo loop legge un valore che il secondo loop può sovrascrivere, quindi dipendenza negativa
      bool isRAW = I1->mayWriteToMemory()  && I2->mayReadFromMemory();  // il secondo loop legge un valore che non è ancora stato scritto dal primo loop, quindi dipendenza negativa
      bool isWAW = I1->mayWriteToMemory() && I2->mayWriteToMemory();  //il primo loop può sovrascrivere il valore scritto dal secondo loop, quindi dipendenza negativa


      if (!isWAR && !isRAW && !isWAW)
        continue;

        //prendo gli operandi della mia istruzione che rappresentano i puntatori (quello che viene letto o scritto in memoria)
      Value *Ptr1 = getPointerOperand(I1);
      Value *Ptr2 = getPointerOperand(I2);

      if (!Ptr1 || !Ptr2)
        return true;

      if (getUnderlyingObject(Ptr1) != getUnderlyingObject(Ptr2)) // controlla che sia lo stesso array, se non lo è allora non c'è dipendenza negativa
        continue;

      /*
        Una SCEV di un puntatore che varia dentro un loop viene rappresentata da LLVM come una SCEVAddRecExpr:
        { Start, +, Stride }<Loop>, ovvero al passo i-esimo del loop il puntatore punta a Start + i*Stride.
        Start = il valore del puntatore alla prima iterazione (i=0)
        Stride = di quanto avanza il puntatore ad ogni iterazione

        Per ogni coppia (I1 appartiene a L1, I2 appartiene a L2) che accede allo stesso array base, estraiamo la SCEV del puntatore di ciascuno 
        e ne prendiamo start e stride:
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
      //      Se start2 > start1 -> L2 legge avanti rispetto a L1
      //      -> dipendenza loop-carried negativa -> blocca.
      // WAR: L1 legge start1+i, L2 scrive start2+i.
      //      Se start1 > start2 -> L1 legge avanti rispetto a L2 -> blocca.
      if (Stride1 && Stride2 && Stride1 == Stride2) {
        const SCEV *diffStart = isRAW || isWAW
            ? SE.getMinusSCEV(Start2, Start1)
            : SE.getMinusSCEV(Start1, Start2);

        if (SE.isKnownPositive(diffStart))
          return true;
      } else {
        // Stride diversi o non noti: non possiamo provare assenza di hazard
        /*
        Se gli stride sono diversi: a me va bene soltanto se i due start sono uguali
        */
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
   // La funzione verifica se ci sono dipendenze scalari tra due loop L1 e L2. 
   // In altre parole, controlla se qualche istruzione in L1 produce un valore che viene utilizzato in L2. 
   // Se esiste almeno una tale dipendenza, la funzione restituisce true, 
   // indicando che i loop non possono essere fusi senza rischiare di alterare il comportamento del programma.
  bool hasScalarDependencies(Loop *L1, Loop *L2) {
    for (BasicBlock *BB : L1->getBlocks()) {
      for (Instruction &I : *BB) {
        // Controlla se l'istruzione I è utilizzata in L2. Se sì, significa che c'è una dipendenza scalare tra L1 e L2.
        if (isUsedInLoop(&I, L2, false))
          return true;
      }
    }
    return false;
  }

  /*
  * @brief checks if a loop is a do-while loop
  *
  * @param L1
  * @return true
  * @return false
  */
  bool isLoopDoWhile(Loop *L1) {
    // Controllo che il terminatore del blocco header del loop sia un'istruzione di branch.
    // Se sì, verifico se il numero di successori del branch è minore o uguale a 1. 
    // Poichè un do-while loop ha un solo percorso di uscita, se il numero di successori è 1 o meno, allora il loop è considerato un do-while.
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
   // La funzione aggiorna i nodi PHI in un blocco di base (TargetBB) sostituendo il predecessore OldPred con il nuovo predecessore NewPred.
   // lo faccio perchè quando fonderò due loop, i blocchi che prima puntavano al latch del secondo loop dovranno ora puntare al latch del primo loop, 
   // quindi devo aggiornare i PHI nodes di conseguenza.
  void updatePhiNodes(BasicBlock *TargetBB, BasicBlock *OldPred,
                      BasicBlock *NewPred) {
    // Itero su tutti i nodi PHI nel blocco di base TargetBB.
    for (PHINode &PN : TargetBB->phis()) {
      // Ottengo l'indice del blocco OldPred nel nodo PHI. Se OldPred non è un predecessore del nodo PHI, l'indice sarà negativo.
      int blockIndex = PN.getBasicBlockIndex(OldPred);
      if (blockIndex >= 0) {
        PN.setIncomingBlock(blockIndex, NewPred); // Sostituisco il predecessore OldPred con NewPred nel nodo PHI.
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
   /*IN GENERALE:
   Intanto la nostra fusione consiste idealmente nel prendere il body del primo loop, farlo puntare al body del secondo loop, poi far puntare il body del secondo loop
   al latch del primo loop ed eliminare i blocchi morti del secondo e aggiornare i blocchi del primo.

   fuseLoops esegue la fusione fisica di due loop L1 e L2 nell'IR di LLVM. 
   Dopo che tutte le verifiche di fattibilità sono state superate (trip count uguale, adiacenza, dipendenze ok), 
   ricollega i BasicBlock, aggiorna i PHI node, e aggiorna la struttura di LoopInfo
   */
  bool fuseLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    // Ottengo i blocchi di intestazione, i latch e le variabili di induzione dei due loop L1 e L2.
    // le variabili di induzione sono le variabili che vengono incrementate ad ogni iterazione del loop e determinano il numero di iterazioni del loop stesso.
    auto L1Header = L1->getHeader();
    auto L1HeaderTerminator = L1Header->getTerminator();
    auto L1InductionVar = L1->getCanonicalInductionVariable();  //canoninca significa che parte da 0 e incrementa di 1 ad ogni iterazione, quindi è la più semplice da gestire
    auto L1Latch = L1->getLoopLatch();
    auto L1ExitBlock = getLoopExit(L1);

    auto L2Header = L2->getHeader();
    auto L2HeaderTerminator = L2Header->getTerminator();
    auto L2ExitBlock = getLoopExit(L2);
    auto L2EntryBlock = getLoopEntry(L2);
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2Latch = L2->getLoopLatch();

    // Controllo se le variabili di induzione dei due loop sono valide. 
    // Se una delle due non è valida, stampo un messaggio di errore e ritorno false,
    //  indicando che la fusione dei loop non può essere eseguita.
    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    // Salvo i predecessori dei latch dei due loop in due vettori separati. Così posso aggiornare i loro successori più tardi, quando fonderò i loop.
    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());


    // Ottengo il blocco di base che rappresenta l'inizio del corpo del secondo loop (L2).
    // L'header di un loop ha due successori: il body e il blocco di uscita. Questo codice identifica quale dei due è il body, 
    // indipendentemente dall'ordine in cui LLVM li ha messi (il branch può essere true=body/false=exit o viceversa).
    BasicBlock *L2BodyEntry =
        (L2HeaderTerminator->getSuccessor(0) == L2ExitBlock)  // Se il primo successore del terminatore dell'header di L2 è il blocco di uscita di L2, allora il corpo del loop inizia con il secondo successore, altrimenti inizia con il primo successore.
            ? L2HeaderTerminator->getSuccessor(1)
            : L2HeaderTerminator->getSuccessor(0);

    // we manage the phi nodes present in the second header and the induction
    // var make_early_inc_range increments before, so we are sure to not
    // invalidate the pointer

    /*
    I PHI node dell'header di L2 devono essere gestiti caso per caso.
    Gestisco i nodi PHI presenti nell'header del secondo loop (L2).
    Uso make_early_inc_range per iterare sui nodi PHI, in modo da non invalidare il puntatore durante la rimozione dei nodi PHI.
    make_early_inc_range incrementa l'iteratore prima di processare l'elemento corrente, rendendo sicura la rimozione durante l'iterazione.
    */
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      /*
       Se il nodo PHI corrente è la variabile di induzione del secondo loop, lo sostituisco con la variabile di induzione del primo loop (L1) e lo rimuovo.
       L2 non ha più bisogno di una propria induction variable: tutti i suoi usi vengono rimpiazzati con quella di L1, poi il PHI viene eliminato.
      */
      if (&PN == L2InductionVar) {
        // induction var is replace by the one in the first loop
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        // other phi nodes are moved in L1Header after the others
        /*
             Gli altri PHI node vengono spostati fisicamente nell'header di L1. 
             I loro incoming block (quelli del phi che mettono il valore dipendentemente da che blocco arrivi) vanno aggiornati: 
             il predecessore che era L2EntryBlock diventa L1EntryBlock, 
             e quello che era L2Latch diventa L1Latch, perché ora il ciclo passa dal latch di L1.
        */
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
    /*
      L'header di L1 non deve più saltare al proprio exit block (che sparisce nella fusione), 
      ma all'exit block di L2 -> che diventa l'unico exit del loop fuso. Si aggiornano anche i PHI node del nuovo exit block: 
      chi riceveva valori da L2Header ora li riceve da L1Header.
    */
    if (L1HeaderTerminator->getSuccessor(0) == L1ExitBlock) {
      L1HeaderTerminator->setSuccessor(0, L2ExitBlock);
    } else {
      L1HeaderTerminator->setSuccessor(1, L2ExitBlock);
    }
    updatePhiNodes(L2ExitBlock, L2Header, L1Header);

    // the blocks in L1 that pointed to the L1 Latch now go to the body of L2

    /*
    Ora collego il body del loop 1 al body del loop 2
    I blocchi del body di L1 che prima saltavano al latch di L1 (fine iterazione) ora saltano al body entry di L2. 
    In questo modo le due sequenze di body vengono concatenate in serie.
    */
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
    /*
      Collego il body di L2 al latch di L1.
      Simmetrico al passo precedente: i blocchi di L2 che puntavano al latch di L2 ora puntano al latch di L1, che è il latch del loop fuso. 
      I PHI node del latch di L1 vengono aggiornati per accettare i nuovi predecessori.
    */
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

    //Se L2 conteneva loop annidati, questi vengono trasferiti a L1 nella struttura di LoopInfo. 
    // Senza questo passo la gerarchia dei loop sarebbe corrotta.
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector(); // tutti i subloop di L2 vengono salvati in un vettore, poi vengono rimossi da L2 e aggiunti a L1.

    // should manage the subloops
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    /* Should move blocks that belong to L2 to L1, except the header and the
     latch */
     /* Spostamento fisico dei blocchi
      Tutti i blocchi di L2 (esclusi header e latch, che vengono abbandonati) vengono spostati formalmente dentro L1 nella struttura di LoopInfo. 
      Il CFG è già stato ricablato nei passi precedenti; questo aggiorna solo i metadati.
     */
    std::vector<BasicBlock *> blocksToMove(L2->block_begin(), L2->block_end());
    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Header && BB != L2Latch) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    // L2 non esiste più come loop: viene rimosso dal suo parent (che sia il top-level o un loop esterno). 
    // Dopo questo punto L2 è un oggetto orfano che verrà deallocato da LLVM.
    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

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
   // Un Loop Guarded ha un branch prima del preheader che salta il loop intero se il trip count è zero
  bool fuseGuardedLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1Latch = L1->getLoopLatch();
    auto L1Preheader = L1->getLoopPreheader();
    auto L1InductionVar = L1->getCanonicalInductionVariable();

    auto L2Header = L2->getHeader();
    auto L2Latch = L2->getLoopLatch();
    auto L2Preheader = L2->getLoopPreheader();
    auto L2InductionVar = L2->getCanonicalInductionVariable();

    //Guardia
    auto L2GuardBr = L2->getLoopGuardBranch();
    auto L2GuardBB = L2GuardBr->getParent();

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    
    // the exit of the second loop guard
    /*L2BodyEntry = L2Header invece di calcolare il successore dell'header. 
    Nei loop guardati l'header è già il primo blocco del body (non c'è un check di condizione separato nell'header come nei loop normali).
    */
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
    /*Rimuovo la guardia:
      Il guard di L2 viene bypassato completamente: chi ci puntava ora va direttamente a L2Bypass. 
      Il check non serve più perché L1 ha già le stesse condizioni (trip count uguale).
      PHI node: entryIdx usa L2Preheader invece di L2EntryBlock come incoming block da rimpiazzare con L1Preheader.
      Migrazione blocchi: esclude anche L2GuardBB e L2Preheader (oltre al latch) dal trasferimento a L1.
    */
    std::vector<BasicBlock *> L2GuardPreds(predecessors(L2GuardBB).begin(),
                                           predecessors(L2GuardBB).end());
    for (BasicBlock *Pred : L2GuardPreds) {
      auto *Term = Pred->getTerminator();
      for (unsigned i = 0; i < Term->getNumSuccessors(); i++) {
        if (Term->getSuccessor(i) == L2GuardBB) {
          Term->setSuccessor(i, L2Bypass);  // chi puntava al guard ora bypassa
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
      if (LI.getLoopFor(BB) == L2) {  // prende solo i blocchi che appartengono a L2(lo hanno come padre), escludendo eventuali subloop, perchè li ho già spostati prima.
        blocksToMove.push_back(BB);
      }
    }

    /*
      Muovo i blocchi di L2 (esclusi L2Latch, L2Preheader e L2GuardBB) dentro L1 nella struttura di LoopInfo.
    */
    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Latch && BB != L2Preheader && BB != L2GuardBB) {  //
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    //LI.erase(L2);
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
   // Un loop do-while non ha un header con branch condizionale all'ingresso -> il check è nel latch
  bool fuseDoWhile(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1Latch = L1->getLoopLatch();
    auto L1Preheader = L1->getLoopPreheader();
    auto L1InductionVar = L1->getCanonicalInductionVariable();

    // Ottengo il branch del latch di L1, che è dove avviene il check della condizione del loop do-while.
    auto L1LatchBr = dyn_cast<BranchInst>(L1Latch->getTerminator());

    auto L2Header = L2->getHeader();
    auto L2Latch = L2->getLoopLatch();
    auto L2Preheader = L2->getLoopPreheader();
    auto L2InductionVar = L2->getCanonicalInductionVariable();

    // Ottengo il branch del latch di L2, che è dove avviene il check della condizione del loop do-while.
    auto L2LatchBr = dyn_cast<BranchInst>(L2Latch->getTerminator());

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    // the exit of the second loop guard
    //  L2Bypass calcolato dal latch di L2, non dal guard: Il successore del latch che non è l'header è l'uscita del loop.
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
    /*
      Redirect dell'exit tramite il latch di L1, non l'header: 
      Nei do-while è il latch che decide se continuare o uscire, quindi è lì che va aggiornato il successore di uscita, non nell'header come in fuseLoops.
      Nessun guard block da eliminare -> i do-while non hanno guard.
      Migrazione blocchi: esclude solo L2Latch e L2Preheader, non un L2GuardBB
    */
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
  bool processNestLevelLoops(std::vector<Loop *> siblings, // loop dello stesso livello e anche dello stesso scope
                           DominatorTree &DT,
                           PostDominatorTree &PDT, ScalarEvolution &SE,
                           LoopInfo &LI, Function &F,
                           bool isTopLevel = false) {
  std::vector<Loop *> candidateLoops;
  bool fused = false;

  for (Loop *L : siblings) {
    if (L->isLoopSimplifyForm()) {
      candidateLoops.push_back(L);
    }
  }

  std::vector<std::vector<Loop *>> cfeGroups;
  if (candidateLoops.size() >= 2) {

    std::sort(candidateLoops.begin(), candidateLoops.end(), //li riordino dal primo all'ultimo
              [&](Loop *L1, Loop *L2) {
                return DT.dominates(getLoopEntry(L1), getLoopEntry(L2));  //controllo dominanza tra i due loop, se L1 domina L2 allora L1 viene prima di L2
              });

    //faccio dei gruppi di loop cfe(control flow equivalent), così sono sicuro che loop dello stesso gruppo vengono eseguiti insieme
    for (auto &loop : candidateLoops) {
      bool addedToGroup = false;
      for (auto &group : cfeGroups) {
        if (areControlFlowEquivalent(group.front(), loop, DT, PDT)) {
          group.push_back(loop);
          addedToGroup = true;
          break;
        }
      }
      if (!addedToGroup) {  //creo un nuovo gruppo se non è stato aggiunto a nessun altro, poichè inizia un altro gruppo cfe diverso
        cfeGroups.push_back({loop});
      }
    }
  }

  //controllo che i gruppi cfe abbiano almeno 2 loop, altrimenti non ha senso provare a fonderli
  for (auto &group : cfeGroups) {
    if (group.size() >= 2)
      outs() << "found CFE with size " << group.size() << "\n";
  }

  for (auto &group : cfeGroups) {
    int baseIndex = 0;
    /* per ogni gruppo faccio delle coppie, il base loop è il primo della coppia mentre il next loop è il secondo, 
    se riesco a fonderli allora il base loop rimane e il next loop viene rimosso dal gruppo, altrimenti passo alla coppia successiva
    poi itero di nuovo e come base loop rimane il primo della coppia e lo confronto col terzo loop creando una nuova coppia
    Altrimenti se non riesco a fonderli come base loop metto il loop successivo
    
    */
    while (group.size() >= 2 && baseIndex < (int)group.size() - 1) {
      // Leggi i puntatori freschi ogni iterazione -- NO reference
      Loop *baseLoop = group[baseIndex];
      Loop *nextLoop = group[baseIndex + 1];

      //Applico i controlli per vedere se posso fondere i due loop, se uno dei controlli fallisce passo alla coppia successiva

      if (!hasSameTripCount(baseLoop, nextLoop)) {
        outs() << "Failed : Could not compute or different trip counts\n";
        baseIndex++;
        continue;
      }
      if (hasNegativeDependencies(baseLoop, nextLoop, SE)) {
        outs() << "Failed: negative dependencies found\n";
        baseIndex++;
        continue;
      }
      if (hasScalarDependencies(baseLoop, nextLoop)) {  //controllo extra
        outs() << "Failed: scalar dependencies found\n";
        baseIndex++;
        continue;
      }

      /*
        sono istruzioni che dobbiamo spostare prima del primo loop e dopo il secondo loop, così da non avere istruzioni tra i due loop che impediscono la fusione
      */
      SetVector<Instruction *> toMoveBeforeL1;
      SetVector<Instruction *> toMoveAfterL2;

      if (!areAdjacent(SE,baseLoop, nextLoop, toMoveBeforeL1, toMoveAfterL2)) {
        outs() << "Failed: loops are not adjacent\n";
        baseIndex++;
        continue;
      }

      //CONTROLLO EXTRA
      // se sono adiacenti e ci sono istruzioni da spostare, le sposto 
      if (!toMoveAfterL2.empty() || !toMoveBeforeL1.empty()) {
        moveInstructionsInBetweenLoops(baseLoop, nextLoop, toMoveBeforeL1,
                                       toMoveAfterL2);
      }

      //QUA FONDO I LOOPS
      //Qua controllo i diversi tipi di loop, se sono guarded o do while, così da usare la funzione di fusione corretta
      outs() << "All checks completed, trying to fuse...\n";
      bool fusionSuccess = false;
      if (baseLoop->isGuarded()) {
        fusionSuccess = fuseGuardedLoops(baseLoop, nextLoop, LI);
      } else if (isLoopDoWhile(baseLoop) && isLoopDoWhile(nextLoop)) {
        fusionSuccess = fuseDoWhile(baseLoop, nextLoop, LI);
      } else {
        fusionSuccess = fuseLoops(baseLoop, nextLoop, LI);
      }

      // se ho fuso con successo, rimuovo il next loop dal gruppo e non incremento baseIndex, così da confrontare il base loop con il nuovo next loop
      if (fusionSuccess) {
        outs() << "Loops successfully fused\n";
        fused = true;

        group.erase(group.begin() + baseIndex + 1);

        removeUnreachableBlocks(F); //rimuove i blocchi non raggiungibili dopo la fusione, così da non avere blocchi morti nel CFG, tipo il preheader e l'header e il latch del secondo loop che non è più raggiungibile(però dipende dalla fusione che facciamo)
        DT.recalculate(F);
        PDT.recalculate(F);
        
      } else {
        outs() << "Error while trying to fuse\n";
        baseIndex++;
      }
    }
  }

  //itero ricorsivamente sui figli dei loop, così da provare a fondere anche i loop interni
  bool childrenFused = false;
  for (Loop *L : siblings) {
    std::vector<Loop *> children = L->getSubLoopsVector();  //loop interni al loop corrente
    if (children.size() >= 2) {
      if (processNestLevelLoops(children, DT, PDT, SE, LI, F, false)) {
        childrenFused = true;
      }
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
   //toglie le istruzioni dal latch (anche la compare(icmp) nel caso do while) se non sono il terminatore, così da evitare che vengano spostate dopo il secondo loop
  void prepareLoopLatch(Loop *L, DominatorTree &DT, LoopInfo &LI) { 
    auto latch = L->getLoopLatch();
    auto header = L->getHeader();

    // Se il latch è l'header o ha più di 2 istruzioni, significa che ci sono istruzioni tra il latch e il terminatore, quindi le separo in un nuovo blocco
    if (latch == header || latch->size() > 2) {
      // latch terminator is inserted in a different block
      latch = SplitBlock(latch, latch->getTerminator(), &DT, &LI);
    }
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    //Mi prendo gli handle delle analisi che mi servono per fare la fusione dei loop
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);

    // LI.getLoopsInPreorder() scorre tutti i loop in preorder, così da processare prima i loop esterni e poi quelli interni, anche se ci basta un qualsiasi ordine
    for (auto L : LI.getLoopsInPreorder()) {
      if (!L->isLoopSimplifyForm()) //loop in forma semplificata, sottoinsieme dei loop naturali, che hanno preheader e latch univoci e hanno uscite dedicate, e sono più facili da gestire
        continue;
      auto backedgeLoop = SE.getBackedgeTakenCount(L); // calcolo tutti i trip count dei loop prima di iniziare la fusione, così da non invalidare le SCEV durante la fusione
      loopsTripCountMap[L] = backedgeLoop;  //salva il trip count del loop in una mappa, così da poterlo usare per confrontarlo con altri loop
      prepareLoopLatch(L, DT, LI);  //vai alla funzione che prepara il latch del loop, separando le istruzioni dal latch se necessario
    }

    bool changed =
        processNestLevelLoops(LI.getTopLevelLoopsVector(), DT, PDT, SE, LI, F, true);

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
