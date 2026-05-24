// Description:
// 1. This pass is used to find the debug information of indirect callsites.
// 2. The pass will output the source code information of the indirect callsite.
// 3. The output format is as follows:
//    In function :<function name>  indirect callsite :   <instruction>
//    <source code information>
// 4. The output will be written to a file named "callsite_target_map.txt".

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <map>
#include<vector>
#include<algorithm>
using namespace llvm;

namespace {
    struct IndirectCallDebugInfoPass : public FunctionPass {
        static char ID;
        IndirectCallDebugInfoPass() : FunctionPass(ID) {}
        
        std::string instructionToString( llvm::Instruction *inst) {
                std::string str;
                llvm::raw_string_ostream rso(str);
                inst->print(rso); 
                rso.flush();
            return str;
        }
        /*
    * a virtual callsite follows the following instruction sequence pattern:
    * %vtable = load this
    * %vfn = getelementptr %vtable, idx
    * %x = load %vfn
    * call %x (this)
    */
        bool isVirtualCallsite(CallBase* cs)
        {
                if(cs->getCalledFunction()!=nullptr||cs->arg_empty())
                {
                    return false;

                }
                if(cs->getArgOperand(0)->getType()->isPointerTy()==false)
                {
                    return false;
                }

            Value* vfunc=cs->getCalledOperand();
            if(const LoadInst*vfuncloadinst=dyn_cast<LoadInst>(vfunc))
                {
                  const  Value* vfuncptr=vfuncloadinst->getPointerOperand();
                    if(const GetElementPtrInst*vfuncptrgepinst=dyn_cast<GetElementPtrInst>(vfuncptr))
                    {
                        if(vfuncptrgepinst->getNumIndices()!=1)
                        return false;
                    const  Value* vtbl=vfuncptrgepinst->getPointerOperand();
                        if(isa<LoadInst>(vtbl))
                        {
                            return true;
                        }
                    }
                }
            return false;
        }
        bool runOnFunction(Function &F) override {
            std::string Funcname=F.getName().str();
            std::string filename="./callsite_target_map.txt";
            std::ofstream outfile;
            outfile.open(filename, std::ios::app);
            for (auto &BB : F) {
                for (auto &I : BB) {
                    
                    if (auto *CI = dyn_cast<CallInst>(&I)) {
                        
                        if(isVirtualCallsite(CI)==false)
                            continue;
                        if (CI->getCalledFunction() == nullptr) {
                           
                            std::string inst=instructionToString(&I);
                            if (DILocation *Loc = I.getDebugLoc()) {
                                unsigned Line = Loc->getLine();
                                unsigned Col = Loc->getColumn();
                                StringRef File = Loc->getFilename();
                                StringRef Dir = Loc->getDirectory();

                                
                                // errs() << "Indirect call at: " << Dir << "/" << File 
                                //        << " Line: " << Line << ", Column: " << Col << "\n";
                                errs()<<"In function :"<<Funcname<<"  indirect callsite :   "<<I<<"\n";
                                outfile<<"In function :"<<Funcname<<"  indirect callsite :   "<<inst<<"\n";
                              
                                std::ifstream sourceFile((Dir + "/" + File).str());
                                if (sourceFile.is_open()) {
                                    std::string lineContent;
                                    for (unsigned i = 0; i < Line && std::getline(sourceFile, lineContent); ++i) {
                                        if (i == Line - 1) {
                                            std::string line_funcname=lineContent.substr(Col-1);
                                            size_t pos=line_funcname.find('(');
                                            line_funcname=line_funcname.substr(0,pos);
                                            errs() << "--target  " << line_funcname << "\n";
                                            outfile<<"--target  " << line_funcname << "\n";
                                        }
                                    }
                                    sourceFile.close();
                                } else {
                                    errs() << "Unable to open source file: " << Dir + "/" + File << "\n";
                                }
                            } else {
                                errs() << "No debug info available for this indirect call.\n";
                            }
                        }
                    }
                }
            }
            outfile.close();
            return false; 
        }
    };
}

char IndirectCallDebugInfoPass::ID = 0;
static RegisterPass<IndirectCallDebugInfoPass> X("indirect-call-debug", "Indirect Call Debug Info Pass", false, false);
