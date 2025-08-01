#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <queue>
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "remove-dead-code"

using namespace llvm;

namespace
{
    bool isLocalAllocated(Value *ptr)
    {
        while (GetElementPtrInst *derived_ptr = dyn_cast<GetElementPtrInst>(ptr))
        {
            ptr = derived_ptr->getPointerOperand();
        }
        if (isa<AllocaInst>(ptr))
        {
            return true;
        }
        return false;
    }

    void getAllReachableBlocks(BasicBlock *startBlock, std::set<BasicBlock *> &reachableSet)
    { // find reachable blocks with BFS
        std::queue<BasicBlock *> workingQueue;
        for (BasicBlock *Successor : successors(startBlock))
        {
            workingQueue.emplace(Successor);
            reachableSet.emplace(Successor);
        }
        while (!workingQueue.empty())
        {
            BasicBlock *current_BB = workingQueue.front();
            workingQueue.pop();
            for (BasicBlock *Successor : successors(current_BB))
            {
                if (reachableSet.find(Successor) == reachableSet.end())
                {
                    reachableSet.emplace(Successor);
                    workingQueue.emplace(Successor);
                }
            }
        }
    }

    bool comparePtrOperand(MemoryLocation store_ptr, MemoryLocation accessed_ptr, AAResults &AA)
    {
        switch (AA.alias(store_ptr, accessed_ptr))
        {
        case AliasResult::MustAlias:
        case AliasResult::MayAlias: // fall through intentionally
            return true;
        default:
            return false;
        }
        return false;
    }

    bool accessedInBlock(Instruction *start_pos, StoreInst *to_check_store, AAResults &AA)
    {
        BasicBlock *parentBlock = start_pos->getParent();
        BasicBlock::iterator start(start_pos);
        for (BasicBlock::iterator i = start; i != parentBlock->end(); ++i)
        {
            Instruction *current = &(*i);
            if (LoadInst *load = dyn_cast<LoadInst>(current))
            {
                if (comparePtrOperand(MemoryLocation::get(to_check_store), MemoryLocation::get(load), AA))
                {
                    return true;
                }
            }
            else if (ReturnInst *ret = dyn_cast<ReturnInst>(current))
            {
                if (Value *ret_val = ret->getReturnValue())
                {
                    if (ret_val->getType()->isPointerTy())
                    {
                        if (comparePtrOperand(MemoryLocation::get(to_check_store), MemoryLocation::get(ret), AA))
                        {
                            return true;
                        }
                    }
                }
            }
            else if (CallBase *call = dyn_cast<CallBase>(current))
            {
                unsigned current_arg_index = 0;
                for (User::op_iterator current_arg = call->arg_begin(); current_arg != call->arg_end(); ++current_arg, ++current_arg_index)
                {
                    Value *current_arg_value = *current_arg;
                    if (current_arg_value->getType()->isPointerTy())
                    {
                        if (comparePtrOperand(MemoryLocation::get(to_check_store), MemoryLocation::getForArgument(call, current_arg_index, nullptr), AA))
                        {
                            return true;
                        }
                    }
                }
            }
            // may be should add more memory access instructions
        }
        return false;
    }

    bool isUsedAfterStore(StoreInst *to_check_store, AAResults &AA)
    {
        BasicBlock::iterator tmp(to_check_store);
        tmp++; // next instruction after to_check_store
        if (accessedInBlock(&(*tmp), to_check_store, AA))
        { // check if this location is accessed in the current block afterwards
            return true;
        }
        BasicBlock *Location_BB = to_check_store->getParent();
        std::set<BasicBlock *> reachableSet;
        getAllReachableBlocks(Location_BB, reachableSet);
        // starts checking reachable blocks
        for (BasicBlock *reachable_current : reachableSet)
        {
            Instruction *reachable_current_first_inst = &(*(reachable_current->begin()));
            if (accessedInBlock(reachable_current_first_inst, to_check_store, AA))
            {
                return true;
            }
        }
        return false;
    }

    bool isBBSimpleJump(BasicBlock *BB)
    {
        if (BB->size() == 1)
        {
            if (BranchInst *br_inst = dyn_cast<BranchInst>(&(*BB->begin())))
            {
                if (br_inst->isUnconditional() && !isa<PHINode>(*(br_inst->getSuccessor(0)->begin())))
                {
                    return true;
                }
            }
        }
        return false;
    }

    BasicBlock *trace_down_BB(BasicBlock *BB)
    {
        BasicBlock *currentBB = BB;
        std::set<BasicBlock *> visited_BB_SimpleJump_set;
        while (isBBSimpleJump(currentBB))
        {
            if (visited_BB_SimpleJump_set.find(currentBB) != visited_BB_SimpleJump_set.end())
            {
                return BB; // infinite simple jump loop, avoid infinite tracing
            }
            else
            {
                visited_BB_SimpleJump_set.emplace(currentBB);
                currentBB = (dyn_cast<BranchInst>(currentBB->getTerminator()))->getSuccessor(0);
            }
        }
        return currentBB;
    }

