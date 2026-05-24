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
#include <fstream>
#include <map>
#include<vector>
#include<set>
#include<algorithm>
#include <iostream>
#include <regex>
#include <tuple>
#include "llvm/IR/GlobalAlias.h"
#define DEBUG_TYPE "insert-dummy-code"
using namespace llvm;
STATISTIC(NumOfLoopCheckPoint,"Number of dummy code  on indirect call");
//static cl::opt<bool> InsertCheckpointOnLoop("insert-check-point-on-loop",cl::Hidden,cl::init(true));

typedef std::map<std::string,int> FunctionMap;
typedef std::vector<std::string> stringlist;
namespace{
    struct InsertDummyCode:public ModulePass {
        static FunctionMap *fmap;
        static int FID;
        static char ID;
        std::vector<std::string> LibcName;
        static stringlist *AvoidHandleFuncName;
        static stringlist *ToprocessFunction;
        static stringlist *DummyCodeName;
        std::vector<std::string> OnlyCalledOnceFunc;
        std::set<int> directCallsiteID;
        std::set<int> indirectCallsiteID;
        std::set<int>indirectCallsitetoOnlyCalledOnceID;
        std::set<int>directCallsitetoOnlyCalledOnceID;
        std::set <std::string> CheckpointFunc;
        std::map<int ,std::vector<std::string>> IndCallMap;
        std::map<std::string,std::tuple<std::string,int>> OnlyCalledOnceMap;
        int count;
        //stringlist  ReadLibcFuncInfile(const std::string&filename);
        bool inser_checkpoint(Instruction *I);
        bool processFunction(Function &F);
        bool runOnModule(Module &M)override;
        bool runOnLoopAndSubLoop(Loop*L,std::string function_name );
        bool runOnLoop(Loop*L,std::string function_name);
        bool InstrumentDummyLoopCode(Instruction* I,std::string function_name);
        InsertDummyCode():ModulePass(ID){}
        void initAvoidHandleFunction();
        bool InstrumentDummyCodeDirectCall(Instruction*I);
        bool InstrumentDummyCodeInCallToCk(Instruction*I);
        bool InstrumentDummyCode(Instruction* I,std::string function_name,int i);
        bool InstrumentDummyCodeInReturn(Instruction* I,std::string function_name,int i);
        bool InstrumentDummyCodeInDiamondShape(BranchInst* BI);
        void initCheckpointFunc();
        void initLibcName();
        void initProcessFunction();
        void initOnlycalledOnceFunction();
        void getIndirectCallsiteID();
        void initIndCallMap();
        std::vector<std::string>  ReadLibcFuncInfile(const std::string &filename);
        bool isDiamondShape(BranchInst *BI);
        bool hasCallOrCondBranchInBlock(BasicBlock *BB, std::set<BasicBlock*> &Visited);
        bool hasCallInBlockRecursive(BasicBlock *BB, BasicBlock *TargetBB, std::set<BasicBlock*> &Visited);
        bool bothBranchesHaveNoCall(BranchInst *BI);
        void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<LoopInfoWrapperPass>();
        // We need to modify IR, so we can't indicate AU to preserve analysis results. 
        // AU.setPreservesAll();
        }
    };
}//end of namespace

void InsertDummyCode::initAvoidHandleFunction(){
    std::string filename="./avoid_handle_function.txt";

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
    AvoidHandleFuncName->push_back("_Z16start_collectingv");
    AvoidHandleFuncName->push_back("_Z14end_collectingv");
    AvoidHandleFuncName->push_back("_Z11recordpointv");
    AvoidHandleFuncName->push_back("_Z16loop_recordpointv");
    AvoidHandleFuncName->push_back("_Z21recursive_recordpointv");
    AvoidHandleFuncName->push_back("use_check");
    AvoidHandleFuncName->push_back("def_check");
    //AvoidHandleFuncName->push_back("main");
    AvoidHandleFuncName->push_back("def_collect");
    AvoidHandleFuncName->push_back("Critical_def_check");
    AvoidHandleFuncName->push_back("non_sen_def_check");
    AvoidHandleFuncName->push_back("use_check_for_basic_type_in_struct");
    AvoidHandleFuncName->push_back("def_check_for_basic_type_in_struct");
    AvoidHandleFuncName->push_back("def_collect_for_float");
    AvoidHandleFuncName->push_back("Critical_def_check_for_float");
    return ;
}

