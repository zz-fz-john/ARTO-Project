//<(checkpoint1,checkpoint2,hash(LOA)),[(BBLs1,BBLd1),(BBLs2,BBLd2)...]>
//In order to record the trible value in the checkpoint
//checkpoint type1 = loop or recursive
#include <string>
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
//#include "llvm/Analysis/IndirectCallSiteVisitor.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include <fstream>
#include <map>
#include<vector>
#include<algorithm>
#include <iostream>
#include <set>
#define DEBUG_TYPE "insert-check-point-on-loop"
using namespace llvm;
STATISTIC(NumOfLoopCheckPoint,"Number of check points on loop");
//static cl::opt<bool> InsertCheckpointOnLoop("insert-check-point-on-loop",cl::Hidden,cl::init(true));

typedef std::map<std::string,int> FunctionMap;
typedef std::vector<std::string> stringlist;
namespace{
    struct InsertCheckpoints:public ModulePass {
        static FunctionMap *fmap;
        static int FID;
        static char ID;
        static stringlist *AvoidHandleFuncName;
        static stringlist *ToprocessFunction;
        static stringlist * RecursiveFunc;
        static stringlist *fakeRecursiveFunc;
        std::vector<std::string> critical_func;
        std ::set <std::string> real_checkpoint;
        std::set <std::string> CheckpointFunc;
        //stringlist  ReadLibcFuncInfile(const std::string&filename);
        bool inser_checkpoint(Instruction *I);
        bool processFunction(Function &F);
        bool runOnModule(Module &M)override;
        bool runOnLoopAndSubLoop(Loop*L);
        bool runOnLoop(Loop*L);
        InsertCheckpoints():ModulePass(ID){}
        void initAvoidHandleFunction();
        void initProcessFunction();
        void initRecursiveFunc();
        std::string getStringFromValue(Value *V);
        // void initCheckpointFunc();
        bool InstrumentLoopCheckPoints(Instruction* I);
        bool dfs_findInCall(Module*M,std::string funcname,std::set<std::string>&visited);
        void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    // We need to modify IR, so we can't indicate AU to preserve analysis results. 
    // AU.setPreservesAll();
  }
    };
}//end of namespace
// stringlist InsertCheckpoints::ReadLibcFuncInfile(const std::string &filename)
// {
//     std::vector<std::string>  result;
//     std::ifstream file(filename);
//     if(!file.is_open())
//     {
//         errs()<<"could not open file"<<filename<<"\n";

//     }
//     std::string line;
//     while (std::getline(file,line))
//     {
//         result.push_back(line);
//     }
//     return result;
    
// }
void InsertCheckpoints::initAvoidHandleFunction(){
    std::string filename="./avoid_handle_function.txt";
    //LibcName=ReadLibcFuncInfile(filename);
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";

    }
    std::string line;
    while (std::getline(file,line))
    {
        AvoidHandleFuncName->push_back(line);
    }
    return ;
}
void InsertCheckpoints::initProcessFunction(){
    std::string filename="./ToInsertFunc.txt";

    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";

    }
    std::string line;
    while (std::getline(file,line))
    {
        ToprocessFunction->push_back(line);
    }
    return ;
}
// void InsertCheckpoints::initCheckpointFunc(){
//     std::string filename="./checkpoint_function.txt";
//     std::ifstream file(filename);
//     if(!file.is_open())
//     {
//         errs()<<"could not open file"<<filename<<"\n";

