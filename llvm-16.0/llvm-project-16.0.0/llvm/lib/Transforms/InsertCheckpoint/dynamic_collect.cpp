// Description: This file is used to insert the dynamic collect function to collect the indirect call hints.
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Pass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <map>
#include<vector>
#include<algorithm>
using namespace llvm;

namespace {

struct IndirectCallInstrumentation : public FunctionPass {
    static char ID;
     std::map<std::string,std::vector<std::string>> funcToIR;
    IndirectCallInstrumentation() : FunctionPass(ID) {}
    bool runOnFunction(Function &F) override {
        if(funcToIR.empty())
        {
        std::string filename="./to_insertIR.txt";
        std::ifstream file(filename);
        if(!file.is_open())
        {
            errs()<<"could not open file "<<filename<<'\n';

        }
        std::string line;
        while(std::getline(file,line))
        {  std::string delimiter="----";
           size_t split_pos=line.find(delimiter);
           std::string function_name=line.substr(0,split_pos);
           std::string IRcallsite=line.substr(split_pos+delimiter.length());
           funcToIR[function_name].push_back(IRcallsite);
        }
        file.close();
        for (std::map<std::string, std::vector<std::string>>::iterator it = funcToIR.begin(); it != funcToIR.end(); ++it) {
            //errs()<< it->second<< '\n';
            }
        }
        
        Module *M = F.getParent();
        LLVMContext &Ctx = M->getContext();

        // 获取插桩函数的声明
        Type *VoidPtrTy = Type::getInt8PtrTy(Ctx);
        FunctionType *TraceFuncTy = FunctionType::get(Type::getVoidTy(Ctx), { VoidPtrTy}, false);
        FunctionCallee TraceFunc = M->getOrInsertFunction("__collect_icall_hints", TraceFuncTy);
        std::string Funcname=F.getName().str();
        if(funcToIR.find(Funcname)==funcToIR.end() )
        {
                //errs()<<Funcname<<" not in map"<<'\n';
                return false;
        }
       
        for (auto &BB : F) {


            for (auto &I : BB) {
               
                if (auto *Call = dyn_cast<CallInst>(&I)) {
                    if (Call->getCalledFunction()==nullptr) {
                        IRBuilder<> Builder(Call);
                        std::string instructionStr;
                        raw_string_ostream rso(instructionStr);
                        I.print(rso);
                        errs()<<"Instruction as string: "<<instructionStr<<'\n';
                       
                        Value *Callee = Call->getCalledOperand();
                        for(std::string callsite: funcToIR[Funcname] )
                        {
                            std::string::size_type idx;
                            idx=instructionStr.find(callsite);
                            if(idx==std::string::npos)
                             continue;
                            
                         //Value *SrcAddr = Builder.CreatePointerCast(Call, VoidPtrTy);

                            
                            Value *DstAddr = Builder.CreatePointerCast(Callee, VoidPtrTy);

                            
                            Builder.CreateCall(TraceFunc, { DstAddr});
                        }

                    }
                }
            }
        }
        return true;
    }
};

} // end anonymous namespace

char IndirectCallInstrumentation::ID = 0;
static RegisterPass<IndirectCallInstrumentation> X("indirect-call-instrumentation", "Instrument Indirect Calls", false, false);