void InsertDummyCode::initOnlycalledOnceFunction(){
    std::string filename="./only_called_once_func.txt";
    std::ifstream file(filename);
    if (!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    std::string line;
    while (std::getline(file,line))
    {
        size_t pos1=line.find("-")+1;
        size_t pos2=line.find("-",pos1);
        std::string func_name=line.substr(0,line.find("-"));
        OnlyCalledOnceFunc.push_back(func_name);
        std::string type=line.substr(pos1,pos2-pos1);
        std::string num=line.substr(pos2+1);
        int number=std::stoi(num);
        if(type=="ind")
        {
            std::tuple<std::string,int > myTuple("ind",number);
            OnlyCalledOnceMap.insert(std::make_pair(func_name,myTuple));
            indirectCallsitetoOnlyCalledOnceID.insert(number);
        }
        else if (type=="di")
        {
            std::tuple<std::string,int > myTuple("di",number);
            OnlyCalledOnceMap.insert(std::make_pair(func_name,myTuple));
            directCallsitetoOnlyCalledOnceID.insert(number);
        }

    }
    return;
    
}

void InsertDummyCode::initCheckpointFunc(){
    std::string filename="./checkpoint_function.txt";
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    std::string line;
    while(std::getline(file,line))
    {
        if(std::find(OnlyCalledOnceFunc.begin(),OnlyCalledOnceFunc.end(),line)!=OnlyCalledOnceFunc.end())
        {
            continue;
        }
        CheckpointFunc.insert(line);
    }
    return;
}

void InsertDummyCode::initIndCallMap(){
    std::string filename="./indirectcall.txt";
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    std::string line;
    
    std::regex pattern_callsite(R"(In function\s*:(\S+)\s*indirect callsite\s*:\s*(\d+))");
    std::regex pattern_target(R"(--target\s*(\S+))");
    int id=0;
    while(std::getline(file,line))
    {
        if(std::regex_match(line,pattern_callsite))
        {
            std::smatch result;
            std::regex_search(line,result,pattern_callsite);
            std::string func_name=result[1];
            id=std::stoi(result[2]);
            //errs()<<"test Id is "<<id<<"\n";
            //IndCallMap[id]=func_name;
        }
        else if(std::regex_match(line,pattern_target))
        {
            std::smatch result;
            std::regex_search(line,result,pattern_target);
            std::string target_name=result[1];
            //errs()<<"test name is "<<target_name<<"\n";
            IndCallMap[id].push_back(target_name);
        }
    }
}
void  InsertDummyCode::getIndirectCallsiteID()
{
    return;
    int CallsiteIDsize=indirectCallsiteID.size();
    int CheckpointFuncsize=CheckpointFunc.size();
    errs()<<"run getIndirectCallsiteID"<<"\n";
    while(1)
    {   
        int old_CallsiteIDsize=CallsiteIDsize;
        int old_CheckpontFuncsize=CheckpointFuncsize;
        for(auto &F:IndCallMap)
        {
            for(auto& target:F.second)
            {
                
                if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),target)!=CheckpointFunc.end())
                {
                    //errs()<<"test  target is "<<target<<'\n';
                    indirectCallsiteID.insert(F.first);
                    break;
                }
            }
        }
        for(auto &ID:indirectCallsiteID)
        {
            //errs()<<"test ID is "<<ID<<"\n";
            for(auto&target :IndCallMap[ID])
            {
                 errs()<<target<<"\n";
                 
                if(std::find(OnlyCalledOnceFunc.begin(),OnlyCalledOnceFunc.end(),target)!=OnlyCalledOnceFunc.end())
                {
                    //OnlyCalledOnceFunc.remove(target);
                    errs()<<"remove onlycall_once func: "<<target<<"\n";
                    OnlyCalledOnceFunc.erase(
                        std::remove(OnlyCalledOnceFunc.begin(), OnlyCalledOnceFunc.end(), target),
                        OnlyCalledOnceFunc.end()
                    );
                    
                }
                CheckpointFunc.insert(target);
            }
        }
        CallsiteIDsize=indirectCallsiteID.size();
        CheckpointFuncsize=CheckpointFunc.size();
        if(CallsiteIDsize==old_CallsiteIDsize&&CheckpointFuncsize==old_CheckpontFuncsize)
        {
            break;
        }
    }
}