//     }
//     std::string line;
//     while (std::getline(file,line))
//     {
//         CheckpointFunc->push_back(line);
//     }
//     return;
// }
void InsertCheckpoints::initRecursiveFunc(){
    //errs()<<"run init recursive func"<<'\n';
    std::string filename="./recursive_function.txt";
    //LibcName=ReadLibcFuncInfile(filename);
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";

    }
    std::string line;
    while(std::getline(file,line))
    {
        RecursiveFunc->push_back(line);
        CheckpointFunc.insert(line);
    }

    std::string filename1="./leaf_func.txt";
    std::ifstream file1(filename1);
    std::string filename2="./only_called_once_func.txt";
    std::ifstream file2(filename2);

    if(!file1.is_open())
    {
        errs()<<"could not open file"<<filename1<<"\n";

    }
    if(!file2.is_open())
    {
        errs()<<"could not open file"<<filename2<<"\n";
    }
    std::string line1;
    std::string line2;
    std ::set <std::string> tmp_funcset1;
    std ::set <std::string> tmp_funcset2;
    //errs()<<"run get line  from leaf_func"<<'\n';
    while(std::getline(file1,line1))
    {   
        //errs()<<"leaf func is : "<<line1<<'\n';
        tmp_funcset1.insert(line1);
        real_checkpoint.insert(line1);
        CheckpointFunc.insert(line1);
    }


}

  std::string InsertCheckpoints::getStringFromValue(Value *V) {
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
bool InsertCheckpoints::runOnModule(Module&M)
{
    bool modified=false;
    initAvoidHandleFunction();
    initProcessFunction();
    initRecursiveFunc();
    errs()<<"run on module on dummy code"<<"\n";
    for (auto &F :M)
    {   
        if(F.isDeclaration())
            continue;
        std::string name=F.getName().str();
        if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),name)==ToprocessFunction->end())
        {
            errs()<<F.getName()<<"is in avoid_handle_function"<<"\n";
            continue;
        }
        if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),name)!=ToprocessFunction->end())
        {
            errs()<<"process function:" <<F.getName()<<"\n";
            modified|=processFunction(F);
        }
    
    }
    for (auto &F:M)
    {   
        if(F.isDeclaration())
            continue;  
        std::string name=F.getName().str();   
        if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),name)==ToprocessFunction->end())
        {
            errs()<<F.getName()<<"is in avoid_handle_function"<<"\n";
            continue;
        }
        
        if(std::find(RecursiveFunc->begin(),RecursiveFunc->end(),name)!=RecursiveFunc->end())
        {
            errs()<<"process recursive function:" <<F.getName()<<"\n";
            BasicBlock &entry=F.getEntryBlock();
            Instruction *firstinst=&*entry.begin();
            IRBuilder<> B(firstinst);
            Module *M=B.GetInsertBlock()->getModule();
            Type * voidty=B.getVoidTy();
            Type* I32Ty=B.getInt32Ty();
            FunctionCallee  FuncInsertCheckpoint=M->getOrInsertFunction("recursive_recordpoint",voidty);
            B.CreateCall(FuncInsertCheckpoint);

            FunctionCallee FuncInsertCheckpoint_ret=M->getOrInsertFunction("ret_recursive_recordpoint",voidty);
            for (BasicBlock &BB : F) {
                Instruction *termInst = BB.getTerminator(); 
                if (isa<ReturnInst>(termInst)) {

                    IRBuilder<> ReturnBuilder(termInst);
                    ReturnBuilder.CreateCall(FuncInsertCheckpoint_ret);
                }

            }
            continue;
        }
        if(std::find(fakeRecursiveFunc->begin(),fakeRecursiveFunc->end(),name)!=fakeRecursiveFunc->end())
        {
            errs()<<"process fake recursive function:" <<F.getName()<<"\n";
            BasicBlock &entry=F.getEntryBlock();
            Instruction *firstinst=&*entry.begin();
            IRBuilder<> B(firstinst);
            Module *M=B.GetInsertBlock()->getModule();
            Type * voidty=B.getVoidTy();
            Type* I32Ty=B.getInt32Ty();
            FunctionCallee  FuncInsertCheckpoint=M->getOrInsertFunction("recursive_latch",voidty);
            B.CreateCall(FuncInsertCheckpoint);
            continue;
        }
        if(std::find(real_checkpoint.begin(),real_checkpoint.end(),name)!=real_checkpoint.end())
        {
            errs()<<"process real checkpoint function:" <<F.getName()<<"\n";
            BasicBlock &entry=F.getEntryBlock();
            Instruction *firstinst=&*entry.begin();
            IRBuilder<> B(firstinst);
            Module *M=B.GetInsertBlock()->getModule();
            Type * voidty=B.getVoidTy();
            Type* I32Ty=B.getInt32Ty();
            FunctionCallee  FuncInsertCheckpoint=M->getOrInsertFunction("checkpoint",voidty);
            B.CreateCall(FuncInsertCheckpoint);
            continue;
        }
    }

    
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

  
    for (unsigned i = 0; i < AnnotationsArray->getNumOperands(); ++i) {
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
            std::string annotation=getStringFromValue(AnnotationString);
            if(annotation.find("critical function") != std::string::npos)
            {
                critical_func.push_back(func_name);
                
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
    std::string filename="./checkpoint_function.txt";
    std::ofstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    for(auto &func:CheckpointFunc)
    {
        file<<func<<"\n";
    }
    return modified;
}
bool InsertCheckpoints::processFunction(Function& F)
{
    bool modified=false;
    std::string name=F.getName().str();
    int fid;
    int count=0;
    //check whether we have visit this function before;
    if(fmap->find(name)!=fmap->end()){
        (*fmap)[name]=++FID;
    }
    fid=(*fmap)[name];
    errs()<<"process function:" <<F.getName()<<"\n";

    if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),name)==ToprocessFunction->end())
    {
        errs()<<F.getName()<<"is in avoid_handle_function"<<"\n";
        return false;
    }
    errs()<<"process loop"<<"\n";

    LoopInfo & LI=getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    if(!LI.empty())
    {
        errs()<<"after analysis on loop"<<"\n";
        int i=0;
        for(Loop * I :LI)
        {

            modified|= runOnLoopAndSubLoop(I);

        }
        if(modified==true)
        {   
            CheckpointFunc.insert(name);

        }
      
    }

    return modified;

}

