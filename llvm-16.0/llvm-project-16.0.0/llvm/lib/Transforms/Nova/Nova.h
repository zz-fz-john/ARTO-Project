#ifndef NOVA_H
#define NOVA_H
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include <queue>
#include <map>
#include <set>
#include <cstdlib>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iostream>
#include <type_traits>
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Pass.h"
#include "llvm/IR/Mangler.h" 
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include <regex>
#include <fstream>


//#include "llvm/IR/InstrTypes.h"
#define MAX_RUNS    1

namespace llvm{
    class Module;

    struct AliasObject;
    struct AliasObjectTuple;
    struct GlobalState;
    struct CallInst;
    struct GEPOperator;
    struct GlobalValue;
    struct CriticalDataPoint;
    typedef SetVector<Value *> ValueSet;
    typedef SetVector<Instruction *> InstSet;

    // AliasTuple，别名元组
    typedef struct AliasObjectTuple *AliasObjectTupleRef;
    typedef SetVector<AliasObjectTupleRef> TupleSet;

    // AliasObject members
    typedef struct AliasObject *AliasObjectRef;
    typedef SetVector<AliasObjectRef> AliasObjectSet;
    typedef std::map<int, AliasObjectSet *> AliasMap;//以偏移量（int）为 key，映射到对应偏移上的别名对象集合。
    typedef AliasMap *AliasMapRef;
    typedef std::map<int, InstSet *> LocalTaintMap;//以偏移量为 key，记录在该偏移处传播到的指令集合，用于局部污点分析。
    typedef LocalTaintMap *LocalTaintMapRef;

    // GlobalState members
    typedef std::map<Value*, InstSet *> TaintMap;//记录全局的污点传播信息，某个 Value* 映射到传播到它的指令集合
    typedef std::map<Value*, TupleSet *> PointsToMap;//记录每个 Value* 可以指向的对象元组集合（指针分析）。
    typedef std::map<Value*, bool> VisitMap;
    typedef TaintMap *TaintMapRef;
    typedef PointsToMap *PointsToMapRef;
    typedef VisitMap *VisitMapRef;

    typedef struct GlobalState *GlobalStateRef; 
    typedef struct CriticalDataPoint *CriticalDataPointRef;
    // SCC
    typedef std::vector<BasicBlock *> SCC;
    typedef std::vector<BasicBlock *> *SCCRef;

// all local/global variables and dynamically allocated objects should have an alias object
// for locations, .aliasMap = null, .taintMap = null, .val = Instruction
// for dynamic allocated object, size is used for record allocation size.
struct AliasObject {//表示一个别名的相关信息，例如他的value，类型，大小
    Value *val;
    bool isStruct;
    bool isLocation;
    Type *type;
    uint32_t size;
    AliasMapRef aliasMap;////以偏移量（int）为 key，映射到对应偏移上的别名对象集合。
    LocalTaintMapRef taintMap;
}; // struct AliasObject

struct AliasObjectTuple {//别名元组，表示偏移量 + 一个别名对象的二元组。
    int offset;// 相对于某个对象的偏移量
    struct AliasObject *ao;// 指向的别名对象
}; // struct AliasObjectTuple

struct GlobalState {//全局状态
    TaintMapRef tMap; // 全局污点映射
    PointsToMapRef pMap;// 指针指向关系
    VisitMapRef vMap;// 访问标记
    CallInst *ci;   // current callInst// 当前分析的调用指令
    ValueSet *senVarSet;// 敏感变量集合
    // 新增：记录对象被访问时的上下文偏移信息
    //std::map<AliasObjectTupleRef, AliasObjectTupleRef> contextOffsetMap;
}; // GlobalState
// add
struct CriticalDataPoint{//标识临界点信息
    Value*var;
    int type;//0标识是手动注释添加，1标识由load分支添加，2标识由store分支添加
};
struct Nova : public ModulePass {
    static char ID;
    Nova() : ModulePass(ID) {}
    bool runOnModule(Module &M);

    // def-use check
    void GetAnnotatedVariables(Module &M, GlobalStateRef gs);
    void DefUseCheck(Module &M, GlobalStateRef gs);
    // Expand sensitive set by backward dataflow: mark sources that define a sensitive var
    void ExtendSensitiveByDataflow(Module &M, GlobalStateRef gs);
    void GetSensitiveVariableAliases(Module &M, GlobalStateRef gs, 
                                   std::map<Value*, std::set<Value*>>& aliasMap);
    std::set<Value*> GetSingleVariableAliases(GlobalStateRef gs, Value* var);
    void RecordDefineEvent(Module &M, Value *var,int setID);
    void CheckUseEvent(Module &M, Value *var,int setID);
    void InstrumentStoreInst(Instruction *inst, Value *addr, Value *val,int setID);
    void InstrumentLoadInst(Instruction *inst, Value *addr, Value *val,int setID);