void  InsertDummyCode::initProcessFunction(){
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
bool InsertDummyCode::runOnModule(Module&M)
{
    bool modified=false;
    //errs()<<"run on module on dummy code"<<"\n";
    initAvoidHandleFunction();
    //errs()<<"run on module on dummy code after avoid"<<"\n";
    //initLibcName();
    initProcessFunction();
    //errs()<<"run on module on dummy code after process"<<"\n";
    //errs()<<"run on module on dummy code after only"<<"\n";
    initOnlycalledOnceFunction();
    initCheckpointFunc();
    //errs()<<"run on module on dummy code after check"<<"\n";
    
    initIndCallMap();
    getIndirectCallsiteID();
    // initOnlycalledOnceFunction();

    std::vector<GlobalAlias *> AliasesToRemove;

      for (GlobalAlias &Alias : M.aliases()) {
        AliasesToRemove.push_back(&Alias);
      }

      for (GlobalAlias *Alias : AliasesToRemove) {
        Alias->replaceAllUsesWith(Alias->getAliasee());
        Alias->eraseFromParent();
        modified = true;
      }
      
    for (auto &F :M)
    {   
        if(F.isDeclaration())
            continue;
        std::string name=F.getName().str();
        if(std::find(AvoidHandleFuncName->begin(),AvoidHandleFuncName->end(),name)!=AvoidHandleFuncName->end())
        {
            errs()<<F.getName()<<"is in avoid_handle_function"<<"\n";
            continue;
        }
        // if(F.hasFnAttribute(Attribute::OptimizeNone))
        //     continue;
        if(std::find(ToprocessFunction->begin(),ToprocessFunction->end(),name)!=ToprocessFunction->end())
        {
            errs()<<"process function:" <<F.getName()<<"\n";
            modified|=processFunction(F);
        }
    }
    std::string dummyCodeFile="./dummycodeName.txt";
    std::ofstream outfile(dummyCodeFile);
    DummyCodeName->push_back("llvm_CallCkDummy");
    for(auto it=DummyCodeName->begin();it!=DummyCodeName->end();it++)
    {
        outfile<<*it<<'\n';
    }
    outfile.close();
    std::string filename="./checkpoint_function_backup.txt";
    std::ofstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    for(auto &func:CheckpointFunc)
    {
        file<<func<<"\n";
    }
    std::string filename1="./only_called_once_func_backup.txt";
    std::ofstream file1(filename1);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename1<<"\n";
    }
    for(auto &func:OnlyCalledOnceFunc)
    {
        std::string type=std::get<0>(OnlyCalledOnceMap[func]);
        std::string num=std::to_string(std::get<1>(OnlyCalledOnceMap[func]));
        std::string func_name=func+"-"+type+"-"+num;
        file1<<func_name<<"\n";
    }
    return modified;
}
std::vector<std::string> InsertDummyCode:: ReadLibcFuncInfile(const std::string &filename)
{
    std::vector<std::string>  result;
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";

    }
    std::string line;
    while (std::getline(file,line))
    {
        result.push_back(line);
    }
    return result;
    
}
void InsertDummyCode::initLibcName()
    {
    std::string filename="./libc_func_name.txt";
    LibcName=ReadLibcFuncInfile(filename);
    return ;
    }