bool InsertCheckpoints::runOnLoopAndSubLoop(Loop *L)
{
    errs()<<"process runOnLoopAndSubLoop"<<"\n";
    bool modified=false;
    for (Loop *I:*L)
    {   
        modified|=runOnLoopAndSubLoop(I);
    }
    modified|=runOnLoop(L);
    return modified;
}
bool InsertCheckpoints::dfs_findInCall(Module*M,std::string funcname,std::set<std::string> &visited)
{
    if(visited.find(funcname)!=visited.end())
    {
        return false;
    }
    else
    {
        visited.insert(funcname);
    }
    Function*func=M->getFunction(funcname);
    bool result=false;
    if(func)
    {
        for(auto &BB:*func)
        {
            for(auto &I:BB)
            {
                if(CallInst* callInst=dyn_cast<CallInst>(&I))
                {

                    if(callInst->isIndirectCall())
                    {
                        result=true;
                        return result;
                    }
                    else{
                        Function *calledFunc = callInst->getCalledFunction();
                        if (calledFunc) {
                            std::string calledFuncName = calledFunc->getName().str();
                            if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),calledFuncName)==ToprocessFunction->end())
                            {
                                continue;
                            }
                            else
                            {
                                result=true;
                                return true;
                                // result=dfs_findInCall(M,calledFuncName,visited);
                                // if(result==true)
                                // {
                                //     return result;
                                // }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}
bool InsertCheckpoints::runOnLoop(Loop* L)
{
    bool modified=false;
    bool hasFunctionCall = false;
    for (BasicBlock *BB : L->getBlocks())
    {
        for (Instruction &I : *BB) 
        {
            if (isa<CallInst>(&I))
            {
                CallInst *callInst = dyn_cast<CallInst>(&I);
                if (callInst) 
                {
                    if(callInst->isIndirectCall())
                    {
                        hasFunctionCall=true;
                        break;
                    }
                    else
                    {
                        Function *calledFunc = callInst->getCalledFunction();
                        if (calledFunc) 
                        {
                            std::string calledFuncName = calledFunc->getName().str();
                            if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),calledFuncName)!=ToprocessFunction->end())
                            {
                             
                                hasFunctionCall=true;
                                break;
                            }
                        }
                        else
                        {

                            if (Value *calledValue = callInst->getCalledOperand()) {
                                if (Function *func = dyn_cast<Function>(calledValue)) {
                                    std::string calledFuncName = func->getName().str();
                                    if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),calledFuncName)!=ToprocessFunction->end())
                                    {
                                        hasFunctionCall=true;
                                        break;
                                    }
                                }
                            }
                        }

                    }
                }

            }
        }
        Instruction* termInst=BB->getTerminator();
        if(isa<ReturnInst>(termInst))
        {
            hasFunctionCall=true;
            break;
        }
        if(hasFunctionCall==true)
            break;
    }
    SmallVector<BasicBlock*, 8> ExitBlocks;
    L->getExitBlocks(ExitBlocks); 
    for (BasicBlock *BB : ExitBlocks) {
        for (Instruction &I : *BB) {
            
            if (isa<CallInst>(&I)) {
                CallInst *callInst = dyn_cast<CallInst>(&I);
                if (callInst) 
                {
                    if(callInst->isIndirectCall())
                    {
                        hasFunctionCall=true;
                        break;
                    }
                    else
                    {
                        Function *calledFunc = callInst->getCalledFunction();
                        if (calledFunc) 
                        {
                            std::string calledFuncName = calledFunc->getName().str();
                            if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),calledFuncName)!=ToprocessFunction->end())
                            {
                                hasFunctionCall=true;
                                break;
                            }
                        }
                        else
                        {
                            
                            if (Value *calledValue = callInst->getCalledOperand()) {
                                if (Function *func = dyn_cast<Function>(calledValue)) {
                                    std::string calledFuncName = func->getName().str();
                                    if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),calledFuncName)!=ToprocessFunction->end())
                                    {
                                        hasFunctionCall=true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if(hasFunctionCall==true)
            break;
    }
    if(hasFunctionCall==false)
    {
        BasicBlock *latchNode=L->getLoopLatch();
        if(latchNode)
        {
            Instruction * terminator=latchNode->getTerminator();
            IRBuilder<> B(terminator);
            Module *M=B.GetInsertBlock()->getModule();
            Type * voidty=B.getVoidTy();
            Type* I32Ty=B.getInt32Ty();
            FunctionCallee  FuncInsertCheckpoint=M->getOrInsertFunction("loop_latch",voidty);
            B.CreateCall(FuncInsertCheckpoint);
            return false;
        }
        return false;
    }

    BasicBlock * latchNode=L->getLoopLatch();
    if(latchNode)
    {
        //Instruction * firstinst=&*latchNode->begin();
        //modified|=InstrumentLoopCheckPoints(firstinst);
        Instruction * terminator=latchNode->getTerminator();
        if(BranchInst *brInst=dyn_cast<BranchInst>(terminator)){
           //modified|= InstrumentLoopCheckPointsOnlatch(terminator);
           modified|=InstrumentLoopCheckPoints(terminator);
        }

    }
    return modified;

}

bool InsertCheckpoints::InstrumentLoopCheckPoints(Instruction* I)
{
    //bool modified=false;
    IRBuilder<> B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    Type* I32Ty=B.getInt32Ty();
    FunctionCallee  FuncInsertCheckpoint=M->getOrInsertFunction("loop_recordpoint",voidty);
    
    B.CreateCall(FuncInsertCheckpoint);
    return true;
}
char InsertCheckpoints::ID=0;
int InsertCheckpoints::FID=0;
FunctionMap *InsertCheckpoints::fmap= new FunctionMap();
stringlist * InsertCheckpoints::AvoidHandleFuncName=new stringlist();
stringlist * InsertCheckpoints::ToprocessFunction=new stringlist();
stringlist * InsertCheckpoints::RecursiveFunc=new stringlist();
stringlist * InsertCheckpoints::fakeRecursiveFunc=new stringlist();
static RegisterPass<InsertCheckpoints> X("insert-check-point-on-loop","insert check points on loop");