#include "addMetadata.h"
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include<algorithm>
#include <map>
#include "macro.h"
using namespace llvm;
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

void addmd::AddMetaData::runOnModule(llvm::Module &M){

    int inCallID = 0;
    int diCallID=0;
    std::string file_name="../output/direct_call_result.txt";
    std::string str1="__cxx_global_var_init";
    std::string str2="_GLOBAL__sub_I_";
    std::string str3="llvm";
    std::string str4="_cxx";
    std::ofstream outfile(file_name);
    
    for(auto& F : M){
        if(F.isDeclaration()){
            continue;
        }
        std::map<int,std::string> directCalls;
        for(auto& BB : F){
            for(auto& I : BB){
                if(auto* callInst = llvm::dyn_cast<llvm::CallInst>(&I)){
                    if (callInst->isIndirectCall()) {
                        // llvm::MDBuilder MDB(M.getContext());
                        llvm::Constant* inCallIDConstant = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M.getContext()), inCallID++);
                        auto *metadataNode = llvm::MDNode::get(M.getContext(), llvm::ConstantAsMetadata::get(inCallIDConstant));
                        callInst->setMetadata("inCallID", metadataNode);
                    }
                    else if (callInst->getCalledFunction()) {
                        std::string callee_name=callInst->getCalledFunction()->getName().str();
                        std::string::size_type idx1 = callee_name.find( str1 );
                        std::string::size_type idx2 = callee_name.find(str2);
                        std::string::size_type idx3 = callee_name.find(str3);
                        std::string::size_type idx4 = callee_name.find(str4);
                        if((idx1==std::string::npos)&&(idx2==std::string::npos)&&(idx3==std::string::npos)&&(idx4==std::string::npos))
                        {
                            //directCalls.insert(calledFunc->getName().str());
                            directCalls[diCallID]=callee_name;
                            llvm::Constant* diCallIDConstant = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M.getContext()), diCallID++);
                            auto *metadataNode = llvm::MDNode::get(M.getContext(), llvm::ConstantAsMetadata::get(diCallIDConstant));
                            callInst->setMetadata("diCallID", metadataNode);
                        }
                    }
                    else if(ConstantExpr *ConstEx = dyn_cast_or_null<ConstantExpr>(callInst->getCalledOperand())){
                                         
                        if (auto *CE = dyn_cast<ConstantExpr>(callInst->getCalledOperand())) {
                            
                            if (CE->isCast()) {  // 仅处理转换表达式
                                if (auto *Func = dyn_cast<Function>(CE->getOperand(0))) {
                                    std::string callee_name=Func->getName().str();
                                    std::string::size_type idx1 = callee_name.find( str1 );
                                    std::string::size_type idx2 = callee_name.find(str2);
                                    std::string::size_type idx3 = callee_name.find(str3);
                                    std::string::size_type idx4 = callee_name.find(str4);
                                    if((idx1==std::string::npos)&&(idx2==std::string::npos)&&(idx3==std::string::npos)&&(idx4==std::string::npos))
                                    {
                                        directCalls[diCallID]=callee_name;
                                        llvm::Constant* diCallIDConstant = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M.getContext()), diCallID++);
                                        auto *metadataNode = llvm::MDNode::get(M.getContext(), llvm::ConstantAsMetadata::get(diCallIDConstant));
                                        callInst->setMetadata("diCallID", metadataNode);
                                    }

                                }
                            }
                        }
                    }
                }
            }
        }
        std::string function_name=F.getName().str();
        for(auto it=directCalls.begin();it!=directCalls.end();it++)
        {
            outfile<<"In function :"<<function_name;
            outfile<<"  direct callsite :   "<<it->first<<"\n";
            outfile<<"--target  "<<it->second<<"\n";
        }
    }
}

void addmd::runAddMetaData(llvm::Module &M){
    llvm::PassBuilder PB;
    llvm::ModulePassManager MPM;
    llvm::ModuleAnalysisManager MAM;

    // Register analysis passes with the managers
    PB.registerModuleAnalyses(MAM);
    AddMetaData am;
    MPM.addPass(std::move(am));
    MPM.run(M, MAM);
}