bool InsertDummyCode::processFunction(Function& F)
{
    bool modified=false;
    std::string name=F.getName().str();
    int fid;
    
    //check whether we have visit this function before;
    if(fmap->find(name)!=fmap->end()){
        (*fmap)[name]=++FID;
    }
    fid=(*fmap)[name];
    errs()<<"process function:" <<F.getName()<<"\n";

    if(std::find(AvoidHandleFuncName->begin(),AvoidHandleFuncName->end(),name)!=AvoidHandleFuncName->end())
    {
        errs()<<F.getName()<<"is in avoid_handle_function"<<"\n";
        return false;
    }
    errs()<<"processing insert dummy in indirectcall and processing libc call"<<"\n";

    for(auto &BB:F)
    {

        Instruction *Terminator = BB.getTerminator();
        if(BranchInst *BI = dyn_cast<BranchInst>(Terminator)) {
            if(isDiamondShape(BI)) {
                modified |= InstrumentDummyCodeInDiamondShape(BI);
            }
            if(bothBranchesHaveNoCall(BI)) {
                modified |= InstrumentDummyCodeInDiamondShape(BI);
              
            }
        }

        for(auto &I:BB)
        {

            if(CallInst*Call=dyn_cast<CallInst>(&I))
            {
                if(Call->isTailCall())
                {
                    Call->setTailCall(false);
                }
            }
            if(auto *Call =dyn_cast<CallBase>(&I))
            {
                // if(start_insert==false)
                //     continue;

                if(Call->isIndirectCall())
                {
                   
                    llvm::Metadata *MD=I.getMetadata("inCallID");
                    auto*md=llvm::dyn_cast<llvm::MDNode>(MD);
                    int inCallID = cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
                    modified|=InstrumentDummyCode(&I,name,inCallID);
                }


            }

        }
    }


    return modified;

}
bool InsertDummyCode::runOnLoopAndSubLoop(Loop*L,std::string function_name)
{
    errs()<<"process runOnLoopAndSubLoop"<<"\n";
    bool modified=false;
    modified|=runOnLoop(L,function_name);
    for (Loop *I:*L)
    {   
        modified|=runOnLoopAndSubLoop(I,function_name);
    }
    return modified;
}
bool InsertDummyCode::runOnLoop(Loop*L,std::string function_name){
    bool modified=false;
    BasicBlock *Header=L->getHeader();
    Instruction * I=Header->getTerminator();
    modified|=InstrumentDummyLoopCode(I,function_name);
    return modified;
}
bool InsertDummyCode::InstrumentDummyCodeInCallToCk(Instruction*I)
{
    errs()<<"processing instrument libc checkpoint dummy"<<"\n";
    //bool modified=false;
    IRBuilder<> B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm_CallCkDummy";

    FunctionCallee  FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    
    B.CreateCall(FuncInsertDummyCode);
    return true;
}
bool InsertDummyCode::InstrumentDummyCodeDirectCall(Instruction*I)
{
    errs()<<"processing instrument libc checkpoint dummy"<<"\n";
    //bool modified=false;
    IRBuilder<> B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm_DirectCallDummy";
    FunctionCallee  FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    
    B.CreateCall(FuncInsertDummyCode);
    return true;
}
bool InsertDummyCode ::InstrumentDummyCodeInReturn(Instruction* I,std::string function_name,int i)
{
    errs()<<"processing instrument return dummy"<<"\n";
    //bool modified=false;
    IRBuilder<> B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm.ReturnDummy."+std::to_string(i)+"."+function_name;
    FunctionCallee  FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    B.CreateCall(FuncInsertDummyCode);
    return true;
}
bool InsertDummyCode::InstrumentDummyLoopCode(Instruction* I,std::string function_name)
{
    errs()<<"processing instrument loop checkpoint dummy"<<"\n";
    //bool modified=false;
    IRBuilder<> B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm.LoopDummy."+std::to_string(count)+"."+function_name;
    count++;
    FunctionCallee  FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    
    B.CreateCall(FuncInsertDummyCode);
    return true;
}
bool InsertDummyCode::InstrumentDummyCode(Instruction* I,std::string funciton_name,int i)
{
    errs()<<"processing instrument function indirect call dummy"<<"\n";
    //bool modified=false;
    // IRBuilder<> B(I->getNextNode());
    IRBuilder<>B(I);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm_Indirectdummy_"+std::to_string(i);
    DummyCodeName->push_back(location_name);
    FunctionCallee  FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    
    B.CreateCall(FuncInsertDummyCode);
    return true;
}


