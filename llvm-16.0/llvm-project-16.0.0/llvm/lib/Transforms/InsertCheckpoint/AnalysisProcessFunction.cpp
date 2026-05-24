
#include <string>
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
//#include "llvm/Analysis/IndirectCallSiteVisitor.h"
#include "llvm/IR/PassManager.h"
#include <fstream>
#define DEBUG_TYPE "insert-dummy-code"
using namespace llvm;
STATISTIC(NumOfProcessfFunction,"Number of to process function");
//生成avoid_function.txt和toProcessFunction.txt
typedef std::vector<std::string> stringlist;
namespace {
    struct AnalysisProcessFunction:public  ModulePass{
        static stringlist *AvoidHandleFuncName;
        static stringlist *ToprocessFunction;
        static char ID;
        AnalysisProcessFunction():ModulePass(ID){}
        bool runOnModule(Module &M)override;
        void WriteTofile();

    };
}
bool AnalysisProcessFunction ::runOnModule(Module &M)
{
    bool modified=false;
    std::string str1="__cxx_global_var_init";
    std::string str2="_GLOBAL__sub_I_";
    std::string str3="_cxx";
    std::string str4="llvm";
    for (auto &F:M)
    {
        if(F.isDeclaration())
            {
                std::string name=F.getName().str();
                AvoidHandleFuncName->push_back(name);
                continue;
                
            }
        else if(F.hasLinkOnceODRLinkage()&&(!F.isDSOLocal()))
        {
            std::string name=F.getName().str();        
            AvoidHandleFuncName->push_back(name);
            continue;
        }
        else
        {
            
            std::string name=F.getName().str();
            std::string::size_type idx = name.find( str1 );
            if(idx!=std::string::npos)
            {
                AvoidHandleFuncName->push_back(name);
                continue;
            }
            idx=name.find(str2);
            if(idx!=std::string::npos)
            {
                AvoidHandleFuncName->push_back(name);
                continue;
            }
            idx=name.find(str3);
            if(idx!=std::string::npos)
            {
                AvoidHandleFuncName->push_back(name);
                continue;
            }
            idx=name.find(str4);
            if(idx!=std::string::npos)
            {
                AvoidHandleFuncName->push_back(name);
                continue;
            }
            if(F.getName().startswith("llvm."))
            {
                AvoidHandleFuncName->push_back(name);
                continue;
            }

            if (llvm::DISubprogram *SP = F.getSubprogram()) {
                if (llvm::DIFile *File = SP->getFile()) {
                    llvm::StringRef Filename = File->getFilename();
                    llvm::StringRef Directory = File->getDirectory();
                    if (Filename.contains("arm-linux-gnueabihf")) {
                        AvoidHandleFuncName->push_back(name);
                        continue;
                    }
                }
            }
            bool isthread_func=false;
            for (auto &BB : F) 
            {
                for (auto &I : BB) {
                    if (auto *call = dyn_cast<CallBase>(&I)) 
                    {
                        Function *callee = call->getCalledFunction();
                        
                        // Support indirect call via bitcast
                        if (!callee) {
                            if (auto *ce = dyn_cast<ConstantExpr>(call->getCalledOperand())) {
                            if (ce->isCast()) {
                                if (auto *func = dyn_cast<Function>(ce->getOperand(0))) {
                                callee = func;
                                }
                            }
                            }
                        }
                        if (callee && callee->getName().contains("pthread_create")) 
                        {
                            errs() << "[+] Found pthread_create in function: " << F.getName() << "\n";
                            if (call->arg_size() >= 4) {
                                Value *funcPtr = call->getArgOperand(2); 

                                if (auto *func = dyn_cast<Function>(funcPtr)) 
                                {
                                    errs() << "    Thread function: " << func->getName() << "\n";
                                    AvoidHandleFuncName->push_back(func->getName().str());
                                    isthread_func = true;
                                } 
                                else if (auto *ce = dyn_cast<ConstantExpr>(funcPtr)) 
                                {
                                    if (ce->isCast()) {
                                    if (auto *f = dyn_cast<Function>(ce->getOperand(0))) {
                                        errs() << "    Thread function (via cast): " << f->getName() << "\n";
                                        AvoidHandleFuncName->push_back(f->getName().str());
                                        isthread_func = true;
                                    }
                                    }
                                } 
                                else 
                                {
                                    errs() << "    Thread function: unknown or indirect\n";
                                }
                            }
                        }
                    }
                }
            }
            // if(isthread_func==true)
            // {
            //     continue;
            // }
            for (auto &BB : F) {
                for (auto &I : BB) {
                    if (auto *call = dyn_cast<CallBase>(&I)) {
                    Function *callee = call->getCalledFunction();

                    // Try to resolve through bitcast
                    if (!callee) {
                        if (auto *ce = dyn_cast<ConstantExpr>(call->getCalledOperand())) {
                        if (ce->isCast()) {
                            if (auto *func = dyn_cast<Function>(ce->getOperand(0))) {
                            callee = func;
                            }
                        }
                        }
                    }

                    if (!callee) continue;
                    // Match pthread_key_create
                    if (callee->getName().contains("pthread_key_create")) {
                        errs() << "[+] Found pthread_key_create in function: " << F.getName() << "\n";

                        if (call->arg_size() >= 2) {
                        Value *destructorArg = call->getArgOperand(1);

                        if (auto *func = dyn_cast<Function>(destructorArg)) {
                            errs() << "    Destructor function: " << func->getName() << "\n";
                             AvoidHandleFuncName->push_back(func->getName().str());
                        } else if (auto *ce = dyn_cast<ConstantExpr>(destructorArg)) {
                            if (ce->isCast()) {
                            if (auto *f = dyn_cast<Function>(ce->getOperand(0))) {
                                AvoidHandleFuncName->push_back(f->getName().str());
                                errs() << "    Destructor function (via cast): " << f->getName() << "\n";
                            }
                            }
                        } else {
                            errs() << "    Destructor function: <unknown>\n";
                        }
                        }
                    }
                    }
                }
            }
            if(std::find(AvoidHandleFuncName->begin(),AvoidHandleFuncName->end(),name)!=AvoidHandleFuncName->end())
            {
                continue;
            }
            ToprocessFunction->push_back(name);
        }
    }
   WriteTofile();
   return false;
}
void AnalysisProcessFunction::WriteTofile()
{
    std::string AvoidFileName="./avoid_handle_function.txt";
    std::string ToProcessFuncName="./to_process_function.txt";
    std::ofstream outfile1(AvoidFileName);
    std::ofstream outfile2(ToProcessFuncName);
    for(auto it=AvoidHandleFuncName->begin();it!=AvoidHandleFuncName->end();it++)
    {
        outfile1<<*it<<'\n';
        
    }
    outfile1.close();
    for (auto it = ToprocessFunction->begin(); it != ToprocessFunction->end(); it++)
    {
        outfile2<<*it<<'\n';
       
    }
     outfile2.close();
    
    
}
char AnalysisProcessFunction::ID=0;
stringlist * AnalysisProcessFunction::AvoidHandleFuncName=new stringlist();
stringlist * AnalysisProcessFunction::ToprocessFunction=new stringlist();
static RegisterPass<AnalysisProcessFunction> X("generate-process-function","generate to process function");