    bool TriviallyDeadInstructionElimination_internalpass(Function &F)
    {

        bool changed = false;
        for (BasicBlock &BB : F)
        {
            for (BasicBlock::reverse_iterator ri = BB.rbegin(); ri != BB.rend();)
            { // reverse delete for better performance
                Instruction *to_check = &(*ri);
                ++ri;
                if (isInstructionTriviallyDead(to_check))
                {
                    to_check->eraseFromParent(); // return value unusable, 'cause here we use reversed iterator
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool DeadStoreElimination_internalpass(Function &F, AAResults &AA) // pass AA result to this internal pass
    {
        bool changed = false;
        for (BasicBlock &BB : F)
        {
            for (BasicBlock::iterator i = BB.begin(); i != BB.end();)
            {
                Instruction *to_check = &(*i);
                if (StoreInst *to_check_store = dyn_cast<StoreInst>(to_check))
                {
                    Value *slot = to_check_store->getPointerOperand();
                    if (isLocalAllocated(slot) && !isUsedAfterStore(to_check_store, AA))
                    {
                        i = to_check->eraseFromParent();
                        changed = true;
                        continue;
                    }
                }
                ++i;
            }
        }
        return changed;
    }

    bool SimplifyBasicBlock_internalpass(Function &F)
    {
        bool changed = false;
        for (BasicBlock &BB : F)
        {
            Instruction *BB_Terminator = BB.getTerminator();
            if (BranchInst *current_branch = dyn_cast<BranchInst>(BB_Terminator))
            { // only check br instructions
                if (current_branch->isConditional())
                { // this branch is a conditional branch
                    BasicBlock *true_BB = current_branch->getSuccessor(0);
                    BasicBlock *false_BB = current_branch->getSuccessor(1);
                    if (true_BB == false_BB)
                    { // conditional branch to two same blocks
                        BranchInst *uncondBr = BranchInst::Create(true_BB);
                        uncondBr->insertBefore(current_branch);
                        uncondBr->copyMetadata(*current_branch);
                        current_branch->eraseFromParent();
                        changed = true;
                    }
                    else
                    { // conditional branch to different blocks
                        BasicBlock *new_true_BB = trace_down_BB(true_BB);
                        BasicBlock *new_false_BB = trace_down_BB(false_BB);
                        if (true_BB != new_true_BB || false_BB != new_false_BB)
                        {
                            // should change
                            BranchInst *condBr = BranchInst::Create(new_true_BB, new_false_BB, current_branch->getCondition());
                            condBr->insertBefore(current_branch);
                            condBr->copyMetadata(*current_branch);
                            current_branch->eraseFromParent();
                            changed = true;
                        }
                    }
                }
                else
                {
                    // this branch is an unconditional branch, traces down BBs
                    BasicBlock *new_jump_BB = trace_down_BB(current_branch->getSuccessor(0));
                    if (new_jump_BB != current_branch->getSuccessor(0))
                    {
                        // should change
                        BranchInst *uncondBr = BranchInst::Create(new_jump_BB);
                        uncondBr->insertBefore(current_branch);
                        uncondBr->copyMetadata(*current_branch);
                        current_branch->eraseFromParent();
                        changed = true;
                    }
                }
            }
        }
        return changed;
    }

    struct DeadCodeEliminationPass : PassInfoMixin<DeadCodeEliminationPass>
    {
        PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
        {
            bool changed = true;
            AAResults &AA = AM.getResult<AAManager>(F);
            while (changed)
            {
                changed = false;
                if (EliminateUnreachableBlocks(F))
                {
                    changed = true;
                }
                if (TriviallyDeadInstructionElimination_internalpass(F))
                {
                    changed = true;
                }
                if (SimplifyBasicBlock_internalpass(F))
                {
                    changed = true;
                }
                if (DeadStoreElimination_internalpass(F, AA))
                {
                    changed = true;
                }
            }
            if(isBBSimpleJump(&(F.getEntryBlock()))){
                (&(F.getEntryBlock()))->eraseFromParent();
            }
            return PreservedAnalyses::none();
        }
    };
} // namespace

/// Registration
PassPluginLibraryInfo getPassPluginInfo()
{
    const auto callback = [](PassBuilder &PB)
    {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, FunctionPassManager &FPM, auto)
            {
                if (Name == "dead-code-elimination")
                {
                    FPM.addPass(DeadCodeEliminationPass());
                    return true;
                }
                return false;
            });
    };
    return {LLVM_PLUGIN_API_VERSION, "DeadCodeEliminationPass",
            LLVM_VERSION_STRING, callback};
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return getPassPluginInfo();
}

#undef DEBUG_TYPE