bool InsertDummyCode::InstrumentDummyCodeInDiamondShape(BranchInst* BI) {
    errs()<<"processing instrument diamond shape branch dummy"<<"\n";
    
    IRBuilder<> B(BI);
    Module *M=B.GetInsertBlock()->getModule();
    Type * voidty=B.getVoidTy();
    std::string location_name="llvm_branch_dummy";
    
    FunctionCallee FuncInsertDummyCode=M->getOrInsertFunction(location_name,voidty);
    B.CreateCall(FuncInsertDummyCode);
    
    return true;
}


bool InsertDummyCode::hasCallInBlockRecursive(BasicBlock *BB, BasicBlock *TargetBB, std::set<BasicBlock*> &Visited) {
  
    if (Visited.count(BB)) {
        return false;
    }
    Visited.insert(BB);
    
    
    if (BB == TargetBB) {
        return false;
    }
    
   
    for (Instruction &I : *BB) {
        
        if (CallInst *CI = dyn_cast<CallInst>(&I)) {
            Function *CalledFunc = CI->getCalledFunction();
            if (CalledFunc) {
                std::string funcName = CalledFunc->getName().str();
                
                if (std::find(AvoidHandleFuncName->begin(), AvoidHandleFuncName->end(), funcName) == AvoidHandleFuncName->end()) {
                    errs() << "  Recursive check: Found call to non-AvoidHandleFunction: " << funcName << "\n";
                    return true;
                }
            } else {

                errs() << "  Recursive check: Found indirect call\n";
                return true;
            }
        }
        
        
        if (InvokeInst *II = dyn_cast<InvokeInst>(&I)) {
            Function *CalledFunc = II->getCalledFunction();
            if (CalledFunc) {
                std::string funcName = CalledFunc->getName().str();
                if (std::find(AvoidHandleFuncName->begin(), AvoidHandleFuncName->end(), funcName) == AvoidHandleFuncName->end()) {
                    errs() << "  Recursive check: Found invoke to non-AvoidHandleFunction: " << funcName << "\n";
                    return true;
                }
            } else {
                errs() << "  Recursive check: Found indirect invoke\n";
                return true;
            }
        }
        

    }
    

    Instruction *Terminator = BB->getTerminator();
    for (unsigned i = 0; i < Terminator->getNumSuccessors(); ++i) {
        BasicBlock *Successor = Terminator->getSuccessor(i);
        if (hasCallInBlockRecursive(Successor, TargetBB, Visited)) {
            return true;  
        }
    }
    
    return false;
}

bool InsertDummyCode::hasCallOrCondBranchInBlock(BasicBlock *BB, std::set<BasicBlock*> &Visited) {

    if (Visited.count(BB)) {
        return false;
    }
    Visited.insert(BB);
    
    for (Instruction &I : *BB) {
    
        if (CallInst *CI = dyn_cast<CallInst>(&I)) {
            
            Function *CalledFunc = CI->getCalledFunction();
            if (CalledFunc) {
                std::string funcName = CalledFunc->getName().str();
                if (std::find(ToprocessFunction->begin(), ToprocessFunction->end(), funcName) != ToprocessFunction->end()) {
                    errs() << "  Found call to ToprocessFunction: " << funcName << "\n";
                    return true; 
                }
                
                if (std::find(AvoidHandleFuncName->begin(), AvoidHandleFuncName->end(), funcName) != AvoidHandleFuncName->end()) {
                    errs() << "  Found call to AvoidHandleFunction: " << funcName << "\n";
                    continue; 
                }

                errs() << "  Found call to other function: " << funcName << "\n";
                return true;
            } else {
             
                errs() << "  Found indirect call\n";
                return true;
            }
        }
        
       
        if (InvokeInst *II = dyn_cast<InvokeInst>(&I)) {
            Function *CalledFunc = II->getCalledFunction();
            if (CalledFunc) {
                std::string funcName = CalledFunc->getName().str();
                if (std::find(AvoidHandleFuncName->begin(), AvoidHandleFuncName->end(), funcName) != AvoidHandleFuncName->end()) {
                    errs() << "  Found invoke to AvoidHandleFunction: " << funcName << "\n";
                    continue;
                }
                errs() << "  Found invoke to non-AvoidHandleFunction: " << funcName << "\n";
                return true;
            } else {
                errs() << "  Found indirect invoke\n";
                return true;
            }
        }
        
       
        if (BranchInst *BI = dyn_cast<BranchInst>(&I)) {
            if (BI->isConditional()) {
                errs() << "  Found conditional branch, recursively checking successors...\n";
                
                for (unsigned i = 0; i < BI->getNumSuccessors(); ++i) {
                    BasicBlock *Successor = BI->getSuccessor(i);
                    if (hasCallOrCondBranchInBlock(Successor, Visited)) {
                        return true;  
                    }
                }
            }
        }
    }
    

    Instruction *Terminator = BB->getTerminator();
    if (Terminator) {
        for (unsigned i = 0; i < Terminator->getNumSuccessors(); ++i) {
            BasicBlock *Successor = Terminator->getSuccessor(i);
            if (hasCallOrCondBranchInBlock(Successor, Visited)) {
                return true;  
            }
        }
    }
    
    return false;
}