    // pointer boundary check
    void ConstructCheckHandlers(Module &M);
    void PointerBoundaryCheck(Module &M, ValueSet &vs);
    void PointerAccessCheck(Module &M, Value *v);
    void ArrayAccessCheck(Module &M, Value *v);
    void CollectArrayBoundaryInfo(Module &M, Value *v);
    BasicBlock::iterator GetInstIterator(Value *v);
    Value* CastToVoidPtr(Value* operand, Instruction* insert_at);
    void DissociateBaseBound(Value* pointer_operand);
    void AssociateBaseBound(Value* pointer_operand, Value* pointer_base, Value* pointer_bound);
    Value* GetAssociatedBase(Value* pointer_operand);
    Value* GetAssociatedBound(Value* pointer_operand);
    bool IsStructOperand(Value* pointer_operand);
    Value* GetSizeOfType(Type* input_type);
    void AddBaseBoundGlobalValue(Module &M, Value *v);
    void AddBaseBoundGlobals(Module &M);
    void HandleGlobalSequentialTypeInitializer(Module& M, GlobalVariable* gv);
    void AddStoreBaseBoundFunc(Value* pointer_dest, Value* pointer_base, Value* pointer_bound,
                                 Value* pointer, Value* size_of_type, Instruction* insert_at);
    void GetConstantExprBaseBound(Constant* given_constant, Value* & tmp_base, Value* & tmp_bound);
    void HandleGlobalStructTypeInitializer(Module& M, StructType* init_struct_type, Constant* initializer, 
                                  GlobalVariable* gv, std::vector<Constant*> indices_addr_ptr, int length);
    Instruction* GetGlobalInitInstruction(Module& M);
    void TransformMain(Module& module);
    void GatherBaseBoundPass1(Function* func);
    void GatherBaseBoundPass2(Function* func);
    void AddLoadStoreChecks(Instruction* load_store, std::map<Value*, int>& FDCE_map);
    void AddDereferenceChecks(Function* func, ValueSet &vs);
    void PrepareForBoundsCheck(Module &M, ValueSet &vs);
    void IdentifyFuncToTrans(Module& module);
    bool CheckIfFunctionOfInterest(Function* func);
    bool CheckTypeHasPtrs(Argument* ptr_argument);
    bool CheckPtrsInST(StructType* struct_type);
    bool CheckBaseBoundMetadataPresent(Value* pointer_operand);
    bool HasPtrArgRetType(Function* func); 
    bool IsFuncDefSoftBound(const std::string &str);
    void IdentifyInitialGlobals(Module& module);
    void IdentifyOriginalInst(Function * func);
    void HandleAlloca (AllocaInst* alloca_inst, BasicBlock* bb, BasicBlock::iterator& i);
    void HandleLoad(LoadInst* load_inst);
    void HandleGEP(GetElementPtrInst* gep_inst);
    void HandleBitCast(BitCastInst* bitcast_inst);
    void HandleMemcpy(CallInst* call_inst);
    void HandleCallInst(CallInst* call_inst);
    void HandleSelect(SelectInst* select_ins, int pass);
    void HandlePHIPass1(PHINode* phi_node);
    void HandlePHIPass2(PHINode* phi_node);
    void HandleIntToPtr(IntToPtrInst* inttoptrinst);
    void HandleReturnInst(ReturnInst* ret);
    void HandleExtractElement(ExtractElementInst* EEI);
    void HandleExtractValue(ExtractValueInst* EVI);
    void HandleVectorStore(StoreInst* store_inst);
    void HandleStore(StoreInst* store_inst);
    void InsertMetadataLoad(LoadInst* load_inst);
    void PropagateMetadata(Value* pointer_operand, Instruction* inst, int instruction_type);
    void AddMemcopyMemsetCheck(CallInst* call_inst, Function* called_func);
    void IntroduceShadowStackAllocation(CallInst* call_inst);
    void IntroduceShadowStackDeallocation(CallInst* call_inst, Instruction* insert_at);
    void IntroduceShadowStackStores(Value* ptr_value, Instruction* insert_at, int arg_no);
    void IntroduceShadowStackLoads(Value* ptr_value, Instruction* insert_at, int arg_no);
    int GetNumPointerArgsAndReturn(CallInst* call_inst);
    void GetGlobalVariableBaseBound(Value* operand, Value* & operand_base, Value* & operand_bound);
    Instruction* GetNextInstruction(Instruction* I);
    void IterateCallSiteIntroduceShadowStackStores(CallInst* call_inst);

