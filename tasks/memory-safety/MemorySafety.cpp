#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "MemorySafetyRuntime.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <string>
#include <vector>
#define DEBUG_TYPE "memory-safety"

using namespace llvm;

namespace
{
    struct MemorySafetyPass : PassInfoMixin<MemorySafetyPass>
    {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &MM)
        {
            declare_runtime_library_functions(M);
            if (has_function_main(M))
            {
                add_shadow_memory_initializer(M);
                add_shadow_memory_cleanup(M);
            }
            alloca_instrumentation_internal_pass(M);
            load_store_instrumentation_internal_pass(M);
            malloc_free_instrumentation_internal_pass(M);
            return PreservedAnalyses::all();
        }

        // do not skip this pass for functions annotated with optnone
        static bool isRequired() { return true; }

    private:
        void replace_malloc_calls(std::vector<CallInst *> &malloc_calls, Module &M)
        {
            for (CallInst *&malloc_call : malloc_calls)
            {
                //get size to malloc
                Value *malloc_size = malloc_call->getArgOperand(0);
                //get runtime malloc function ptr
                Function *runtime_malloc = M.getFunction("__runtime_malloc");
                //create IRBuilder before malloc_call
                IRBuilder<> builder(malloc_call);
                Value *mallocated_ptr = builder.CreateCall(runtime_malloc, {malloc_size});
                //delete dependencies of original malloc calls and the original call itself
                malloc_call->replaceAllUsesWith(mallocated_ptr);
                malloc_call->eraseFromParent();
            }
        }

        void replace_free_calls(std::vector<CallInst *> &free_calls, Module &M)
        {
            for (CallInst *&free_call : free_calls)
            {
                //get ptr to free
                Value *to_free_ptr = free_call->getArgOperand(0);
                //get runtime free function prt
                Function *runtime_free = M.getFunction("__runtime_free");
                //Create IRBuilder before free_call
                IRBuilder<> builder(free_call);
                builder.CreateCall(runtime_free, {to_free_ptr});
                //remove the original free_call
                free_call->eraseFromParent();
            }
        }

        void malloc_free_instrumentation_internal_pass(Module &M)
        {
            for (Function &F : M)
            {
                if (F.isDeclaration())
                {
                    continue;
                }
                std::vector<CallInst *> malloc_calls;
                std::vector<CallInst *> free_calls;
                for (BasicBlock &BB : F)
                {
                    for (BasicBlock::iterator i = BB.begin(); i != BB.end(); ++i)
                    {
                        Instruction *to_check = &(*i);
                        if (CallInst *call_inst = dyn_cast<CallInst>(to_check))
                        {
                            Function *callee = call_inst->getCalledFunction();
                            if (callee)
                            {
                                StringRef callee_name = callee->getName();
                                if (callee_name == "malloc")
                                {
                                    malloc_calls.emplace_back(call_inst);
                                }
                                else if (callee_name == "free")
                                {
                                    free_calls.emplace_back(call_inst);
                                }
                            }
                        }
                    }
                }
                replace_malloc_calls(malloc_calls, M);
                replace_free_calls(free_calls, M);
            }
        }

        void load_store_instrumentation_internal_pass(Module &M)
        {
            for (Function &F : M)
            {
                if (F.isDeclaration())
                {
                    continue;
                }
                std::vector<LoadInst *> load_insts;
                std::vector<StoreInst *> store_insts;
                for (BasicBlock &BB : F)
                {
                    for (BasicBlock::iterator i = BB.begin(); i != BB.end(); ++i)
                    {
                        Instruction *to_check = &(*i);
                        if (LoadInst *load_inst = dyn_cast<LoadInst>(to_check))
                        {
                            load_insts.emplace_back(load_inst);
                        }
                        else if (StoreInst *store_inst = dyn_cast<StoreInst>(to_check))
                        {
                            store_insts.emplace_back(store_inst);
                        }
                    }
                }
                // instrument before load and store instructions
                instrument_before_load(load_insts, M);
                instrument_before_store(store_insts, M);
            }
        }

        void instrument_before_load(std::vector<LoadInst *> &load_insts, Module &M)
        {
            for (LoadInst *&load_inst : load_insts)
            {
                // get load address and size
                Value *load_address = load_inst->getPointerOperand();
                Type *load_type = load_inst->getType();
                const DataLayout &M_DL = M.getDataLayout();
                uint64_t accessSize = M_DL.getTypeStoreSize(load_type);
                // prepare runtime function
                Function *runtime_check_addr = M.getFunction("__runtime_check_addr");
                // create IRBuilder
                IRBuilder<> builder(load_inst);
                // make sure load address is of type i8 *
                load_address = builder.CreateBitCast(load_address, builder.getInt8PtrTy());
                // create runtime function call
                builder.CreateCall(runtime_check_addr,
                                   {load_address, ConstantInt::get(Type::getInt64Ty(M.getContext()), accessSize)});
            }
        }