bool InsertDummyCode::isDiamondShape(BranchInst *BI) {
  
    if (!BI->isConditional()) {
        return false;
    }
    
    BasicBlock *TrueBB = BI->getSuccessor(0);
    BasicBlock *FalseBB = BI->getSuccessor(1);
    std::set<BasicBlock*> Visited;
    if (hasCallOrCondBranchInBlock(FalseBB, Visited)) {
        return false;
    }
    Instruction *FalseTerminator = FalseBB->getTerminator();
    
    if (BranchInst *FalseBr = dyn_cast<BranchInst>(FalseTerminator)) {
        if (FalseBr->isUnconditional()) {
            BasicBlock *FalseTarget = FalseBr->getSuccessor(0);
            if (FalseTarget == TrueBB) {
                errs() << "Found Diamond Shape:\n";
                errs() << "  True branch: " << TrueBB->getName() << "\n";
                errs() << "  False branch: " << FalseBB->getName() << " (simple path - no calls/cond branches)\n";
                errs() << "  Merge point: " << TrueBB->getName() << " (False branch merges to True branch)\n";
                return true;
            }
        }
    }
    
    return false;
}

bool InsertDummyCode::bothBranchesHaveNoCall(BranchInst *BI) {
    if (!BI->isConditional()) {
        errs() << "Not a conditional branch\n";
        return false;
    }
    
    BasicBlock *TrueBB = BI->getSuccessor(0);
    BasicBlock *FalseBB = BI->getSuccessor(1);
    
    errs() << "Checking both branches for non-AvoidHandleFunction calls:\n";
    errs() << "  True branch: " << TrueBB->getName() << "\n";
    errs() << "  False branch: " << FalseBB->getName() << "\n";
    
    std::set<BasicBlock*> VisitedTrue;
    bool trueHasCall = hasCallOrCondBranchInBlock(TrueBB, VisitedTrue);
    
    std::set<BasicBlock*> VisitedFalse;
    bool falseHasCall = hasCallOrCondBranchInBlock(FalseBB, VisitedFalse);
    
    if (!trueHasCall && !falseHasCall) {
        errs() << "  Result: Both branches have NO non-AvoidHandleFunction calls (符合条件)\n";
        return true;
    } else {
        if (trueHasCall) {
            errs() << "  Result: True branch has non-AvoidHandleFunction calls\n";
        }
        if (falseHasCall) {
            errs() << "  Result: False branch has non-AvoidHandleFunction calls\n";
        }
        return false;
    }
}

char InsertDummyCode::ID=0;
int InsertDummyCode::FID=0;
FunctionMap *InsertDummyCode::fmap= new FunctionMap();
stringlist * InsertDummyCode::AvoidHandleFuncName=new stringlist();
stringlist * InsertDummyCode::ToprocessFunction=new stringlist();
stringlist *InsertDummyCode::DummyCodeName=new stringlist();
static RegisterPass<InsertDummyCode> X("insert-dummy-code","insert dummy code on indirect branch");