    //backward analysis to get dependecies
    void AugmentSensitiveSetWithDataflow(Module &M, GlobalStateRef gs,
                                           ValueSet &senVarSet);
    //foward analysis to  get more sensitive variables                                       
    void extendSenVarSet(Module &M,GlobalStateRef gs,ValueSet &senVarSet);
    // SCC traversal
    void Traversal(GlobalStateRef gs, Function *f);
    void CollectOperandsDependencies(GlobalStateRef gs, Value *V, std::set<Value*> &visited);
    void ReverseSCC(std::vector<SCCRef> &, Function *f);
    void VisitSCC(GlobalStateRef gs, SCC &scc);  
    void HandleLoop(GlobalStateRef gs, SCC &scc);  
    void HandleCall(GlobalStateRef gs, CallInst &I);  
    uint32_t LongestUseDefChain(SCC &scc);
    void DispatchClients(GlobalStateRef gs, Instruction &I);
    Function *ResolveCall(GlobalStateRef gs, CallInst &I);
    Function *ResolveBitcastCall(GlobalStateRef gs, CallInst &I);
    void init_pointer_analysis_result();
    void collectNovaSensitiveVars(Module &M, GlobalState *GS,StringRef Target );
    std::vector<Function *> ResolveIndirectCall(GlobalStateRef gs, CallInst &I);
    std::string getStringFromValue(Value *V);
    // points to analysis & taint analysis framework
    void PointsToAnalysis(GlobalStateRef gs, Instruction &I);
    void TaintAnalysis(GlobalStateRef gs, Instruction &I);
    void InitializeGS(GlobalStateRef gs, Module &M);
    void InitializeFunction(GlobalStateRef gs, Function *f, CallInst &I);
    void PrintPointsToMap(GlobalStateRef gs);
    void PrintTaintMap(GlobalStateRef gs);

    // points to analysis rules
    void UpdatePtoAlloca(GlobalStateRef gs, Instruction &I);
    void UpdatePtoBinOp(GlobalStateRef gs, Instruction &I);
    void UpdatePtoLoad(GlobalStateRef gs, Instruction &I);
    void UpdatePtoStore(GlobalStateRef gs, Instruction &I);
    void UpdatePtoGEP(GlobalStateRef gs, Instruction &I);
    void UpdatePtoRet(GlobalStateRef gs, Instruction &I);
    void UpdatePtoBitCast(GlobalStateRef gs, Instruction &I);
    
    // points to analysis helper functions 
    void HandlePtoGEPOperator(GlobalStateRef gs, GEPOperator *op);
    AliasObject *CreateAliasObject(Type *type, Value *v);
    void PrintAliasObject(AliasObjectRef ao);
    bool SkipStructType(Type *type);

    // taint to analysis rules
    void UpdateTaintAlloca(GlobalStateRef gs, Instruction &I);
    void UpdateTaintBinOp(GlobalStateRef gs, Instruction &I);
    void UpdateTaintLoad(GlobalStateRef gs, Instruction &I);
    void UpdateTaintStore(GlobalStateRef gs, Instruction &I);
    void UpdateTaintGEP(GlobalStateRef gs, Instruction &I);
    void UpdateTaintRet(GlobalStateRef gs, Instruction &I);
    void UpdateTaintBitCast(GlobalStateRef gs, Instruction &I);
    static std::map<std::pair<std::string,int>,std::set<std::string>> indirect_call_graph;//存储由指针分析svf+llvm内置虚函数分析得到的间接调用关系
    static std::map<std::pair<std::string,int>,std::set<std::string>> direct_call_graph;//存储直接调用关系

    //dummy function for collecting sensitive variables
    void collect_argument(Module &M);
    void init_value_range();
    void instrument_to_collect_data(Module &M,CriticalDataPoint *cdp,int SetID);
    void expandStructPtr(Module &M, IRBuilder<> &B, Value *ptr,int setID);
    void insertInstrumentation_to_collect(Module &M, IRBuilder<> &B, Value *val,int index);
    void insertInstrumentation_to_check(Module &M,IRBuilder<> &B,Value* addr,Value *val,int setID,float maxvalue,float minvalue);
    void expandAndInstrument(Module &M, IRBuilder<> &B, Value *ptr, Type *ty, std::string prefix = "");
    void get_non_sensitive_var(Module &M,Function& F,SetVector<Value *> &non_sensitive_var,SetVector<Value *> &senVarSet);
    void CheckNoSenVarDef(Module &M, Value *var);
    void InstrumentNoSenStoreVar(Instruction *inst, Value *addr);
    bool whetherIsAFiledInStruct(Value*val);
    bool isCrossFunctionValue(Value* v1, Value* v2);
    Function* getValueFunction(Value* v);
    void CollectValueSources(GlobalStateRef gs, Value* V, std::set<Value*>& out,
                               std::set<Value*>& visited, unsigned depth); 
    // 检查变量集合是否有store或load指令
    bool HasStoreOrLoadInSet(const std::set<Value*>& varSet);
    // 检查变量是否是从常量数组通过 memcpy 初始化的预定义数组
    bool isPreDefinedArray(Value* var); 
}; // struct Nova
} // namespace

#endif //NOVA_H