        void instrument_before_store(std::vector<StoreInst *> &store_insts, Module &M)
        {
            for (StoreInst *&store_inst : store_insts)
            {
                // Get store address and size
                Value *store_address = store_inst->getPointerOperand();
                Type *value_type = store_inst->getValueOperand()->getType();
                const DataLayout &M_DL = M.getDataLayout();
                uint64_t store_size = M_DL.getTypeStoreSize(value_type);
                // prepare runtime function
                Function *runtime_check_addr = M.getFunction("__runtime_check_addr");
                // create IRBuilder
                IRBuilder<> builder(store_inst);
                // make sure store address is of type i8 *
                store_address = builder.CreateBitCast(store_address, builder.getInt8PtrTy());
                // create runtime function call
                builder.CreateCall(runtime_check_addr,
                                   {store_address, ConstantInt::get(Type::getInt64Ty(M.getContext()), store_size)});
            }
        }

        void alloca_instrumentation_internal_pass(Module &M)
        {
            // only instrument for alloca in function entry block, VLA is not considered as it is not supported C/C++ language standard any more
            for (Function &F : M)
            {
                if (F.isDeclaration())
                {
                    continue;
                }
                BasicBlock &F_entry = F.getEntryBlock();
                std::vector<AllocaInst *> to_handle_alloca;
                for (BasicBlock::iterator i = F_entry.begin(); i != F_entry.end(); ++i)
                {
                    // iterate every instruction in the entry block, find and instrument AllocaInst
                    Instruction *to_check = &(*i);
                    if (AllocaInst *alloca_inst = dyn_cast<AllocaInst>(to_check))
                    {
                        to_handle_alloca.emplace_back(alloca_inst);
                    }
                }
                for (AllocaInst *&alloca_inst : to_handle_alloca)
                {
                    create_red_zone(alloca_inst, M);
                }
                // will not mark stack white zone as unaccessable for now, because we suppose not to check stack use after free
            }
        }

        void create_red_zone(AllocaInst *alloca_inst_original, Module &M)
        {
            LLVMContext &context = alloca_inst_original->getContext();
            const DataLayout &DL = alloca_inst_original->getModule()->getDataLayout();              // retrieve module data layout
            uint64_t original_size = DL.getTypeAllocSize(alloca_inst_original->getAllocatedType()); // size for a single element
            if (alloca_inst_original->isArrayAllocation())
            {
                // if the original alloca is allocating an array
                if (ConstantInt *constant_number = dyn_cast<ConstantInt>(alloca_inst_original->getArraySize()))
                {
                    // ignore VLA
                    original_size *= constant_number->getZExtValue();
                }
                else
                {
                    return;
                }
            }
            // round to allocate space up to next multiple of 8 and add front and back padding
            uint64_t allocated_size = (original_size + 7) & (~7);
            uint64_t new_allocate_size = allocated_size + 2 * 16;
            // Create new alloca instruction and GEP, mark accessable, insert these instructions to the front of original alloca
            IRBuilder<> builder(alloca_inst_original);
            Type *i8_type_ptr = Type::getInt8Ty(context);
            AllocaInst *new_alloca = builder.CreateAlloca(i8_type_ptr,
                                                          ConstantInt::get(Type::getInt64Ty(context), new_allocate_size),
                                                          alloca_inst_original->getName() + "_with_red_zone");
            new_alloca->setAlignment(Align(16));
            Value *new_base_ptr = builder.CreateGEP(i8_type_ptr,
                                                    new_alloca,
                                                    ConstantInt::get(Type::getInt64Ty(context), 16),
                                                    new_alloca->getName() + "_with_offset");
            Value *adjusted_new_base_ptr = builder.CreateBitCast(new_base_ptr,
                                                                 alloca_inst_original->getType(),
                                                                 new_base_ptr->getName() + "_adjusted");
            // mark white zone as accessable and read zone as unaccessable
            Function *runtime_mark_accessable = M.getFunction("__runtime_mark_accessable");
            builder.CreateCall(runtime_mark_accessable, {new_base_ptr, ConstantInt::get(Type::getInt64Ty(context), original_size)});
            // remove dependencies of the original alloca and itself
            alloca_inst_original->replaceAllUsesWith(adjusted_new_base_ptr);
            alloca_inst_original->eraseFromParent();
        }

