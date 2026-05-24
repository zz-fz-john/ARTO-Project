#ifndef ADD_METADATA_H
#define ADD_METADATA_H

#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/WithColor.h>
#include "llvm/IRPrinter/IRPrintingPasses.h"
#include <llvm/Transforms/Utils.h>
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include <llvm/IR/Verifier.h>
#include "llvm/IR/DebugInfoMetadata.h"

namespace addmd{

    void runAddMetaData(llvm::Module &M);

    class AddMetaData : public llvm::PassInfoMixin<AddMetaData>{
    public:
        AddMetaData() {}

        llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM) {
            runOnModule(M);
            return llvm::PreservedAnalyses::all();
        }
        
        static bool isRequired() { return true; }

        void runOnModule(llvm::Module &M);
    }; 
}

#endif