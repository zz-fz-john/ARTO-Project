
#include <string>
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
#include <fstream>
#include <map>
#include<vector>
#include<algorithm>
#include <set>
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/PassManager.h"
using namespace llvm;
#define DEBUG_TYPE "get-instrument-critical-func"
STATISTIC(NumOfCriticalFunc,"Number of Critical function");
namespace{
    struct CriticalFunctionPass :public ModulePass {
        static char ID; 
        std::vector<std::string> critical_func;
        CriticalFunctionPass():ModulePass(ID){}
        bool runOnModule(Module &M)override;
        std::string getStringFromValue(Value *V);
    };
}

bool CriticalFunctionPass::runOnModule(Module &M) {
    bool modified = false;
    
    GlobalVariable *GlobalAnnotations = M.getNamedGlobal("llvm.global.annotations");
    if (!GlobalAnnotations) {
    errs() << "No @llvm.global.annotations found in the module.\n";
    return false;
    }

    
    if (!GlobalAnnotations->hasInitializer()) {
    errs() << "@llvm.global.annotations has no initializer.\n";
    return false;
    }

    
    ConstantArray *AnnotationsArray = dyn_cast<ConstantArray>(GlobalAnnotations->getInitializer());
    if (!AnnotationsArray) {
    errs() << "@llvm.global.annotations is not a ConstantArray.\n";
    return false;
    }

   
    for (unsigned i = 0; i < AnnotationsArray->getNumOperands(); ++i) 
    {
        ConstantStruct *AnnotationStruct = dyn_cast<ConstantStruct>(AnnotationsArray->getOperand(i));
        if (!AnnotationStruct) {
            errs() << "Annotation element is not a ConstantStruct.\n";
            continue;
        }

        
        if (AnnotationStruct->getNumOperands() != 5) {
            errs() << "Unexpected number of operands in annotation struct.\n";
            continue;
        }

       
        Value *AnnotatedEntity = AnnotationStruct->getOperand(0); 
        if (ConstantExpr *CE = dyn_cast<ConstantExpr>(AnnotatedEntity)) {
            if (CE->getOpcode() == Instruction::BitCast) {
                AnnotatedEntity = CE->getOperand(0); 
            }
        }
        Value *AnnotationString = AnnotationStruct->getOperand(1); 
        std::string annotation=getStringFromValue(AnnotationString);
        Value *FileName = AnnotationStruct->getOperand(2);       
        ConstantInt *LineNumber = dyn_cast<ConstantInt>(AnnotationStruct->getOperand(3)); 
        Value *ExtraInfo = AnnotationStruct->getOperand(4);       

        
        errs() << "Annotation " << i << ":\n";
        if (auto *Entity = dyn_cast<Function>(AnnotatedEntity)) {
            errs() << "  Function: " << Entity->getName() << "\n";
            std::string func_name=Entity->getName().str();
            //use to debug
            // errs()<<annotation<<'\n';
            // errs() << "Annotation (hex): ";
            // for (char c : annotation) {
            //     errs().write_hex(c) << " ";
            // }
            // errs() << "\n";
            // llvm::StringRef annotation_ref(annotation);
            // annotation_ref = annotation_ref.trim(); 
            // errs() << "Annotation (hex): ";
            // for (char c : annotation_ref.str()) {
            //     errs().write_hex(c) << " ";
            // }
            // errs() << "\n";
            if (annotation.find("critical function") != std::string::npos)
            {
                errs()<<"find critical function"<<'\n';
                critical_func.push_back(func_name);
                NumOfCriticalFunc++;
            }
        } else if (auto *GV = dyn_cast<GlobalVariable>(AnnotatedEntity)) {
            errs() << "  Global Variable: " << GV->getName() << "\n";
        } else {
            errs() << "  Annotated Entity: (unknown)\n";
        }

        errs() << "  Annotation String: " << getStringFromValue(AnnotationString) << "\n";

        errs() << "  File Name: " << getStringFromValue(FileName) << "\n";

        if (LineNumber) {
            errs() << "  Line Number: " << LineNumber->getZExtValue() << "\n";
        } else {
            errs() << "  Line Number: (unknown)\n";
        }

        errs() << "  Extra Info: " << (ExtraInfo ? "Non-null" : "null") << "\n";
    }
    for (auto &F :M)
    {
        if(F.isDeclaration())
            continue;
        std::string name=F.getName().str();
        if(std::find(critical_func.begin(),critical_func.end(),name)!=critical_func.end())
        {
            errs()<<"process function:" <<F.getName()<<"\n";
            BasicBlock &entry=F.getEntryBlock();
            Instruction * firstisnt=&*entry.begin();
            IRBuilder<> builder(firstisnt);
            Module *M=builder.GetInsertBlock()->getModule();
            Type * voidty=builder.getVoidTy();
            FunctionCallee  FuncStart=M->getOrInsertFunction("start_collecting",voidty); 
            builder.CreateCall(FuncStart);
            for(auto &BB:F)
            {
                Instruction* termInst=BB.getTerminator();
                if(isa<ReturnInst>(termInst))
                {
                    IRBuilder<> ReturnBuilder(termInst);
                    FunctionCallee FuncEnd=M->getOrInsertFunction("end_collecting",voidty);
                    ReturnBuilder.CreateCall(FuncEnd);
                }
            }
            modified = true;
        }
    }
   
    errs()<<"Critical function:\n";
    std::ofstream outfile("critical_function.txt");
    for(auto &func:critical_func)
    {
        errs()<<func<<"\n";
        outfile<<func<<"\n";
    }
    outfile.close();
    return modified;
}

  std::string CriticalFunctionPass::getStringFromValue(Value *V) {
    if (!V) return "(null)";

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::GetElementPtr) {
            V = CE->getOperand(0); 
        }
    }

    if (GlobalVariable *GVar = dyn_cast<GlobalVariable>(V)) {
        if (GVar->hasInitializer()) {
            if (ConstantDataArray *CDA = dyn_cast<ConstantDataArray>(GVar->getInitializer())) {
                if (CDA->isString()) {
                    return CDA->getAsString().str();
                }
            }
        }
    }

    return "(unknown)";
  }
char CriticalFunctionPass::ID = 0;
static RegisterPass<CriticalFunctionPass> X("get-instrument-critical-func","get and instrument critical func");