        void declare_runtime_library_functions(Module &M)
        {
            LLVMContext &Context = M.getContext();
            Type *void_ret_type = Type::getVoidTy(Context);
            Type *void_ptr_type = Type::getInt8PtrTy(Context);
            Type *size_t_type = Type::getInt64Ty(Context);
            FunctionType *runtime_check_addr_type = FunctionType::get(void_ret_type, {void_ptr_type, size_t_type}, false);
            FunctionType *runtime_malloc_type = FunctionType::get(void_ptr_type, {size_t_type}, false);
            FunctionType *runtime_free_type = FunctionType::get(void_ret_type, {void_ptr_type}, false);
            FunctionType *runtime_mark_accessable_type = FunctionType::get(void_ret_type, {void_ptr_type, size_t_type}, false);
            FunctionType *runtime_mark_unaccessable_type = FunctionType::get(void_ret_type, {void_ptr_type}, false);
            FunctionCallee runtime_check_addr_callee = M.getOrInsertFunction("__runtime_check_addr", runtime_check_addr_type);
            FunctionCallee runtime_malloc_callee = M.getOrInsertFunction("__runtime_malloc", runtime_malloc_type);
            FunctionCallee runtime_free_callee = M.getOrInsertFunction("__runtime_free", runtime_free_type);
            FunctionCallee runtime_mark_accessable_callee = M.getOrInsertFunction("__runtime_mark_accessable", runtime_mark_accessable_type);
            FunctionCallee runtime_mark_unaccessable_callee = M.getOrInsertFunction("__runtime_mark_unaccessable", runtime_mark_unaccessable_type);
            cast<Function>(runtime_check_addr_callee.getCallee())->setLinkage(GlobalValue::ExternalLinkage);
            cast<Function>(runtime_malloc_callee.getCallee())->setLinkage(GlobalValue::ExternalLinkage);
            cast<Function>(runtime_free_callee.getCallee())->setLinkage(GlobalValue::ExternalLinkage);
            cast<Function>(runtime_mark_accessable_callee.getCallee())->setLinkage(GlobalValue::ExternalLinkage);
            cast<Function>(runtime_mark_unaccessable_callee.getCallee())->setLinkage(GlobalValue::ExternalLinkage);
        }

        bool has_function_main(Module &M)
        {
            for (Function &F : M)
            {
                if (F.getName() == "main")
                {
                    return true;
                }
            }
            return false;
        }

        void add_shadow_memory_initializer(Module &M)
        {
            const std::string shadow_memory_initializer_wrapper_name = "shadow_memory_initializer_wrapper";
            if (!M.getFunction(shadow_memory_initializer_wrapper_name))
            {
                // shadow memory initializer not yet instrumented
                LLVMContext &Ctx = M.getContext();
                FunctionType *shadow_memory_initializerandwrapper_type = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
                // create wrapper function with a function body
                Function *shadow_memory_initializer_wrapper = Function::Create(shadow_memory_initializerandwrapper_type,
                                                                               GlobalValue::InternalLinkage,
                                                                               shadow_memory_initializer_wrapper_name,
                                                                               &M);
                // create function definition of runtime library
                FunctionCallee shadow_memory_initializer_callee = M.getOrInsertFunction("__runtime_init", shadow_memory_initializerandwrapper_type);
                Function *shadow_memory_initializer = cast<Function>(shadow_memory_initializer_callee.getCallee());
                shadow_memory_initializer->setLinkage(GlobalValue::ExternalLinkage);
                // Starts constructing the wrapper function
                BasicBlock *BB = BasicBlock::Create(Ctx, "entry", shadow_memory_initializer_wrapper);
                IRBuilder<> wrapper_builder(BB);
                wrapper_builder.CreateCall(shadow_memory_initializer);
                wrapper_builder.CreateRetVoid();
                appendToGlobalCtors(M, shadow_memory_initializer_wrapper, 0);
            }
        }

        void add_shadow_memory_cleanup(Module &M)
        {
            const std::string shadow_memory_cleanup_wrapper_name = "shadow_memory_cleanup_wrapper";
            if (!M.getFunction(shadow_memory_cleanup_wrapper_name))
            {
                // shadow memory cleanup not yet instrumented
                LLVMContext &Ctx = M.getContext();
                FunctionType *shadow_memory_cleanupandwrapper_type = FunctionType::get(Type::getVoidTy(Ctx), {}, false);
                // create wrapper function with a function body
                Function *shadow_memory_cleanup_wrapper = Function::Create(shadow_memory_cleanupandwrapper_type,
                                                                           GlobalValue::InternalLinkage,
                                                                           shadow_memory_cleanup_wrapper_name,
                                                                           &M);
                // create function definition of runtime library
                FunctionCallee shadow_memory_cleanup_callee = M.getOrInsertFunction("__runtime_cleanup", shadow_memory_cleanupandwrapper_type);
                Function *shadow_memory_cleanup = cast<Function>(shadow_memory_cleanup_callee.getCallee());
                shadow_memory_cleanup->setLinkage(GlobalValue::ExternalLinkage);
                // Starts constructing the wrapper function
                BasicBlock *BB = BasicBlock::Create(Ctx, "entry", shadow_memory_cleanup_wrapper);
                IRBuilder<> wrapper_builder(BB);
                wrapper_builder.CreateCall(shadow_memory_cleanup);
                wrapper_builder.CreateRetVoid();
                appendToGlobalDtors(M, shadow_memory_cleanup_wrapper, 0);
            }
        }
    };
} // namespace

/// Registration
PassPluginLibraryInfo getPassPluginInfo()
{
    const auto callback = [](PassBuilder &PB)
    {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM, auto)
            {
                if (Name == "memory-safety")
                {
                    MPM.addPass(MemorySafetyPass());
                    return true;
                }
                return false;
            });
    };
    return {LLVM_PLUGIN_API_VERSION, "MemorySafetyPass",
            LLVM_VERSION_STRING, callback};
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return getPassPluginInfo();
}

#undef DEBUG_TYPE
