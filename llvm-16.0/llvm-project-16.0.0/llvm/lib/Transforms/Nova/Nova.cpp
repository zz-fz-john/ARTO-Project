
#include "Nova.h"

using namespace llvm;
using std::stringstream;

// The MACROs below are for evaluation only, controlling the percentage of instrumented variables
//#define INSTRUMENT_30_PER
//#define INSTRUMENT_50_PER
//#define INSTRUMENT_ALL
//#define INSTRUMENT_HALF

//If none of the macros are enabled, only the key variables in comments will be instrumented.
//#define INSTRUMENT_ALL_FOR_RTOAI //for compare with OPDFI,instrument all 
//#define SMALLTEST //for small test only ,embench ,在测试ardupilot大型项目时，不要开启，会加大分析时间
//#define COPTER_ATTACK 
//#define PX4_PROJECT //for px4 project only
//#define MULTIOPERATION
// statistics
int define_event_count = 0;
int use_event_count = 0;

Type *m_void_ptr_type;
ConstantPointerNull* m_void_null_ptr;
Value* m_infinite_bound_ptr;
std::map<Value*, Value*> m_pointer_base;
std::map<Value*, Value*> m_pointer_bound;
Function* m_spatial_load_dereference_check;
Function* m_spatial_store_dereference_check;
Function* m_load_base_bound_func;
Function* m_metadata_load_vector_func;
Function* m_metadata_store_vector_func;
std::vector<Function* >visitedFunc;
/* Function Type of the function that stores the base and bound
 * for a given pointer
 */
Function* m_store_base_bound_func;

/* Map of all functions defined by Softboundcets */
StringMap<bool> m_func_def_softbound;//stringMap(string,value)
StringMap<bool> m_func_wrappers_available;

/* Map of all functions for which Softboundcets Transformation must
 * be invoked
 */
StringMap<bool> m_func_softboundcets_transform;

/* Map of all functions that need to be transformed as they have as
 * they either have pointer arguments or pointer return type and are
 * defined in the module
 */
StringMap<bool> m_func_to_transform;
std::map<GlobalVariable*, int> m_initial_globals;
std::map<Value*, int> m_present_in_original;
std::map<Value*, int> m_is_pointer;
enum { SBCETS_BITCAST, SBCETS_GEP};

std::map<Value*, Value*> m_vector_pointer_base;
std::map<Value*, Value*> m_vector_pointer_bound;
Type* m_key_type;
SetVector<CriticalDataPoint*> top_dependency_value;//存储最顶层的依赖，应该是一个全局变量。
int CriticalVarIndex=0;//临界点变量的唯一标识
std::vector<std::string> critical_operations;
Function* m_memcopy_check;
Function* m_memset_check;
Function* m_copy_metadata;
Function* m_shadow_stack_allocate;
Function* m_shadow_stack_deallocate;
Function* m_shadow_stack_base_load;
Function* m_shadow_stack_bound_load;
Function* m_shadow_stack_base_store;
Function* m_shadow_stack_bound_store;
Function* m_call_dereference_func;
std::map<int,std::tuple<float,float>> m_cdp_value_range;//存储每个临界点变量的值域范围
Constant* m_constantint32ty_zero;

bool GLOBALCONSTANTOPT = true;//global constant operation
bool BOUNDSCHECKOPT = true; //bounds check operation
bool CALLCHECKS = true;//call check
bool INDIRECTCALLCHECKS = true;//indirect all check
static cl::opt<std::string> NovaIndirectCallFilePath("nova-indirect-call-analysis-filepath",
  cl::desc("Path to the file for indirect call analysis"),
  cl::init(""), cl::value_desc("indirect-filepath"));// indirect call result file

static cl::opt<std::string> NovaDirectCallFilePath("nova-analysis-direct-callfilepath",
  cl::desc("Path to the file for direct call analysis"),
  cl::init(""), cl::value_desc("direct-filepath"));// direct call result file
enum InsertMode {
  CollectValue,
  DefUseCheck1
};
static cl::opt<InsertMode> NovaDynamicCollect(
    "nova-dynamic-collect",
    cl::desc("Choose insertion mode"),
    cl::values(
        clEnumVal(CollectValue, "Insert function to collect sensitive variable values"),
        clEnumVal(DefUseCheck1,  "Insert def-use check")));

//展示如何通过读取全局注释llvm.global.annotations和局部注释call @llvm.var.annotation.*来获取敏感变量, 该函数未调用。
void Nova:: collectNovaSensitiveVars(Module &M, GlobalState *GS,
                                     StringRef Target) {
    if (!GS->senVarSet)
        GS->senVarSet = new ValueSet();

    LLVMContext &Ctx = M.getContext();

    /// --------- 帮助函数：把 i8* GEP/bitcast/constexpr 解析成 std::string ----------
    auto getStringFromValue = [&](Value *V) -> std::string {
        // 1) ConstantExpr<GEP>
        if (auto *CE = dyn_cast<ConstantExpr>(V)) {
            if (CE->getOpcode() == Instruction::GetElementPtr)
                V = CE->getOperand(0);
        }
        // 2) GetElementPtrInst
        else if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
            V = GEP->getOperand(0);
        }

        if (auto *GV = dyn_cast<GlobalVariable>(V)) {
            if (auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer()))
                if (CDA->isString())
                    return CDA->getAsCString().str();   // 自带去掉 '\0'
        }
        return "";
    };

    /// --------- 帮助函数：去掉层层 bitcast / constexpr cast ----------
    auto stripCasts = [](Value *V) -> Value * {
        while (true) {
            if (auto *BC = dyn_cast<BitCastInst>(V))
                V = BC->getOperand(0);
            else if (auto *CE = dyn_cast<ConstantExpr>(V)) {
                if (CE->isCast())
                    V = CE->getOperand(0);
                else
                    break;
            } else
                break;
        }
        return V;
    };

    /* =======================================================================
     *  一、处理全局变量注解  (@llvm.global.annotations)
     * ===================================================================== */
    GlobalVariable *GA = M.getGlobalVariable("llvm.global.annotations");
    if (GA && GA->hasInitializer()) {
        if (auto *CA = dyn_cast<ConstantArray>(GA->getInitializer())) {
            for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
                auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(i));
                if (!CS || CS->getNumOperands() < 2) continue;

                std::string Ann = getStringFromValue(CS->getOperand(1));
                if (Ann.find(Target) == std::string::npos) continue;

                Value *Annotated = stripCasts(CS->getOperand(0));
                GS->senVarSet->insert(Annotated);
                errs() << "[NOVA] global annotated: " << Annotated->getName() << '\n';
            }
        }
    }

    /* =======================================================================
     *  二、处理局部变量注解  (call @llvm.var.annotation.*)
     * ===================================================================== */
    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                auto *CI = dyn_cast<CallInst>(&I);
                if (!CI) continue;

                Function *Callee = CI->getCalledFunction();
                if (!Callee) continue;
                if (!Callee->getName().startswith("llvm.var.annotation"))
                    continue;                         // 不是注解 intrinsic

                if (CI->arg_size() < 2) continue;     // 规范里至少 2 个实际参数

                std::string Ann = getStringFromValue(CI->getArgOperand(1));
                if (Ann.find(Target) == std::string::npos) continue;

                Value *Annotated = stripCasts(CI->getArgOperand(0));
                GS->senVarSet->insert(Annotated);
                errs() << "[NOVA] local  annotated: " << Annotated->getName() << '\n';
            }
        }
    }
}

bool Nova::runOnModule(Module &M) {

    GlobalStateRef GS;
    Function *f = NULL;
    init_pointer_analysis_result();
    // add function definition for constructors and checkers 
    //ConstructCheckHandlers(M);

    // initialize common constants
    //m_void_ptr_type = PointerType::getUnqual(Type::getInt8Ty(M.getContext()));
    //size_t inf_bound = (size_t) pow(2, 48);
    //ConstantInt* infinite_bound = ConstantInt::get(Type::getInt64Ty(M.getContext()), inf_bound, false);
    //m_infinite_bound_ptr = ConstantExpr::getIntToPtr(infinite_bound, m_void_ptr_type);
    //PointerType* vptrty = dyn_cast<PointerType>(m_void_ptr_type);
    //m_void_null_ptr = ConstantPointerNull::get(vptrty);

    GS = new GlobalState();//维护一个全局变量，里面包括TaintMapRef，PointsToMapRef，VisitMapRef，CallInst，ValueSet
    GS->tMap = new TaintMap();
    GS->pMap = new PointsToMap();
    GS->vMap = new VisitMap();
    GS->senVarSet = new ValueSet();

    // initialize pointsto map
    InitializeGS(GS, M);
    //判断是否存在llvm.global.annotations
    bool has_global_annotations=false;
    for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) 
    {
      if (I->getName() == "llvm.global.annotations") {//llvm.global.annotations的保存方式为：all metadata are stored in an array of struct of metadata
          Value * var0 = cast<Value>(I->getOperand(0));
          if (var0->getValueID() == Value::ConstantArrayVal) {
              ConstantArray *ca = cast<ConstantArray>(var0);
              unsigned num = ca->getNumOperands();
              for (unsigned i = 0; i < num; i++) {
                  ConstantStruct *cs = cast<ConstantStruct>(ca->getOperand(i));
                  Value* AnnotationString=cs->getOperand(1);
                  std::string annotation=getStringFromValue(AnnotationString);
                  if(annotation.find("nova_sensitive_var")!=std::string::npos)//__attribute__((annotate("nova_sensitive_var")))
                  {
                      has_global_annotations=true;
                  }
              }
          }
      }
    }
    if(has_global_annotations==true)
    {
      f = M.getFunction("main");
      Traversal(GS,f);
    }
    
    std::ifstream criticalFile("critical_function.txt");
    if (criticalFile.is_open()) {
      std::string line;
      while (std::getline(criticalFile, line)) {
        if (!line.empty()) {
          critical_operations.push_back(line);
        }
      }
      criticalFile.close();
    } else {
      errs() << "Failed to open critical_function.txt\n";
    }
    for(auto &operation :critical_operations)
    {
      f = M.getFunction(operation);
      if (f == NULL)
          errs() << "f is NULL!" <<"\n";
      errs()<<"run Traversal on "<< operation<<" function\n";
      Traversal(GS, f);//从关键操作函数开始遍历
    }
    errs() << "Points To Map:\n";
    // f = M.getFunction("main");
    // Traversal(GS,f);
    // PrintPointsToMap(GS);

    // errs() << "Taint Map:\n";
    // PrintTaintMap(GS);

  // get initial set of sensitive variables annotated by programmer.
  #ifdef INSTRUMENT_ALL_FOR_RTOAI
    // 收集所有全局变量
    for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
        GS->senVarSet->insert(&(*I));
        errs() << "[NOVA] Added global variable: " << I->getName() << "\n";
    }
    
    // 收集所有函数中的局部变量
    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                // 处理 alloca 指令（栈变量）
                if (isa<AllocaInst>(&I)) {
                    GS->senVarSet->insert(&I);
                    errs() << "[NOVA] Added local variable (alloca): " << I.getName() << "\n";
                }
                // 处理所有其他指令产生的值
                else if (I.getType()->isVoidTy() == false) {
                    GS->senVarSet->insert(&I);
                    errs() << "[NOVA] Added instruction value: " << I.getName() << "\n";
                }
            }
        }
        
        // 收集函数参数
        for (Function::arg_iterator AI = F.arg_begin(), AE = F.arg_end(); AI != AE; ++AI) {
            GS->senVarSet->insert(&(*AI));
            errs() << "[NOVA] Added function argument: " << AI->getName() << "\n";
        }
    }
  #else
    GetAnnotatedVariables(M, GS);
  #endif
    // Expand sensitive set via lightweight backward dataflow from definitions
    //ExtendSensitiveByDataflow(M, GS);
      // enforce def-use check
    if(NovaDynamicCollect==DefUseCheck1)
    {
      init_value_range();
    }
    //collect_argument(M);
    DefUseCheck(M, GS);
    errs() << "\ndefine_event_count: " << define_event_count << "\n";
    errs() << "\nuse_event_count: " << use_event_count << "\n";



    return true;
}
void Nova::init_value_range()
{
  //读取文件，文件名为“./critical_var_value_range.txt”，每一行的格式为“变量ID 最小值 最大值”，例如“0 -100.0 100.0”
  std::ifstream infile("critical_var_value_range.txt");
  if (!infile.is_open()) {
    errs() << "Failed to open critical_var_value_range.txt\n";
    return;
  }
  std::string line;
  while (std::getline(infile, line)) {
    std::istringstream iss(line);
    int var_id;
    float min_value, max_value;
    if (!(iss >> var_id >> min_value >> max_value)) {
      errs() << "Error reading line: " << line << "\n";
      continue; // 跳过格式错误的行
    }
    errs()<<"CriticalVarIndex: "<<var_id<<" min_value: "<<min_value<<" max_value: "<<max_value<<"\n";
    m_cdp_value_range[var_id] = std::make_tuple(min_value, max_value);

  }
}
#ifdef BOUDCHECK
void Nova::ConstructCheckHandlers(Module &module){

  Type* void_ty = Type::getVoidTy(module.getContext());
  Type* void_ptr_ty = PointerType::getUnqual(Type::getInt8Ty(module.getContext()));
  Type* size_ty = Type::getInt32Ty(module.getContext());
  Type* ptr_void_ptr_ty = PointerType::getUnqual(void_ptr_ty);
  Type* ptr_size_ty = PointerType::getUnqual(size_ty);
  Type* int32_ty = Type::getInt32Ty(module.getContext());
  m_key_type = Type::getInt32Ty(module.getContext());

  m_constantint32ty_zero = 
    ConstantInt::get(Type::getInt32Ty(module.getContext()), 0);

  module.getOrInsertFunction("__softboundcets_metadata_store", 
                               void_ty, void_ptr_ty, void_ptr_ty, 
                               void_ptr_ty, NULL);

  m_store_base_bound_func = module.getFunction("__softboundcets_metadata_store");
  assert(m_store_base_bound_func && "__softboundcets_metadata_store null?");

  module.getOrInsertFunction("__softboundcets_spatial_load_dereference_check",
                             void_ty, void_ptr_ty, void_ptr_ty, 
                             void_ptr_ty, size_ty, NULL);

  module.getOrInsertFunction("__softboundcets_spatial_store_dereference_check", 
                             void_ty, void_ptr_ty, void_ptr_ty, 
                             void_ptr_ty, size_ty, NULL);
  m_spatial_load_dereference_check =
    module.getFunction("__softboundcets_spatial_load_dereference_check");
  assert(m_spatial_load_dereference_check &&
         "__softboundcets_spatial_load_dereference_check function type null?");

  m_spatial_store_dereference_check = 
    module.getFunction("__softboundcets_spatial_store_dereference_check");
  assert(m_spatial_store_dereference_check && 
         "__softboundcets_spatial_store_dereference_check function type null?");

  module.getOrInsertFunction("__softboundcets_metadata_load", 
                             void_ty, void_ptr_ty, ptr_void_ptr_ty, ptr_void_ptr_ty, NULL);

  m_load_base_bound_func = module.getFunction("__softboundcets_metadata_load");
  assert(m_load_base_bound_func && "__softboundcets_metadata_load null?");

  module.getOrInsertFunction("__softboundcets_metadata_load_vector", 
                             void_ty, void_ptr_ty, ptr_void_ptr_ty, ptr_void_ptr_ty, 
                             ptr_size_ty, ptr_void_ptr_ty, int32_ty, NULL);

  m_metadata_load_vector_func = module.getFunction("__softboundcets_metadata_load_vector");
  assert(m_metadata_load_vector_func && "__softboundcets_metadata_load_vector null?");

  module.getOrInsertFunction("__softboundcets_metadata_store_vector", 
                             void_ty, void_ptr_ty, void_ptr_ty, 
                             void_ptr_ty, size_ty, void_ptr_ty, int32_ty, NULL);

  m_metadata_store_vector_func = module.getFunction("__softboundcets_metadata_store_vector");
  assert(m_metadata_store_vector_func && "__softboundcets_metadata_store_vector null?");


    module.getOrInsertFunction("__softboundcets_memcopy_check",
                               void_ty, void_ptr_ty, void_ptr_ty, size_ty, 
                               void_ptr_ty, void_ptr_ty, void_ptr_ty, void_ptr_ty, NULL);

  m_memcopy_check = 
    module.getFunction("__softboundcets_memcopy_check");
  assert(m_memcopy_check && 
         "__softboundcets_memcopy_check function null?");

    module.getOrInsertFunction("__softboundcets_memset_check",
                               void_ty, void_ptr_ty,size_ty, 
                               void_ptr_ty, void_ptr_ty, NULL);

  m_memset_check = 
    module.getFunction("__softboundcets_memset_check");
  assert(m_memcopy_check && 
         "__softboundcets_memset_check function null?");

  module.getOrInsertFunction("__softboundcets_introspect_metadata", 
                             void_ty, void_ptr_ty, void_ptr_ty, int32_ty, NULL);
  module.getOrInsertFunction("__softboundcets_copy_metadata", 
                             void_ty, void_ptr_ty, void_ptr_ty, size_ty, NULL);

  m_copy_metadata = module.getFunction("__softboundcets_copy_metadata");
  assert(m_copy_metadata && "__softboundcets_copy_metadata NULL?");

  module.getOrInsertFunction("__softboundcets_allocate_shadow_stack_space", 
                             void_ty, int32_ty, NULL);
  module.getOrInsertFunction("__softboundcets_deallocate_shadow_stack_space", 
                             void_ty, NULL);

  m_shadow_stack_allocate = 
    module.getFunction("__softboundcets_allocate_shadow_stack_space");
  assert(m_shadow_stack_allocate && 
         "__softboundcets_allocate_shadow_stack_space NULL?");

  module.getOrInsertFunction("__softboundcets_load_base_shadow_stack", 
                             void_ptr_ty, int32_ty, NULL);
  module.getOrInsertFunction("__softboundcets_load_bound_shadow_stack", 
                             void_ptr_ty, int32_ty, NULL);

  m_shadow_stack_base_load = 
    module.getFunction("__softboundcets_load_base_shadow_stack");
  assert(m_shadow_stack_base_load && 
         "__softboundcets_load_base_shadow_stack NULL?");

  m_shadow_stack_bound_load = 
    module.getFunction("__softboundcets_load_bound_shadow_stack");
  assert(m_shadow_stack_bound_load && 
         "__softboundcets_load_bound_shadow_stack NULL?");

  module.getOrInsertFunction("__softboundcets_store_base_shadow_stack", 
                             void_ty, void_ptr_ty, int32_ty, NULL);
  module.getOrInsertFunction("__softboundcets_store_bound_shadow_stack", 
                             void_ty, void_ptr_ty, int32_ty, NULL);

  m_shadow_stack_base_store = 
    module.getFunction("__softboundcets_store_base_shadow_stack");
  assert(m_shadow_stack_base_store && 
         "__softboundcets_store_base_shadow_stack NULL?");
  
  m_shadow_stack_bound_store = 
    module.getFunction("__softboundcets_store_bound_shadow_stack");
  assert(m_shadow_stack_bound_store && 
         "__softboundcets_store_bound_shadow_stack NULL?");

  m_shadow_stack_deallocate = 
    module.getFunction("__softboundcets_deallocate_shadow_stack_space");
  assert(m_shadow_stack_deallocate && 
         "__softboundcets_deallocate_shadow_stack_space NULL?");

  module.getOrInsertFunction("__softboundcets_stack_memory_deallocation", 
                             void_ty, size_ty, NULL);

  module.getOrInsertFunction("__softboundcets_spatial_call_dereference_check",
                             void_ty, void_ptr_ty, void_ptr_ty, void_ptr_ty, NULL);

  m_call_dereference_func = 
    module.getFunction("__softboundcets_spatial_call_dereference_check");
  assert(m_call_dereference_func && 
         "__softboundcets_spatial_call_dereference_check function null??");

  llvm::FunctionCallee global_init_callee = module.getOrInsertFunction(
      "__softboundcets_global_init", void_ty);
  llvm::Function* global_init = llvm::cast<llvm::Function>(global_init_callee.getCallee());
      

  global_init->setDoesNotThrow();
  global_init->setLinkage(GlobalValue::InternalLinkage);

  BasicBlock* BB = BasicBlock::Create(module.getContext(), 
                                      "entry", global_init);
  
  Function* softboundcets_init = (Function*) module.getOrInsertFunction("__softboundcets_init", void_ty, Type::getInt32Ty(module.getContext()), NULL);
  
  SmallVector<Value*, 8> args;
  Constant * const_one = ConstantInt::get(Type::getInt32Ty(module.getContext()), 1);
  
  args.push_back(const_one);
  Instruction* ret = ReturnInst::Create(module.getContext(), BB);
  
  CallInst::Create(softboundcets_init, args, "", ret);

  Type * Int32Type = IntegerType::getInt32Ty(module.getContext());
  std::vector<Constant *> CtorInits;
  CtorInits.push_back(ConstantInt::get(Int32Type, 0));
  CtorInits.push_back(global_init);
  StructType * ST = ConstantStruct::getTypeForElements(CtorInits, false);
  Constant * RuntimeCtorInit = ConstantStruct::get(ST, CtorInits);

  //
  // Get the current set of static global constructors and add the new ctor
  // to the list.
  //
  std::vector<Constant *> CurrentCtors;
  GlobalVariable * GVCtor = module.getNamedGlobal ("llvm.global_ctors");
  if (GVCtor) {
    if (Constant * C = GVCtor->getInitializer()) {
      for (unsigned index = 0; index < C->getNumOperands(); ++index) {
        CurrentCtors.push_back (dyn_cast<Constant>(C->getOperand (index)));
      }
    }
  }
  CurrentCtors.push_back(RuntimeCtorInit);

  //
  // Create a new initializer.
  //
  ArrayType * AT = ArrayType::get (RuntimeCtorInit-> getType(),
                                   CurrentCtors.size());
  Constant * NewInit = ConstantArray::get (AT, CurrentCtors);

  //
  // Create the new llvm.global_ctors global variable and remove the old one
  // if it existed.
  //
  Value * newGVCtor = new GlobalVariable (module,
                                          NewInit->getType(),
                                          false,
                                          GlobalValue::AppendingLinkage,
                                          NewInit,
                                          "llvm.global_ctors");
  if (GVCtor) {
    newGVCtor->takeName (GVCtor);
    GVCtor->eraseFromParent ();
  }
}
#endif
  // 从 Value 中解析字符串
  std::string Nova::getStringFromValue(Value *v) {
    Value *gep = nullptr;

    // 情况 1：ConstantExpr（常见于 IR 全局初始化中）
    if (auto *ce = dyn_cast<ConstantExpr>(v)) {
        if (ce->getOpcode() == Instruction::GetElementPtr) {
            gep = ce->getOperand(0);
        }
    }

    // 情况 2：GetElementPtrInst（更常见于注释语法编译出来的 IR）
    else if (auto *inst = dyn_cast<GetElementPtrInst>(v)) {
        gep = inst->getOperand(0);
    }

    // 解析全局字符串变量
    if (auto *gv = dyn_cast<GlobalVariable>(gep)) {
        if (auto *cda = dyn_cast<ConstantDataArray>(gv->getInitializer())) {
            if (cda->isString()) {
                return cda->getAsCString().str(); // 自动去掉 \00
            }
        }
    }

    return "";
}
// get initial set of sensitive variables annotated by programmer
void Nova::GetAnnotatedVariables(Module &M, GlobalStateRef gs) {
    Value *var0, *var1;
    ConstantArray *ca;
    ConstantStruct *cs;
    unsigned num, i;

    for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
        if (I->getName() == "llvm.global.annotations") {//llvm.global.annotations的保存方式为：all metadata are stored in an array of struct of metadata
            var0 = cast<Value>(I->getOperand(0));
            if (var0->getValueID() == Value::ConstantArrayVal) {
                ca = cast<ConstantArray>(var0);
                num = ca->getNumOperands();
                for (i = 0; i < num; i++) {
                    cs = cast<ConstantStruct>(ca->getOperand(i));
                    Value* AnnotationString=cs->getOperand(1);
                    std::string annotation=getStringFromValue(AnnotationString);
                    if(annotation.find("nova_sensitive_var")!=std::string::npos)//__attribute__((annotate("nova_sensitive_var")))
                    {
                      errs()<<"find nova_sensitive_var annotation\n";
                      var1 = cs->getOperand(0)->getOperand(0);//被注释的变量在第一个字段中，注释内容在第二个字段中
                      gs->senVarSet->insert(var1);
                      CriticalDataPointRef cdp = new struct CriticalDataPoint();
                      cdp->var = var1;
                      cdp->type = 0; // 0标识是手动注释添加
                      top_dependency_value.insert(cdp);
                      errs()<<"llvm.global.annotations: var" << i << " : " << var1->getName() << "\n";
                    }
                }
            }
        }
    }
    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (CallInst *CI = dyn_cast<CallInst>(&I)) {
                    Function *calledFunc = CI->getCalledFunction();
                    if (!calledFunc) continue;

                    // 检查是不是 llvm.var.annotation
                    if (calledFunc->getName().startswith("llvm.var.annotation")) {
                        Value *annotatedVar = CI->getArgOperand(0);
                        Value *annotationStrVal = CI->getArgOperand(1);

                        std::string annotation = getStringFromValue(annotationStrVal);
                        errs() << "Annotation found: " << annotation << "\n";

                        if (annotation == "nova_sensitive_var") {
                            // bitcast 回原变量（如 %5）
                            if (auto *bitcast = dyn_cast<BitCastInst>(annotatedVar)) {
                                Value *realVar = bitcast->getOperand(0);
                                gs->senVarSet->insert(realVar);
                                CriticalDataPointRef cdp = new struct CriticalDataPoint();
                                cdp->var = realVar;
                                cdp->type = 0; // 0标识是手动注释添加
                                top_dependency_value.insert(cdp);
                                errs() << "Annotated variable: " << *realVar << "\n";
                            }
                        }
                    }
                }
            }
        }
    }


    return;
}

// Lightweight backward dataflow expansion:
// If a sensitive variable is defined by a store "store X, &SEN" (normal var)
// or by a pointer-store path "load TMP, &SEN; store X, TMP" (pointer var),
// then mark X as sensitive too. Repeat to a small fixed point within the module.
//有问题，无法找到最顶层依赖
void Nova::ExtendSensitiveByDataflow(Module &M, GlobalStateRef gs) {
  if (!gs || !gs->senVarSet) return;

  ValueSet worklist; // reuse SetVector API
  for (auto *v : *(gs->senVarSet)) worklist.insert(v);

  auto enqueue = [&](Value *v) {
    if (!v) return;
    if (!gs->senVarSet->count(v)) {
      gs->senVarSet->insert(v);
      worklist.insert(v);
    }
  };

  unsigned iter = 0, maxIter = 3;
  while (!worklist.empty() && iter++ < maxIter) {
    // drain current frontier
    std::vector<Value*> current(worklist.begin(), worklist.end());
    worklist.clear();

    for (Value *sv : current) {
      for (User *U : sv->users()) {
        if (auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() == sv) {
            Value *rhs = SI->getValueOperand();
            if (!isa<Constant>(rhs)) enqueue(rhs);
          }
        } else if (auto *LI = dyn_cast<LoadInst>(U)) {
          if (LI->getPointerOperand() == sv) {
            for (User *UL : LI->users()) {
              if (auto *SI2 = dyn_cast<StoreInst>(UL)) {
                if (SI2->getPointerOperand() == LI) {
                  Value *rhs2 = SI2->getValueOperand();
                  if (!isa<Constant>(rhs2)) enqueue(rhs2);
                }
              }
            }
          }
        }
      }
    }
  }

  errs() << "[NOVA] ExtendSensitiveByDataflow done. Total sensitive count: "
       << gs->senVarSet->size() << "\n";
}

bool Nova::SkipStructType(Type *type) {
	StructType *st = cast<StructType>(type);

        //errs() << "struct type name: " << type->getStructName() << "\n";
	assert(type != NULL);

	if (!(st->isLiteral()) && type->getStructName().str().compare(0,10,"struct.IO_")) {
        	// errs() << "note! rely on external struct _IO_FILE/_IO_marker, 
                // only stop creating nested AliasObject for such type!\n";
		return true;
	} else
		return false;
}

void Nova::InitializeGS(GlobalStateRef gs, Module &M) {
    GlobalValue *gv;
    Type *type = NULL;
    AliasObjectRef aor = NULL;
    AliasObjectTupleRef aot = NULL;
    TupleSet *ts = NULL;
    InstSet *is = NULL;

    //errs() <<__func__<<" : \n";
//遍历module中的全局变量
    for (Module::global_iterator s = M.global_begin(), 			\
          e = M.global_end(); s != e; ++s) {
        gv = &(*s);

	// filter out .str***, stderr
	//if (SkipGlobalValue(gv))
	//	continue;
        // errs() << "&&&&&&&&&&&&" <<gv->getGlobalIdentifier() << "\n";
        type = gv->getType();
        // errs() << "GlobalValue gv typeID: " << type->getTypeID() << "\n";
        // errs() << "GlobalValue gv pointee typeID: " << type->getPointerElementType()->getTypeID() << "\n";
        aor = CreateAliasObject(type->getPointerElementType(), gv);

        // create alias object tuple ，创建别名对象
        aot = new struct AliasObjectTuple();
        aot->offset = 0;
        aot->ao = aor;

        // create tuple set，创建元组集合
        ts = new TupleSet();
        ts->insert(aot);

        // add ele into points to map，将全局变量映射到其对应的元组集合，这是指针分析的基础数据结构
        (*(gs->pMap))[gv] = ts;

        // initialize global taint map
        is = new InstSet();
        (*(gs->tMap))[gv] = is;
    }

    return;
}
// 使用 LLVM 的 scc_iterator 遍历函数中的 SCC（强连通分量）。
// 每个 SCC：
// 拷贝到你自定义的 SCC 容器。
// 反转该 SCC 中基本块的顺序。
// 把这些 SCC 加入 sccVector。
// 最后整体反转整个 sccVector。
// 这样做的一个潜在用途是：
// 实现 逆向遍历控制流图的拓扑排序，例如在做程序分析时（如数据流分析、依赖分析等）从底向上处理 SCC。
// 或者为循环结构等提供某种处理顺序
void Nova::ReverseSCC(std::vector<SCCRef> &sccVector, Function *f) {
    for (scc_iterator<Function *> I = scc_begin(f), //在函数的控制流图（CFG）中遍历 强连通分量
                                  IE = scc_end(f);
                                  I != IE; ++I) 
                                  {
        const std::vector<BasicBlock *> &SCCBBs = *I;//// 获取当前SCC(强连通分量）中的所有基本块。
        SCCRef tmpSCC = new SCC();//创建新的SCC对象

        for (SCC::const_iterator BBI = SCCBBs.begin(),
                                 BBIE = SCCBBs.end();
                                 BBI != BBIE; ++BBI) {//将当前 SCC 中的基本块复制进 tmpSCC。
            tmpSCC->push_back(*BBI);
        }

        reverse(tmpSCC->begin(), tmpSCC->end());//.第一次反转 - 反转SCC内部基本块顺序
        sccVector.push_back(tmpSCC);
    }
    
    reverse(sccVector.begin(), sccVector.end());// 第二次反转 - 反转SCC之间的顺序

    return;
}

uint32_t Nova::LongestUseDefChain(SCC &scc) {
    // TODOO
    return MAX_RUNS;
}

void Nova::HandleLoop(GlobalStateRef gs, SCC &scc) {
    uint32_t i, numRuns = LongestUseDefChain(scc);

    i = 0;
    while (i < numRuns) {
        //errs() << "SCC: ";
        VisitSCC(gs, scc);
        //errs() << "\n";
        i++;
    }

    return;
}
void Nova::init_pointer_analysis_result()
{
    // 读取文件内容
    std::string line;
    //正则表达式
    std::regex pattern_indirect_callsite(R"(In function\s*:(\S+)\s*indirect callsite\s*:\s*(\d+))");
    std::regex pattern_direct_callsite(R"(In function\s*:(\S+)\s*direct callsite\s*:\s*(\d+))");
    std::regex pattern_target(R"(--target\s*(\S+))");
    std::string func_name;
    std::pair<std::string,int> callsite;
    if (!NovaIndirectCallFilePath.empty()) {
        std::ifstream file(NovaIndirectCallFilePath.c_str());
        if (file.is_open()) {
            while (std::getline(file, line)) {
                // 处理文件内容
                //llvm::errs() << "Read line: " << line << "\n";
                if(std::regex_match(line,pattern_indirect_callsite))
                {
                    std::smatch result;
                    std::regex_search(line,result,pattern_indirect_callsite);
                    func_name=result[1];
                    int id=std::stoi(result[2]);
                    callsite=std::make_pair(func_name,id);
                }
                else if(std::regex_match(line,pattern_target))
                {
                    std::smatch result;
                    std::regex_search(line,result,pattern_target);
                    std::string target_name=result[1];
                    Nova::indirect_call_graph[callsite].insert(target_name);
                }

            }
            file.close();
        } else {
            llvm::errs() << "Failed to open file: " <<NovaIndirectCallFilePath << "\n";
        }
    }
    if(!NovaDirectCallFilePath.empty()){
        std::ifstream file(NovaDirectCallFilePath.c_str());
        if (file.is_open()) {
            while (std::getline(file, line)) {
                // 处理文件内容
                //llvm::errs() << "Read line: " << line << "\n";
                if(std::regex_match(line,pattern_direct_callsite))
                {
                    std::smatch result;
                    std::regex_search(line,result,pattern_direct_callsite);
                    func_name=result[1];
                    int id=std::stoi(result[2]);
                    callsite=std::make_pair(func_name,id);
                }
                else if(std::regex_match(line,pattern_target))
                {
                    std::smatch result;
                    std::regex_search(line,result,pattern_target);
                    std::string target_name=result[1];
                    Nova::direct_call_graph[callsite].insert(target_name);
                }

            }
            file.close();
        } else {
            llvm::errs() << "Failed to open file: " << NovaDirectCallFilePath << "\n";
        }
    }
}
//得到call指令目标地址
Function *Nova::ResolveCall(GlobalStateRef gs, CallInst &I) {//得到call指令目标地址
    // TODOO indirect call should be handled later
    if ((*(gs->vMap))[&I])//如果已经访问了
        return NULL;
    else {
        // mark f's callsite as visited
        (*(gs->vMap))[&I] = true;
        return I.getCalledFunction();
    }
}
Function *Nova ::ResolveBitcastCall(GlobalStateRef gs, CallInst &I) {
  // TODOO indirect call should be handled later
  if ((*(gs->vMap))[&I])//如果已经访问了
    return NULL;
  else 
  {
    // mark f's callsite as visited
    (*(gs->vMap))[&I] = true;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(I.getCalledOperand())) {
      if (CE->isCast()) {
        Value *V = CE->getOperand(0);
        if (Function *F = dyn_cast<Function>(V)) {
          return F;
        }
      }
    }
  }
}
std::vector<Function *> Nova::ResolveIndirectCall(GlobalStateRef gs, CallInst &I) {
  std::vector<Function *> calleeList;
  if ((*(gs->vMap))[&I])//如果已经访问了
      return calleeList;
  if(I.isIndirectCall()) {
    (*(gs->vMap))[&I]=true;
    Module* M=I.getModule();
    llvm::Metadata *incallID = I.getMetadata("inCallID");
    if (auto *md = llvm::dyn_cast<llvm::MDNode>(incallID)) {
        if (auto *constMeta = llvm::dyn_cast<llvm::ConstantAsMetadata>(md->getOperand(0))) {
            if (auto *constInt = llvm::dyn_cast<llvm::ConstantInt>(constMeta->getValue())) {
                int ID = constInt->getZExtValue();
                errs()<<"ID is : " << ID << "\n";
                std::pair<std::string,int> key(I.getFunction()->getName().str(),ID);
                for(auto it =Nova::indirect_call_graph[key].begin();it!=Nova::indirect_call_graph[key].end();it++){
                    Function *callee=M->getFunction(*it);
                    if(callee){
                        calleeList.push_back(callee);
                        //errs() << "indirect call: " << I.getFunction()->getName() << " : " << *it << "\n";
                    }
                }
            }
        }
    }
  }
  return calleeList;
}
//建立call指令中的实参和函数中的形参之间的指向关系，call指令中的操作数的value对应的TupleSet 与 函数参数的value对应的TupleSet相同
void Nova::InitializeFunction(GlobalStateRef gs, Function *f, CallInst &I) {
    Value *var;
    TupleSet *ts = NULL;
    InstSet *is = NULL;

    //errs() <<__func__<<" : \n";

    // record current callinst to help handle ret inst
    gs->ci = &I;
  //同时遍历函数的参数和对应调用指令的参数。
    CallInst::op_iterator argit = I.arg_begin(), argie = I.arg_end();//op_iterator 用于遍历函数调用指令的操作数列表
    Function::arg_iterator it = f->arg_begin(), ie = f->arg_end();//arg_iterator,用于遍历函数的参数列表
    for (;(argit != argie) && (it != ie); ++it, ++argit) {
        var = cast<Value>(&(*it));
        // // 【修改点】：增加类型检查，只有当参数是指针类型时才建立别名关系
        // if (!var->getType()->isPointerTy()) {
        //     continue;
        // }
        // copy args'tMap and pMap to funciton params' one by one
        if (gs->pMap->find(*argit) != gs->pMap->end()) {
            if (gs->pMap->find(*argit) == gs->pMap->end()) {//未找到
                continue;
            }
            #if !defined(SMALLTEST) && !defined(PX4_PROJECT) //如果是px4项目或者small test项目，则不启用该代码
            //debug for 2025.12.26 enable for ardupilot
            if (gs->tMap->find(*argit) == gs->tMap->end()) {//未找到
                continue;
            }
            //end debug for 2025.12.26 enable for ardupilot
            #endif
            //将call指令目标函数的参数同call指令的操作数建立映射，使call指令中的操作数的value对应的TupleSet 与 函数参数的value对应的TupleSet相同
            ts = (*(gs->pMap))[*argit];
            (*(gs->pMap))[var] = ts;

            if (gs->tMap->find(*argit) != gs->tMap->end()) {
                is = (*(gs->tMap))[*argit];
                (*(gs->tMap))[var] = is;
            }
        }
    }

    return;
}

void Nova::HandleCall(GlobalStateRef gs, CallInst &I) {
    Value *var;
    Function *f;
    if ((*(gs->vMap))[&I])//如果已经访问了
      return ;
    if(I.getCalledFunction() != NULL)
    {
      f = ResolveCall(gs, I);
      if (f != NULL && f->getName() == "llvm.var.annotation") 
      {
        //llvm.var.annotation(arg0, arg1, arg2, arg3), arg0 is the annotated variable
        var = I.getArgOperand(0);//返回第0个参数的值
        gs->senVarSet->insert(var);
        errs() << "llvm.var.annotation: arg0: " << var->getName() << " \n";
      }
      else if (f != NULL) 
      {
        //errs() << "\ncall inst: " << I << "\n";
        //errs() << "called func: " << f->getName() << "\n";

        // handle parameters and global variables
        if (!f->isDeclaration()) 
        {//如果不是声明,则处理
            InitializeFunction(gs, f, I);
            // if (std::find(visitedFunc.begin(), visitedFunc.end(), f) != visitedFunc.end()) {
            //     return;
            // }
            Traversal(gs, f);
        }
      }
    }
    else if(ConstantExpr *ConstEx = dyn_cast_or_null<ConstantExpr>(I.getCalledOperand())){
      if(auto *CE = dyn_cast<ConstantExpr>(I.getCalledOperand()))
      {
        if (CE->isCast())
        {
          f= ResolveBitcastCall(gs, I);
          if (f != NULL && f->getName() == "llvm.var.annotation") 
          {
            //llvm.var.annotation(arg0, arg1, arg2, arg3), arg0 is the annotated variable
            var = I.getArgOperand(0);//返回第0个参数的值
            gs->senVarSet->insert(var);
            errs() << "llvm.var.annotation: arg0: " << var->getName() << " \n";
          }
          else if (f != NULL) 
          {
            //errs() << "\ncall inst: " << I << "\n";
            //errs() << "called func: " << f->getName() << "\n";

            // handle parameters and global variables
            if (!f->isDeclaration()) 
            {//如果不是声明,则处理
                InitializeFunction(gs, f, I);
                Traversal(gs, f);
            }
          }
        }
      }

    }
    if(I.isIndirectCall() )//处理间接调用
    {
        errs() << "indirect call inst: " << I << "\n";
        std::vector<Function *> callee = ResolveIndirectCall(gs, I);
        if (callee.size() > 0) {
            for (auto it = callee.begin(); it != callee.end(); it++) {
                if (!(*it)->isDeclaration()) {
                    InitializeFunction(gs, *it, I);
                    Traversal(gs, *it);
                }
            }
        }
    }
    (*(gs->vMap))[&I]=true;
    return;

}

AliasObject *Nova::CreateAliasObject(Type *type, Value *v) {  
    // std::cout<<"-----------10------------"<<std::endl;

    unsigned i;
    Type *subtype;
    AliasObjectRef loc = NULL;// a location object for stack var 
    AliasObjectRef aor = NULL, ao = NULL;
    AliasObjectSet *aos = NULL;

    //errs() <<__func__<<" value: "<< *v << " name: " << v->getName() << " typeID: "<< type->getTypeID() << "\n";

    assert(type != NULL);

    // std::cout<<"-----------11------------"<<std::endl;

    // create a alias object 
    aor = new struct AliasObject();
    // std::cout<<"-----------12------------"<<std::endl;

    aor->val = v;
    aor->type = type;
    // std::cout<<"-----------13------------"<<std::endl;

    // if(type == NULL){
    //   std::cout << "xxxxxxxxxxxxxxxxxxxxxxxxx" << std::endl;
    // }
    aor->isStruct = type->isStructTy();
    aor->isLocation = false;
    // std::cout<<"-----------14------------"<<std::endl;

    aor->aliasMap = new AliasMap();
    // std::cout<<"-----------15------------"<<std::endl;

    aor->taintMap = new LocalTaintMap();
    // std::cout<<"-----------16------------"<<std::endl;

    if (type->isStructTy() && SkipStructType(type)) {
    	aor->isStruct = false;
    }

    if (type->isStructTy() && !SkipStructType(type)) {

        i = 0;
        for (Type::subtype_iterator it = type->subtype_begin(), ie = type->subtype_end();//遍历结构体中每个元素的类型
                                                                it != ie; ++it, ++i) {
            // alias object set
            aos = new AliasObjectSet();

            // recursively create aliasobject for struct type
            //errs() <<__func__<<" struct field: "<<(*it)->getTypeID()<<"\n";
            ao = CreateAliasObject(*it, v);
            aos->insert(ao);

            // initialize aliasMap at loc i 
            (*(aor->aliasMap))[i] = aos;
        }
    } else if (type->isPointerTy()) {

            // alias object set
            aos = new AliasObjectSet();

            // recursively create aliasobject for pointer type
            subtype = type->getPointerElementType();
            ao = CreateAliasObject(subtype, v);
            aos->insert(ao);

            // initialize aliasMap at loc 0 
            (*(aor->aliasMap))[0] = aos;
    } else {

        // alias object set
        aos = new AliasObjectSet();

        // create a location object for stack var 
        loc = new struct AliasObject();
        loc->val = v;
        loc->type = NULL;
        loc->isStruct = false;
        loc->isLocation = true;
        loc->aliasMap = NULL;
        loc->taintMap = NULL;

        aos->insert(loc);

        // initialize aliasMap at loc 0
        (*(aor->aliasMap))[0] = aos;
    }

    return aor;
}
//输出AliasObject的type
void Nova::PrintAliasObject(AliasObjectRef ao) {
    unsigned i, size;

    if (ao->aliasMap == NULL) {
        errs() << __func__ << ": aliasMap is NULL\n";
        return;
    }

    if (ao->isStruct) {
        //errs() << __func__ << ":" << "struct  " << ao->type->getTypeID() << " { " << "\n"; 
        size = ao->aliasMap->size();
        for (i = 0; i < size; i++) {
            PrintAliasObject((*((*(ao->aliasMap))[i]))[0]);
        }
        //errs() << __func__ << ":" << "}\n";
    } else if (ao->type->isPointerTy()) {
            //errs() << __func__ << ":" << "typeID: " << ao->type->getTypeID() << "\n"; 
            PrintAliasObject((*((*(ao->aliasMap))[0]))[0]);
    } else {
            //errs() << __func__ << ":" << "typeID: " << ao->type->getTypeID() << "\n"; 
    }
}
//将该指令对应的TupleSet加入到pMap中
void Nova::UpdatePtoAlloca(GlobalStateRef gs, Instruction &I){
    // std::cout<<"-----------0------------"<<std::endl;

    AllocaInst *ai = NULL;
    Type *type = NULL;
    AliasObjectRef aor = NULL;
    AliasObjectTupleRef aot = NULL;
    TupleSet *ts = NULL;

    //errs() <<__func__<<" : "<<I<<"\n";

    // assert(ai = cast<AllocaInst>(&I));
    ai = cast<AllocaInst>(&I);
    assert(ai != NULL);

    type = ai->getAllocatedType();/// Return the type that is being allocated by the instruction.


    //errs() << "AllocaInst typeID: " << ai->getType()->getTypeID() << "\n";
    //errs() << "AllocaInst pointee typeID: " << ai->getType()->getPointerElementType()->getTypeID() << "\n";
    //errs() << "AllocaInst allocated typeID: " << type->getTypeID() << "\n";
    // std::cout<<"-----------1------------"<<std::endl;

    aor = CreateAliasObject(type, &I); //根据内存分配指令为分配的对象创建一个别名
    //comment start
    // std::cout<<"-----------2------------"<<std::endl;

    // debug
    PrintAliasObject(aor);
    // create alias object tuple

    aot = new struct AliasObjectTuple();
    aot->offset = 0;
    aot->ao = aor;

    // create tuple set
    ts = new TupleSet();
    ts->insert(aot);

    // add ele into points to map
    (*(gs->pMap))[&I] = ts;
    //comment end
    return;
}

void Nova::UpdatePtoBinOp(GlobalStateRef gs, Instruction &I){
    TupleSet *ts1, *ts2, *ts;
    Value *op1, *op2;
    #ifdef COPTER_ATTACK
    // 只处理指针类型
    if (!I.getType()->isPointerTy()) {
        return;
    }
    #endif
    //errs() <<__func__<<" : "<<I<<"\n";

    op1 = I.getOperand(0);
    op2 = I.getOperand(1);

    // get tuple set for op1
    if (gs->pMap->find(op1) != gs->pMap->end()) {
        ts1 = (*(gs->pMap))[op1];
    } else {
        ts1 = NULL;
    }

    // get tuple set for op2
    if (gs->pMap->find(op2) != gs->pMap->end()) {
        ts2 = (*(gs->pMap))[op2];
    } else {
        ts2 = NULL;
    }

    // new tuple set is for v
    if (gs->pMap->find(&I) != gs->pMap->end()) {
        ts = (*(gs->pMap))[&I];
    } else {
        ts = new TupleSet();
        (*(gs->pMap))[&I] = ts;
    }

    // merge ts1, ts2 into ts
    if (ts1 != NULL) {
        for (TupleSet::iterator it = ts1->begin(), ie = ts1->end();
                                    it != ie; ++it) {
            ts->insert(*it);
        }
    }

    if (ts2 != NULL) {
        for (TupleSet::iterator it = ts2->begin(), ie = ts2->end();
                                    it != ie; ++it) {
            ts->insert(*it);
        }
    }

    return;
}

void Nova::UpdatePtoLoad(GlobalStateRef gs, Instruction &I){
    LoadInst *li;
    Value *op;
    TupleSet *ts, *nts;
    AliasObjectTupleRef aot;
    AliasObjectSet *aos;
    AliasMapRef aliasMap;
    std::stringstream stream;
    uint32_t offset;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand
    // assert(li = cast<LoadInst>(&I));
    li = cast<LoadInst>(&I);//获取load指令的操作数
    op = li->getPointerOperand();
  #ifdef COPTER_ATTACK
    if(!I.getType()->isPointerTy()) {
        return;
    }
  #endif
    // get tupleset
    if (gs->pMap->find(op) == gs->pMap->end()) {
         return;
    }
    ts = (*(gs->pMap))[op];
    if (ts == NULL)
        return;
    
    // create new tupleset for loadinst, llvm IR is in ssa form, so every load defines a new tmp var
    nts = new TupleSet();
    for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                                tsit != tsie; ++tsit) {
        aliasMap = (*tsit)->ao->aliasMap;
        offset = (*tsit)->offset;

        //assert(aliasMap->find(offset) != aliasMap->end());
        //errs() << __func__ << "(*tsit)->offset:" << (*tsit)->offset << "\n";
        //errs() << __func__ <<": aliasMap :" << aliasMap<<"\n";
        if (aliasMap == NULL) {
            errs() << "aliasMap is NULL\n";
            return;
        }

        if (aliasMap == nullptr || aliasMap->find(offset) == aliasMap->end()) {
            return;
        }
    
        // create a new tuple
        aos = (*aliasMap)[offset];
        for(AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                           aosit != aosie; ++aosit) {
            aot = new struct AliasObjectTuple();
            aot->offset = 0;
            aot->ao = (*aosit);

            // insert new tuple into a new tupleset 
            nts->insert(aot);
        }
    } 

    // add ele into points to map
    (*(gs->pMap))[&I] = nts;

    return;
}

void Nova::HandlePtoGEPOperator(GlobalStateRef gs, GEPOperator *gop) {//getelementptr 指令用于计算聚合数据结构中子元素的地址。
    Value *op, *idx1, *idx2;
    uint32_t i, off;
    TupleSet *ts, *nts;
    AliasObjectTupleRef aot;

    // get operand
    op = gop->getPointerOperand();
    //errs() << __func__ << " op: " << *op << "\n";
    //errs() << __func__ << " op typeid: " << op->getType()->getTypeID() << "\n";
    //errs() << __func__ << " op pointee typeid: " << op->getType()->getPointerElementType()->getTypeID() << "\n";

    // getelementptr inst is fairly complex, we only handle basic struct type and array here
    // for struct: we go into the struct to fetch the aliasobject at offset, it's possible
    //             because we create aliasobject for members of struct, refer to  CreateAliasObject
    // for array: we don't go into the array elements, cause we didn't create aliasobject for each
    //            element because of the possible overhead. we treat array as a whole object. we
    //            comprimise precision for lower overhead.
    i = 0;
    idx1 = NULL;
    idx2 = NULL;
    for(User::op_iterator it = gop->idx_begin(), ie = gop->idx_end();
                                                it != ie; ++it, ++i) {
        // idx = *it;
        if (i == 0)
            idx1 = *it;//数据结构的类型
        else if (i == 1)
            idx2 = *it;//数据结构的基地址
        //errs() << "idx : " << *idx << "\n";
    }

    // TODOO :just a rough handling of offset 
    //assert (isa<ConstantInt>(idx1));
    if (!isa<ConstantInt>(idx1)) {
        return;
    }

    off = 0;
    if (idx2 != NULL && isa<ConstantInt>(idx2)) {
        off = (uint32_t)((cast<ConstantInt>(idx2))->getZExtValue());
        //errs() << "off: " << off << "\n";
    }

    // get tupleset
    if (gs->pMap->find(op) == gs->pMap->end()) {
        //errs() << __func__ << " error: gd->pMap->find(op) == gs->pMap->end(), give up trying!" << "\n";
        return;
    }

    ts = (*(gs->pMap))[op];

    if (ts == NULL)
        return;
    assert(op->getType()->isPointerTy());
    if (op->getType()->getPointerElementType()->isArrayTy()) {
        // for array type, we copy op's ts into gop's ts
        //errs() << __func__ << ": handle array " << "\n";

        // add ele into points to map
        (*(gs->pMap))[gop] = ts;

    } else if (op->getType()->getPointerElementType()->isStructTy()) {//判断op是否是一个结构体
        // for struct type, we fetch struct member and put it into gop's ts
        //errs() << __func__ << ": handle struct" << "\n";

        // create new tupleset for gep
        // NOTE: we don't dereference op here, op should always be the start address of the struct
        nts = new TupleSet();
        for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                                    tsit != tsie; ++tsit) {
            aot = new struct AliasObjectTuple();
            aot->offset = off;
            aot->ao = (*tsit)->ao;

            // debug
            //errs() << __func__ << ": new tuple : off = " << off << " ao->val: " << (*tsit)->ao->val <<"\n";
            // print op'ao
            PrintAliasObject((*tsit)->ao);

            // insert new tuple into a new tupleset 
            nts->insert(aot);
        } 

        // add ele into points to map
        (*(gs->pMap))[gop] = nts;
    }

    return;
}

void Nova::UpdatePtoStore(GlobalStateRef gs, Instruction &I){
    StoreInst *si;
    GEPOperator *gepop;
    Value *op, *v;
    TupleSet *ots, *vts;
    AliasObjectSet *aos, *vaos;
    AliasMapRef aliasMap;
    AliasObjectRef ao;
    uint32_t offset;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand
    // assert(si = cast<StoreInst>(&I));
    si = cast<StoreInst>(&I);
    op = si->getPointerOperand();// 目标地址
    v = si->getValueOperand();
    #ifdef COPTER_ATTACK
    //debug on 2025.12.18 enbale for ardupilot attack
    if (!v->getType()->isPointerTy()) {
        return;
    }
    //end debug on 2025.12.18
    #endif
    // if v is constant integer or function parameter, nothing to do here
    if (gs->pMap->find(v) == gs->pMap->end()) {
        //errs() << "val operand has not pMap entry, so it is constant or funciton prarmeter!\n";
        return;
    }

    // if op is gepoperator, call HandlePtoGEPOperator first
    if (isa<GEPOperator>(op)) {//用于获取指针类型数据结构（例如数组或结构体）中的元素的地址。
        //errs() << "op operand is GEPOperator!\n";
        gepop = dyn_cast<GEPOperator>(op);
        HandlePtoGEPOperator(gs, gepop);
    }

    // get tupleset
    if (gs->pMap->find(op) == gs->pMap->end()) {
        errs() << __func__ << ": gs->pMap->find(op) can't find it!\n";
        return;
    }

    ots = (*(gs->pMap))[op];
    vts = (*(gs->pMap))[v];

    if (ots == NULL || vts == NULL) { 
        errs() << __func__ << ":" << "ots or vts is NULL";
        return;
    }
    
    // update aliasobject points-to by operand op

    // get operand v's AliasObject set
    vaos = new AliasObjectSet();
    for (TupleSet::iterator vtsit = vts->begin(), vtsie = vts->end();
                                                vtsit != vtsie; ++vtsit) {
        ao = (*vtsit)->ao;//ao是一个AliasObject
        vaos->insert(ao);

    }


    // get operand op's value's AliasObject set, note, we dereference op here
    for (TupleSet::iterator otsit = ots->begin(), otsie = ots->end();
                                                otsit != otsie; ++otsit) {
        aliasMap = (*otsit)->ao->aliasMap;
        offset = (*otsit)->offset;

        //errs() << __func__ << ": tuple : off = " << offset << " ao->val: " << *((*otsit)->ao->val) <<"\n";
        // print op'ao
        PrintAliasObject((*otsit)->ao);
                              
        if (aliasMap == NULL) {
            errs() << __func__ << ": aliasMap is Null\n";
            return;
        }
        assert(aliasMap);
        //assert(aliasMap->find(offset) != aliasMap->end());
        if (aliasMap->find(offset) == aliasMap->end()) {
           return;
        }

        // copy v's aliasobject into op's value's aliasobject
        // TODOO we lose the v's ao'offset info here
        aos = (*aliasMap)[offset];
        for(AliasObjectSet::iterator aosit = vaos->begin(), aosie = vaos->end();
                                                           aosit != aosie; ++aosit) {
            aos->insert(*aosit);
        }
    } 

    // release allocated tempary objects
    delete vaos;

    return;
}

void Nova::UpdatePtoGEP(GlobalStateRef gs, Instruction &I){
    GetElementPtrInst *gepi;
    Value *op, *idx2;
    uint32_t i, off;
    TupleSet *ts, *nts;
    AliasObjectTupleRef aot;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand
    // assert(gepi = cast<GetElementPtrInst>(&I));
    gepi = cast<GetElementPtrInst>(&I);
    op = gepi->getPointerOperand();
    //errs() << " gep address space :" <<gepi->getAddressSpace() << "\n";

    i = 0;
    idx2 = NULL;
    for(User::op_iterator it = gepi->idx_begin(), ie = gepi->idx_end();
                                                it != ie; ++it, ++i) {
        // idx = *it;
        if (i == 1)
            idx2 = *it;
        //errs() << "idx : " << *idx << "\n";
    }

    //errs() << "idx1:" << (uint64_t)idx1 << "\n";

    // TODOO :just a rough handling of offset 
    //assert (isa<ConstantInt>(idx1));
    //if (!isa<ConstantInt>(idx1)) {
    //    errs() << __func__ << " skip instruction : " << I << "\n"; 
    //    return;
    //}

    off = 0;
    //errs() << "idx2:" << (uint64_t)idx2 << "\n";
    if(idx2 != NULL && isa<ConstantInt>(idx2)) {
        off = (uint32_t)((cast<ConstantInt>(idx2))->getZExtValue());
        //errs() << "off: " << off << "\n";
    }

    // get tupleset
    //assert(gs->pMap->find(op) != gs->pMap->end());
    if (gs->pMap->find(op) == gs->pMap->end()) {
        //errs() << __func__ << " skip instruction : " << I << "\n"; 
        return;
    }
    ts = (*(gs->pMap))[op];

    if (ts == NULL)
        return;

    assert(op->getType()->isPointerTy());

    if (op->getType()->getPointerElementType()->isArrayTy()) {
        // for array type, we copy op's ts into gop's ts
        //errs() << __func__ << ": handle array " << "\n";

        // add ele into points to map
        (*(gs->pMap))[&I] = ts;

    } else if (op->getType()->getPointerElementType()->isStructTy()) {
        // for struct type, we fetch struct member and put it into gop's ts
        //errs() << __func__ << ": handle struct" << "\n";

        // create new tupleset for gep
        // NOTE: we don't dereference op here, op should always be the start address of the struct
        nts = new TupleSet();
        for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                                    tsit != tsie; ++tsit) {
            aot = new struct AliasObjectTuple();
            aot->offset = off;
            aot->ao = (*tsit)->ao;

            // debug
            //errs() << __func__ << ": new tuple : off = " << off << " ao->val: " << (*tsit)->ao->val <<"\n";
            // print op'ao
            PrintAliasObject((*tsit)->ao);

            // insert new tuple into a new tupleset 
            nts->insert(aot);
        } 

        // add ele into points to map
        (*(gs->pMap))[&I] = nts;
    } else {
        // errs() << __func__ << " nearly skip instruction : " << I << "\n"; 
        (*(gs->pMap))[&I] = ts;
    }
    
    return;
}

void Nova::UpdatePtoRet(GlobalStateRef gs, Instruction &I){
    ReturnInst *ri;
    Value *ret, *ci;
    TupleSet *ts = NULL, *nts = NULL;

    // get operand
    // assert(ri = cast<ReturnInst>(&I));
    ri = cast<ReturnInst>(&I);
    ret = ri->getReturnValue();
    // 检查返回值是否存在
    if (!ret) {
        // 函数返回 void，无需处理指针元数据
        return;
    }
    
    // 检查返回值是否为指针类型
    if (!ret->getType()->isPointerTy()) {
        // 返回值不是指针，不需要传播 points-to 信息
        return;
    }
    // get tuple set
    if (gs->pMap->find(ret) != gs->pMap->end())
        ts = (*(gs->pMap))[ret];

    // get current callInst and the corresponding tuple set
    ci = gs->ci;
    if (gs->pMap->find(ci) != gs->pMap->end())
        nts = (*(gs->pMap))[ci];

    if (ts != NULL) {
        if (nts != NULL) {
            // merge ts and nts
            for (TupleSet::iterator it = ts->begin(), ie = ts->end();
                                            it != ie; ++it ) {
                nts->insert(*it);
            }
        } 
        else {
            // assign ts to ci's points-to map, thus ci will carry the ret's info back to caller
            (*(gs->pMap))[ci] = ts;
        }
    }

    return;
}

void Nova::UpdatePtoBitCast(GlobalStateRef gs, Instruction &I){
    BitCastInst *bci;
    Value *op;
    TupleSet *ts = NULL;

    // get operand
    // assert(bci = cast<BitCastInst>(&I));
    bci = cast<BitCastInst>(&I);
    op = bci->getOperand(0);

    // get tuple set
    if (gs->pMap->find(op) != gs->pMap->end())
        ts = (*(gs->pMap))[op];

    // for cast inst, the target and src share the same tuple set
    if (gs->pMap->find(bci) != gs->pMap->end()) {
        //errs() << __func__ <<" pMap(bci) is not null : " << I << "\n";
    }

    (*(gs->pMap))[bci] = ts;
}

void Nova::UpdateTaintAlloca(GlobalStateRef gs, Instruction &I){
    // TODOO
    //errs() <<__func__<<" : "<<I<<"\n";
}

void Nova::UpdateTaintBinOp(GlobalStateRef gs, Instruction &I){
    InstSet *is1, *is2, *is;
    Value *op1, *op2;

    //errs() <<__func__<<" : "<<I<<"\n";

    op1 = I.getOperand(0);
    op2 = I.getOperand(1);

    // get instset for op1
    if (gs->tMap->find(op1) != gs->tMap->end()) {
        is1 = (*(gs->tMap))[op1];
    } else {
        is1 = NULL;
    }

    // get instset for op2
    if (gs->tMap->find(op2) != gs->tMap->end()) {
        is2 = (*(gs->tMap))[op2];
    } else {
        is2 = NULL;
    }

    // new instset is for v
    if (gs->tMap->find(&I) != gs->tMap->end()) {
        is = (*(gs->tMap))[&I];
    } else {
        is = new InstSet();
        (*(gs->tMap))[&I] = is;
    }

    // merge is1, is2 into is
    if (is1 != NULL) {
        for (InstSet::iterator it = is1->begin(), ie = is1->end();
                                    it != ie; ++it) {
            is->insert(*it);
        }
    }

    if (is2 != NULL) {
        for (InstSet::iterator it = is2->begin(), ie = is2->end();
                                    it != ie; ++it) {
            is->insert(*it);
        }
    }

    is->insert(&I);


    return;
}

void Nova::UpdateTaintLoad(GlobalStateRef gs, Instruction &I){
    LoadInst *li;
    Value *op;
    TupleSet *ts;
    LocalTaintMapRef taintMap;
    InstSet *is, *bis, *obis;
    uint32_t offset;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand
    // assert(li = cast<LoadInst>(&I));
    li = cast<LoadInst>(&I);
    op = li->getPointerOperand();

    // get tupleset
    //assert(gs->pMap->find(op) != gs->pMap->end());
    if (gs->pMap->find(op) == gs->pMap->end()) {
        return;
    }
    ts = (*(gs->pMap))[op];
    if (ts == NULL)
        return;
    
    // collect local instset from all aliasobject's local taintmap, put them into big instset bis
    bis = new InstSet();
    for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                                tsit != tsie; ++tsit) {
        taintMap = (*tsit)->ao->taintMap;
        offset = (*tsit)->offset;
        if (taintMap == NULL) {
            errs() <<__func__ <<  ":tainMap is NULL\n";
            return;
        }
        // if there's no taintMap at offset , create one
        if (taintMap->find(offset) != taintMap->end()) {
            is = (*taintMap)[offset];
        } else {
            is = new InstSet();
            (*taintMap)[offset] = is;
        }

        // iterate over local taintmap's instset is
        for(InstSet::iterator it = is->begin(), ie = is->end();
                                                it != ie; ++it) {
           bis->insert(*it);
        }
    }

    bis->insert(&I);

    // merge old bis with new bis
    if (gs->tMap->find(&I) != gs->tMap->end()) {
        obis = (*(gs->tMap))[&I];
        for (InstSet::iterator it = obis->begin(), ie = obis->end();
                                                it != ie; ++it) {
            bis->insert(*it);
        }
    } 

    // add (I, bis) into taint map
    (*(gs->tMap))[&I] = bis;

    return;
}

void Nova::UpdateTaintStore(GlobalStateRef gs, Instruction &I) {
    StoreInst *si;
    Value *v, *op;
    GEPOperator *gepop;
    TupleSet *ts;
    LocalTaintMapRef taintMap;
    InstSet *vis, *is;
    uint32_t offset;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand v, op
    // assert(si = cast<StoreInst>(&I));
    si = cast<StoreInst>(&I);
    v = si->getValueOperand();
    op = si->getPointerOperand();

    // get v's taint map
    if (gs->tMap->find(v) != gs->tMap->end()) {
            vis = (*(gs->tMap))[v];
    } else {
        //errs() << __func__ << " : operand val's no tMap, it may be constant value or constant parameter!\n";
        vis = NULL;
    }

    // if op is gepoperator, call HandlePtoGEPOperator first
    if (isa<GEPOperator>(op)) {
        //errs() << "op operand is GEPOperator!\n";
        gepop = dyn_cast<GEPOperator>(op);
        HandlePtoGEPOperator(gs, gepop);
    }
    
    // get tupleset
    if (gs->pMap->find(op) == gs->pMap->end()) {
        //errs() << __func__ << " error: gd->pMap->find(op) == gs->pMap->end(), give up trying!" << "\n";
        return;
    }
    ts = (*(gs->pMap))[op];
    if (ts == NULL)
        return;

    // spread v's taint map to all op's aliasobject's local taint map
    for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                                tsit != tsie; ++tsit) {
        taintMap = (*tsit)->ao->taintMap;
        offset = (*tsit)->offset;
        if (taintMap == NULL) {
             errs() << "taintMap is NULL\n";
            return;
        } 

        // if taintMap at ao'offset does not exist, create one.
        assert(taintMap != NULL);
        if (taintMap->find(offset) == taintMap->end()) {
            is = new InstSet();
            (*taintMap)[offset] = is;
        } else {
            is = (*taintMap)[offset];
        }

        // merge v's taintmap into op's aliasobject's local taintmap at offset
        if (vis != NULL) {
            for(InstSet::iterator it = vis->begin(), ie = vis->end();
                                                    it != ie; ++it) {
                is->insert(*it);
            }
        }

        // include this storeinst I
        is->insert(&I);
    }

    return;
}

void Nova::UpdateTaintGEP(GlobalStateRef gs, Instruction &I) {
    GetElementPtrInst *gepi;
    Value *op;
    InstSet *is1, *is;

    //errs() <<__func__<<" : "<<I<<"\n";

    // get operand
    // assert(gepi = cast<GetElementPtrInst>(&I));
    gepi = cast<GetElementPtrInst>(&I);
    op = gepi->getPointerOperand();

    // get instset for op
    if (gs->tMap->find(op) != gs->tMap->end()) {
        is1 = (*(gs->tMap))[op];
    } else {
        is1 = NULL;
    }

    // new instset is for v
    is = new InstSet();

    // merge is1, is2 into is
    if (is1 != NULL) {
        for (InstSet::iterator it = is1->begin(), ie = is1->end();
                                    it != ie; ++it) {
            is->insert(*it);
        }
    }

    // don't forget this GEP inst
    is->insert(&I);

    // add (I, is) into taint map
    //assert(gs->tMap->find(&I) == gs->tMap->end());
    (*(gs->tMap))[&I] = is;

    return;
}

void Nova::UpdateTaintRet(GlobalStateRef gs, Instruction &I){
    ReturnInst *ri;
    Value *ret, *ci;
    InstSet *is = NULL, *nis = NULL; // NOTE! we must initialize stack variable, it might has non-zero initial value!!!

    // get operand
    // assert(ri = cast<ReturnInst>(&I));
    ri = cast<ReturnInst>(&I);
    ret = ri->getReturnValue();

    // get ret's inst set
    if (gs->tMap->find(ret) != gs->tMap->end())
        is = (*(gs->tMap))[ret];

    // get current callInst and the corresponding tuple set
    ci = gs->ci;
    if (gs->tMap->find(ci) != gs->tMap->end())
        nis = (*(gs->tMap))[ci];

    if (is != NULL) {
        if (nis != NULL) {
            // merge is and nis
            for (InstSet::iterator it = is->begin(), ie = is->end();
                                            it != ie; ++it ) {
                nis->insert(*it);
            }
        } else {
            // assign ts to ci's points-to map, thus ci will carry the ret's info back to caller
            (*(gs->tMap))[ci] = is;
        }
    }

    return;
}

void Nova::UpdateTaintBitCast(GlobalStateRef gs, Instruction &I){
    BitCastInst *bci;
    Value *op;
    InstSet *is = NULL;

    // get operand
    // assert(bci = cast<BitCastInst>(&I));
    bci = cast<BitCastInst>(&I);
    op = bci->getOperand(0);

    // get inst set
    if (gs->tMap->find(op) != gs->tMap->end())
        is = (*(gs->tMap))[op];
    else
        is = new InstSet();

    is->insert(&I);

    // for cast inst, the target and src share the same tuple set
    if (gs->tMap->find(bci) != gs->tMap->end()) {
        //errs() << __func__ <<" tMap(bci) is not null : " << I << "\n";
    }

    (*(gs->tMap))[bci] = is;
}

void Nova::PointsToAnalysis(GlobalStateRef gs, Instruction &I) {
  //isa判断是否是一个类的实例
  //判断该指令是否是在堆栈上分配内存的指令
  //errs()<<"run points to analysis for instruction: "<<I<<"\n";
    if (isa<AllocaInst>(&I)) {
        UpdatePtoAlloca(gs, I);
    } else if (I.isBinaryOp()){//判断是否是二进制操作
        UpdatePtoBinOp(gs, I);
    } else if (isa<LoadInst>(&I)){//判断是否是从内存中读取数据的指令
        UpdatePtoLoad(gs, I);
    } else if (isa<StoreInst>(&I)){//判断是否是往内存中存储数据的指令
        UpdatePtoStore(gs, I);
    } else if (isa<GetElementPtrInst>(&I)){/// an instruction for type-safe pointer arithmetic to access elements of arrays and structs
        UpdatePtoGEP(gs, I);
    } else if (isa<ReturnInst>(&I)){///// Return a value (possibly void), from a function. 
        UpdatePtoRet(gs, I);
    } else if (isa<BitCastInst>(&I)){//This class represents a no-op cast from one type to another.
        UpdatePtoBitCast(gs, I);
    } else {
        // Not handled inst
        //errs() <<"Unhandled Inst: "<<I<<"\n";
    }
}

//更新gs中tMap的指向关系
void Nova::TaintAnalysis(GlobalStateRef gs, Instruction &I) {
  //errs()<<"run taint analysis for instruction: "<<I<<"\n";
    if (isa<AllocaInst>(&I)) {
        UpdateTaintAlloca(gs, I);
    } else if (I.isBinaryOp()){
        UpdateTaintBinOp(gs, I);
    } else if (isa<LoadInst>(&I)){
        UpdateTaintLoad(gs, I);
    } else if (isa<StoreInst>(&I)){
        UpdateTaintStore(gs, I);
    } else if (isa<GetElementPtrInst>(&I)){
        UpdateTaintGEP(gs, I);
    } else if (isa<ReturnInst>(&I)){
        UpdateTaintRet(gs, I);
    } else if (isa<BitCastInst>(&I)){
        UpdateTaintBitCast(gs, I);
    } else {
        // Not handled inst
        //errs() <<"Unhandled Inst: "<<I<<"\n";
    }
}

void Nova::DispatchClients(GlobalStateRef gs, Instruction &I) {
    //errs()<<"run dispatch clients for instruction: "<<I<<"\n";
    PointsToAnalysis(gs, I);
    TaintAnalysis(gs, I);
}

void Nova::VisitSCC(GlobalStateRef gs, SCC &scc) {//遍历scc
    for (SCC::iterator BBI = scc.begin(),
                       BBIE = scc.end();
                       BBI != BBIE; ++BBI) {
        //errs() << (*BBI)->getName() << " ";
        for (Instruction &I: *(*BBI)) {
            if (auto *callInst = dyn_cast<CallInst>(&I)) {
                HandleCall(gs, *callInst);
            } else {
                DispatchClients(gs, I);
            }
        }
    }
    return;
}
void Nova::CollectOperandsDependencies(GlobalStateRef gs, Value *V, std::set<Value*> &visited) {
    if (!V) return;
    
    // 将条件变量本身加入敏感变量集合
    gs->senVarSet->insert(V);
    
    // 如果是比较指令（icmp, fcmp等），收集其直接操作数
    if (auto *CmpI = dyn_cast<CmpInst>(V)) {
        errs() << "  -> Found CmpInst: " << *CmpI << "\n";
        for (unsigned i = 0; i < CmpI->getNumOperands(); ++i) {
            Value *Op = CmpI->getOperand(i);
            gs->senVarSet->insert(Op);
            errs() << "    -> Operand " << i << ": " << *Op << "\n";
        }
    }
}
void Nova::Traversal(GlobalStateRef gs, Function *f) {
    //判断visitedFunc中是否存在f
    // if (std::find(visitedFunc.begin(), visitedFunc.end(), f) != visitedFunc.end()) {
    //   return;
    // }
    // visitedFunc.push_back(f);
    std::vector<SCCRef> sccVector;
    for (auto &BB : *f) {
          // 获取基本块的终止指令 (Terminator)
          Instruction *Term = BB.getTerminator();

          if (!Term) continue;

          // 情况 1: 处理条件跳转 (if-else, loops)
          if (auto *BI = dyn_cast<BranchInst>(Term)) {
              if (BI->isConditional()) {
                  Value *Cond = BI->getCondition();
                  gs->senVarSet->insert(Cond);
                  errs() << "[Found] Conditional Branch in BB: " << BB.getName() << "\n";
                  #if defined(SMALLTEST) ||defined(COPTER_ATTACK)
                  //debug for 2025.12.26 enable for small test
                  std::set<Value*> visited;
                  CollectOperandsDependencies(gs, Cond, visited);
                  //end debug for 2025.12.26
                  #endif
              }
          }
          // 情况 2: 处理 Switch 语句
          else if (auto *SI = dyn_cast<SwitchInst>(Term)) {
              Value *Cond = SI->getCondition();
              gs->senVarSet->insert(Cond);
              errs() << "[Found] Switch Statement in BB: " << BB.getName() << "\n";
              errs() << "  -> Switch Control Var: " << *Cond << "\n";
              #if defined(SMALLTEST) ||defined(COPTER_ATTACK)
               //debug for 2025.12.26 enable for small test
              std::set<Value*> visited;
              CollectOperandsDependencies(gs, Cond, visited);
              //end debug for 2025.12.26
              #endif
          }
    }
    ReverseSCC(sccVector, f);//反转sccVector，在反转sccVector时，会首先将sccVector中的basic block也反转了

    for (std::vector<SCCRef>::iterator it = sccVector.begin(), 
                                       ie = sccVector.end();
                                       it != ie; ++it) {//遍历连通分量的集合
        // Is loop ?
        if ((*it)->size() > 1) {//循环中,基本块的数量大于1
            HandleLoop(gs, *(*it));
        } else {
            //errs() << "SCC: ";
            VisitSCC(gs, *(*it));
            //errs() << "\n";
        }
    }

    return;
}

void Nova::PrintPointsToMap(GlobalStateRef gs) {
    AliasMapRef aliasMap;
    unsigned offset;
    AliasObjectSet *aos;

    for (PointsToMap::iterator it = gs->pMap->begin(), ie = gs->pMap->end();
                                                         it != ie; ++it) { 
        if ((it)->second == NULL) {
            errs() << "it->second is NULL!" << "\n";
            continue;
        }

        errs() << it->first->getName() << " : " << "\n";
        for (TupleSet::iterator tsit = it->second->begin(), tsie = it->second->end();
                                                    tsit != tsie; ++tsit) {
            if ((*tsit)->ao == NULL) {
                errs() << "tsit->ao is NULL!" << "\n";
                continue;
            }

            errs() << "(" << (*tsit)->offset << ", " << (*tsit)->ao->val->getName() << ")" << "\n";
            aliasMap = (*tsit)->ao->aliasMap;
            offset = (*tsit)->offset;
            if (aliasMap != NULL && aliasMap->find(offset) != aliasMap->end()) {
                aos = (*aliasMap)[offset];
                if (aos == NULL) {
                    errs() << "aos is NULL!" << "\n";
                    continue;
                }
                errs() << "alias object set: ";
                for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                    aosit != aosie; ++aosit) {
                    errs() << (*aosit)->val->getName() << ",";
                }
                errs() << "\n";
            } else {
                errs() << "location object ?" << "\n";
            }

            if ((*tsit)->ao->type != NULL && (*tsit)->ao->type->isStructTy()) {
                // print struct field aliasMap
                errs() << "struct internal alias map: \n";
                for (AliasMap::iterator ait = aliasMap->begin(), aie = aliasMap->end();
                                                ait != aie; ++ait) {
                    offset = ait->first;
                    aos = ait->second;
                    if (aos == NULL) {
                        errs() << "aos is NULL!" << "\n";
                        continue;
                    }
                    errs() << "alias object set at offset " << offset << " : ";
                    for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                        aosit != aosie; ++aosit) {
                        errs() << (*aosit)->val->getName() << ",";
                    }
                    errs() << "\n";
                }
            }
        }
        errs() << "\n";
    }
}

void Nova::PrintTaintMap(GlobalStateRef gs) {
    LocalTaintMapRef taintMap;
    AliasObjectRef ao;
    unsigned offset;
    InstSet *is;

    for (TaintMap::iterator it = gs->tMap->begin(), ie = gs->tMap->end();
                                                        it != ie; ++it) { 
        errs() << it->first->getName() << " : " << "\n";
        for (InstSet::iterator isit = it->second->begin(), isie = it->second->end();
                                                    isit != isie; ++isit) {
            if ((*isit) != NULL)
                errs() << *(*isit) << "\n";
            else {
                errs() << "(*isit) == NULL!\n";
                continue;
            }
        }

        if (gs->pMap->find(it->first) != gs->pMap->end()) {
            if (((*(gs->pMap))[it->first]) == NULL || ((*(gs->pMap))[it->first])->empty()) {
                errs() << "pMap[it->first] == empty !\n";
                ao = NULL;
            } else {
                ao = (*(*(gs->pMap))[it->first])[0]->ao;
            }
        } else {
            ao = NULL;
        }

        if (ao!= NULL) {
            taintMap = ao->taintMap;
        } else {
            taintMap = NULL;
        }

        offset = 0;
        if (taintMap != NULL && taintMap->find(offset) != taintMap->end()) {
            is = (*taintMap)[offset];
            errs() << "alias object local taint trace: \n";
            for (InstSet::iterator iit = is->begin(), iie = is->end();
                                                iit != iie; ++iit) {
                errs() << *(*iit) << "\n";
            }
            errs() << "\n";
        } else {
            errs() << "location object ?" << "\n";
        }

        errs() << "\n";

        if (ao != NULL && taintMap != NULL && ao->type != NULL && ao->type->isStructTy()) {
            // print struct field taintMap
            errs() << "struct internal alias map: \n";
            for (LocalTaintMap::iterator tit = taintMap->begin(), tie = taintMap->end();
                                            tit != tie; ++tit) {
                offset = tit->first;
                is = tit->second;
                errs() << "locat taint map at offset " << offset << " : " << "\n";
                for (InstSet::iterator iit = is->begin(), iie = is->end();
                                                    iit != iie; ++iit) {
                    errs() << *(*iit) << "\n";
                }
                errs() << "\n";
            }
        }
    }
}

// for pointer type, we get boundary info when it's initialized
// it could be initialized by malloc, getAddrOf array type, or others.
// step : find its definition
// step : instrument its definition to collect boundary info
// step : check pointer-based read&write
void Nova::PointerAccessCheck(Module &M, Value *v) {
}

BasicBlock::iterator Nova::GetInstIterator(Value *v) {
    Instruction *inst;
    BasicBlock *bb;

    inst = dyn_cast<Instruction>(v);
    assert(inst && "v should be a instruction!");
    bb = inst->getParent();

    for (BasicBlock::iterator i = bb->begin(), ie = bb->end(); i != ie; ++i) {
        if (&(*i) == inst)
            return i;
    }

    assert( 0 && "Can't find inst in its parent basicblock!");
    return bb->end();
}

//
// Method: getSizeOfType 
// 
// Description: This function returns the size of the memory access
// based on the type of the pointer which is being dereferenced.  This
// function is used to pass the size of the access in many checks to
// perform byte granularity checking.
//
// Comments: May we should use TargetData instead of m_is_64_bit
// according Criswell's comments.
 
//org_code base on llvm 4.0
// Value* Nova::GetSizeOfType(Type* input_type) {

//   // Create a Constant Pointer Null of the input type.  Then get a
//   // getElementPtr of it with next element access cast it to unsigned
//   // int
   
//   const PointerType* ptr_type = dyn_cast<PointerType>(input_type);

//   if (isa<FunctionType>(ptr_type->getNonOpaquePointerElementType())) {
//       return ConstantInt::get(Type::getInt64Ty(ptr_type->getContext()), 0);
//   }

//   const SequentialType* seq_type = dyn_cast<SequentialType>(input_type);
//   Constant* int64_size = NULL;
//   if (!seq_type) {
//     if(input_type->isSized()){
//       return ConstantInt::get(Type::getInt64Ty(input_type->getContext()), 0);
//     }
//   }
//   assert(seq_type && "pointer dereference and it is not a sequential type\n");
  
//   StructType* struct_type = dyn_cast<StructType>(input_type);

//   if(struct_type){
//     if(struct_type->isOpaque()){
//         return ConstantInt::get(Type::getInt64Ty(seq_type->getContext()), 0);        
//     }
//   }
  
//   if(!seq_type->getElementType()->isSized()){
//     return ConstantInt::get(Type::getInt64Ty(seq_type->getContext()), 0);
//   }
//   int64_size = ConstantExpr::getSizeOf(seq_type->getElementType());
//   return int64_size;
// }
//add code
Value* Nova::GetSizeOfType(Type* input_type) {
  // 如果是指针类型并指向 FunctionType，返回 0
  if (auto *ptr_type = dyn_cast<PointerType>(input_type)) {
    if (isa<FunctionType>(ptr_type->getNonOpaquePointerElementType())) {
      return ConstantInt::get(Type::getInt32Ty(ptr_type->getContext()), 0);
    }

    // 获取指针指向的类型
    input_type = ptr_type->getNonOpaquePointerElementType();
  }

  // 如果是 struct 且 opaque，返回 0
  if (auto *struct_type = dyn_cast<StructType>(input_type)) {
    if (struct_type->isOpaque()) {
      return ConstantInt::get(Type::getInt32Ty(input_type->getContext()), 0);
    }
  }

  // 如果类型不可计算大小，返回 0
  if (!input_type->isSized()) {
    return ConstantInt::get(Type::getInt32Ty(input_type->getContext()), 0);
  }

  // 最终返回 sizeof(input_type)
  return ConstantExpr::getSizeOf(input_type);
}
// 
// Method: castToVoidPtr()
// **Borrowed from SoftBoundsCETS
//
// Description: 
// 
// This function introduces a bitcast instruction in the IR when an
// input operand that is a pointer type is not of type i8*. This is
// required as all the SoftBound/CETS handlers take i8*s
//

Value* Nova::CastToVoidPtr(Value* operand, Instruction* insert_at) {

  Value* cast_bitcast = operand;
  if (operand->getType() != m_void_ptr_type) {
    cast_bitcast = new BitCastInst(operand, m_void_ptr_type,
                                   "bitcast",
                                   insert_at);
  }
  return cast_bitcast;
}

//
// Method: dissociateBaseBound
// **Borrowed from SoftboundCETS
//
// Description: This function removes the base/bound metadata
// associated with the pointer operand in the SoftBound/CETS maps.

void Nova::DissociateBaseBound(Value* pointer_operand){

  if(m_pointer_base.count(pointer_operand)){
    m_pointer_base.erase(pointer_operand);
  }
  if(m_pointer_bound.count(pointer_operand)){
    m_pointer_bound.erase(pointer_operand);
  }
  assert((m_pointer_base.count(pointer_operand) == 0) && 
         "dissociating base failed\n");
  assert((m_pointer_bound.count(pointer_operand) == 0) && 
         "dissociating bound failed");
}

//
// Method: associateBaseBound
// **Borrowed from SoftboundCETS
//
// Description: This function associates the base bound with the
// pointer operand in the SoftBound/CETS maps.


void Nova::AssociateBaseBound(Value* pointer_operand, 
                              Value* pointer_base, 
                              Value* pointer_bound){

  if(m_pointer_base.count(pointer_operand)){
    DissociateBaseBound(pointer_operand);
  }

  if(pointer_base->getType() != m_void_ptr_type){
    assert(0 && "base does not have a void pointer type ");
  }
  m_pointer_base[pointer_operand] = pointer_base;

  if(m_pointer_bound.count(pointer_operand)){
    assert(0 && "bound map already has an entry in the map");
  }
  if(pointer_bound->getType() != m_void_ptr_type) {
    assert(0 && "bound does not have a void pointer type ");
  }
  m_pointer_bound[pointer_operand] = pointer_bound;
}

//
// Methods: getAssociatedBase, getAssociatedBound, getAssociatedKey,
// getAssociatedLock
//
// Description: Retrieves the metadata from SoftBound/CETS maps 
//

Value* Nova::GetAssociatedBase(Value* pointer_operand) {
    

  if(isa<Constant>(pointer_operand)){
    Value* base = NULL;
    Value* bound = NULL;
    Constant* ptr_constant = dyn_cast<Constant>(pointer_operand);
    GetConstantExprBaseBound(ptr_constant, base, bound);

    if(base->getType() != m_void_ptr_type){
      Constant* base_given_const = dyn_cast<Constant>(base);
      assert(base_given_const!=NULL);
      Constant* base_const = ConstantExpr::getBitCast(base_given_const, m_void_ptr_type);
      return base_const;
    }
    return base;
  }

  if(!m_pointer_base.count(pointer_operand)){
    pointer_operand->dump();
  }
  assert(m_pointer_base.count(pointer_operand) && 
         "Base absent. Try compiling with -simplifycfg option?");
    
  Value* pointer_base = m_pointer_base[pointer_operand];
  assert(pointer_base && "base present in the map but null?");

  if(pointer_base->getType() != m_void_ptr_type)
    assert(0 && "base in the map does not have the right type");

  return pointer_base;
}

Value* Nova::GetAssociatedBound(Value* pointer_operand) {

  if(isa<Constant>(pointer_operand)){
    Value* base = NULL;
    Value* bound = NULL;
    Constant* ptr_constant = dyn_cast<Constant>(pointer_operand);
    GetConstantExprBaseBound(ptr_constant, base, bound);

    if(bound->getType() != m_void_ptr_type){
      Constant* bound_given_const = dyn_cast<Constant>(bound);
      assert(bound_given_const != NULL);
      Constant* bound_const = ConstantExpr::getBitCast(bound_given_const, m_void_ptr_type);
      return bound_const;
    }

    return bound;
  }

    
  assert(m_pointer_bound.count(pointer_operand) && 
         "Bound absent.");
  Value* pointer_bound = m_pointer_bound[pointer_operand];
  assert(pointer_bound && 
         "bound present in the map but null?");    

  if(pointer_bound->getType() != m_void_ptr_type)
    assert(0 && "bound in the map does not have the right type");

  return pointer_bound;
}

Instruction* Nova:: GetGlobalInitInstruction(Module& module){
  Function* global_init_function = module.getFunction("__softboundcets_global_init");    
  assert(global_init_function && "no __softboundcets_global_init function??");    
  Instruction *global_init_terminator = NULL;
  bool return_inst_flag = false;
  for(Function::iterator fi = global_init_function->begin(), fe = global_init_function->end(); fi != fe; ++fi) {
      
    BasicBlock* bb = dyn_cast<BasicBlock>(fi);
    assert(bb && "basic block null");
    Instruction* bb_term = dyn_cast<Instruction>(bb->getTerminator());
    assert(bb_term && "terminator null?");
      
    if(isa<ReturnInst>(bb_term)) {
      assert((return_inst_flag == false) && "has multiple returns?");
      return_inst_flag = true;
      global_init_terminator = dyn_cast<ReturnInst>(bb_term);
      assert(global_init_terminator && "return inst null?");
    }
  }
  assert(global_init_terminator && "global init does not have return, strange");
  return global_init_terminator;
}

// Method: handleGlobalStructTypeInitializer()
//
// Description: handles the global
// initialization for global variables which are of struct type and
// have a pointer as one of their fields and is globally
// initialized 
//
// Comments: This function requires review and rewrite
void Nova::HandleGlobalStructTypeInitializer(Module& module, 
                                  StructType* init_struct_type,
                                  Constant* initializer, 
                                  GlobalVariable* gv, 
                                  std::vector<Constant*> indices_addr_ptr, 
                                  int length) {
  
  // TODOO:URGENT: Do I handle nesxted structures
  //errs() << __func__ <<"\n";
  
  // has zero initializer 
  if(initializer->isNullValue())
    return;
    
  Instruction* first = GetGlobalInitInstruction(module);
  unsigned num_elements = init_struct_type->getNumElements();
  Constant* constant = dyn_cast<Constant>(initializer);
  assert(constant && 
         "[handleGlobalStructTypeInit] global stype with init but not CA?");

  for (unsigned i = 0; i < num_elements; i++) {
    Type* element_type = init_struct_type->getElementType(i);  // 直接获取元素类型

    if (isa<PointerType>(element_type)) {
        Value* initializer_opd = constant->getOperand(i);
        Value* operand_base = nullptr;
        Value* operand_bound = nullptr;
        Constant* addr_of_ptr = nullptr;

        Constant* given_constant = dyn_cast<Constant>(initializer_opd);
        assert(given_constant && 
                "[handleGlobalStructTypeInitializer] not a constant?");
        
        GetConstantExprBaseBound(given_constant, operand_base, operand_bound);   

        Constant* index2 = ConstantInt::get(Type::getInt32Ty(module.getContext()), i);
        indices_addr_ptr.push_back(index2);
        length++;

        addr_of_ptr = ConstantExpr::getGetElementPtr(nullptr, gv, indices_addr_ptr);

        Type* initializer_type = initializer_opd->getType();
        Value* initializer_size = GetSizeOfType(initializer_type);     

        AddStoreBaseBoundFunc(addr_of_ptr, operand_base, 
                              operand_bound, initializer_opd, 
                              initializer_size, first);

        indices_addr_ptr.pop_back();
        length--;

        continue;
    }

    if (isa<StructType>(element_type)) {
        StructType* child_element_type = dyn_cast<StructType>(element_type);
        Constant* struct_initializer = dyn_cast<Constant>(constant->getOperand(i));

        Constant* index2 = ConstantInt::get(Type::getInt32Ty(module.getContext()), i);
        indices_addr_ptr.push_back(index2);
        length++;

        HandleGlobalStructTypeInitializer(module, child_element_type, 
                                          struct_initializer, gv, 
                                          indices_addr_ptr, length); 

        indices_addr_ptr.pop_back();
        length--;
        continue;
    }
  }      
}

//
// Method: getConstantExprBaseBound
//
// Description: This function uniform handles all global constant
// expression and obtains the base and bound for these expressions
// without introducing any extra IR modifications.

void Nova::GetConstantExprBaseBound(Constant* given_constant, 
                                             Value* & tmp_base,
                                             Value* & tmp_bound){


  if(isa<ConstantPointerNull>(given_constant)){
    tmp_base = m_void_null_ptr;
    tmp_bound = m_void_null_ptr;
    return;
  }
  
  ConstantExpr* cexpr = dyn_cast<ConstantExpr>(given_constant);
  tmp_base = NULL;
  tmp_bound = NULL;
    

  if(cexpr) {

    assert(cexpr && "ConstantExpr and Value* is null??");
    switch(cexpr->getOpcode()) {
        
    case Instruction::GetElementPtr:
      {
        Constant* internal_constant = dyn_cast<Constant>(cexpr->getOperand(0));
        GetConstantExprBaseBound(internal_constant, tmp_base, tmp_bound);
        break;
      }
      
    case BitCastInst::BitCast:
      {
        Constant* internal_constant = dyn_cast<Constant>(cexpr->getOperand(0));
        GetConstantExprBaseBound(internal_constant, tmp_base, tmp_bound);
        break;
      }
    case Instruction::IntToPtr:
      {
        tmp_base = m_void_null_ptr;
        tmp_bound = m_void_null_ptr;
        return;
        break;
      }
    default:
      {
        break;
      }
    } // Switch ends
    
  } else {
      
    const PointerType* func_ptr_type = 
      dyn_cast<PointerType>(given_constant->getType());
      
    if(isa<FunctionType>(func_ptr_type->getNonOpaquePointerElementType())) {
      tmp_base = m_void_null_ptr;
      tmp_bound = m_infinite_bound_ptr;
      return;
    }
    // Create getElementPtrs to create the base and bound 

    std::vector<Constant*> indices_base;
    std::vector<Constant*> indices_bound;
      
    GlobalVariable* gv = dyn_cast<GlobalVariable>(given_constant);


    // TODO: External globals get zero base and infinite_bound 

    if(gv && !gv->hasInitializer()) {
      tmp_base = m_void_null_ptr;
      tmp_bound = m_infinite_bound_ptr;
      return;
    }

    Constant* index_base0 = 
      Constant::
      getNullValue(Type::getInt32Ty(given_constant->getType()->getContext()));

    Constant* index_bound0 = 
      ConstantInt::
      get(Type::getInt32Ty(given_constant->getType()->getContext()), 1);

    indices_base.push_back(index_base0);
    indices_bound.push_back(index_bound0);

    Constant* gep_base = ConstantExpr::getGetElementPtr(nullptr,
							given_constant, 
                                                        indices_base);    
    Constant* gep_bound = ConstantExpr::getGetElementPtr(nullptr,
							 given_constant, 
                                                         indices_bound);
      
    tmp_base = gep_base;
    tmp_bound = gep_bound;      
  }
}

//
// Method: addStoreBaseBoundFunc
//
// Description:
//
// This function inserts metadata stores into the bitcode whenever a
// pointer is being stored to memory.
//
// Inputs:
//
// pointer_dest: address where the pointer being stored
//
// pointer_base, pointer_bound, pointer_key, pointer_lock: metadata
// associated with the pointer being stored
//
// pointer : pointer being stored to memory
//
// size_of_type: size of the access
//
// insert_at: the insertion point in the bitcode before which the
// metadata store is introduced.
//
void Nova::AddStoreBaseBoundFunc(Value* pointer_dest, 
                                 Value* pointer_base, 
                                 Value* pointer_bound, 
                                 Value* pointer,
                                 Value* size_of_type, 
                                 Instruction* insert_at) {

  Value* pointer_base_cast = NULL;
  Value* pointer_bound_cast = NULL;

  
  Value* pointer_dest_cast = CastToVoidPtr(pointer_dest, insert_at);

  pointer_base_cast = CastToVoidPtr(pointer_base, insert_at);
  pointer_bound_cast = CastToVoidPtr(pointer_bound, insert_at);

  //  Value* pointer_cast = castToVoidPtr(pointer, insert_at);
    
  SmallVector<Value*, 8> args;

  args.push_back(pointer_dest_cast);

  args.push_back(pointer_base_cast);
  args.push_back(pointer_bound_cast);

  CallInst::Create(m_store_base_bound_func, args, "", insert_at);
}

//
// Method: handleGlobalSequentialTypeInitializer
//
// Description: This performs the initialization of the metadata for
// the pointers in the global segments that are initialized with
// non-zero values.
//
// Comments: This function requires review and rewrite

void Nova::HandleGlobalSequentialTypeInitializer(Module& module, GlobalVariable* gv) {

  //errs() << __func__ <<"\n";
  // Sequential type can be an array type, a pointer type 
  // const SequentialType* init_seq_type = 
  //   dyn_cast<SequentialType>((gv->getInitializer())->getType());
  // assert(init_seq_type && 
  //        "[handleGlobalSequentialTypeInitializer] initializer  null?");
  Type* raw_type=gv->getInitializer()->getType();
  //ArrayType* init_array_type=dyn_cast<ArrayType>(raw_type);
  //PointerType* init_ptr_type=dyn_cast<PointerType>(raw_type);
  Instruction* init_function_terminator = GetGlobalInitInstruction(module);
  if(gv->getInitializer()->isNullValue())
    return;
    
  if(isa<ArrayType>(raw_type)){      
    const ArrayType* init_array_type = dyn_cast<ArrayType>(raw_type);     
    if(isa<StructType>(init_array_type->getElementType())){
      // It is an array of structures

      // Check whether the structure has a pointer, if it has a
      // pointer then, we need to store the base and bound of the
      // pointer into the metadata space. However, if the structure
      // does not have any pointer, we can make a quick exit in
      // processing this global
      //
      
      bool struct_has_pointers = false;
      StructType* init_struct_type = 
        dyn_cast<StructType>(init_array_type->getElementType());

      assert(init_struct_type && 
             "Array of structures and struct type null?");        
      unsigned num_struct_elements = init_struct_type->getNumElements();        
      for(unsigned i = 0; i < num_struct_elements; i++) {
        Type* element_type = init_struct_type->getTypeAtIndex(i);
        if(isa<PointerType>(element_type)){
          struct_has_pointers = true;
        }
      }
      if(!struct_has_pointers)
        return;

      // Here implies, global variable is an array of structures with
      // a pointer. Thus for each pointer we need to store the base
      // and bound

      size_t num_array_elements = init_array_type->getNumElements();
      ConstantArray* const_array = 
        dyn_cast<ConstantArray>(gv->getInitializer());
      if(!const_array)
        return;

      for( unsigned i = 0; i < num_array_elements ; i++) {
        Constant* struct_constant = const_array->getOperand(i);
        assert(struct_constant && 
               "Initializer structure type but not a constant?");          
        // Constant has zero initializer 
        if(struct_constant->isNullValue())
          continue;
          
        for( unsigned j = 0 ; j < num_struct_elements; j++) {
          const Type* element_type = init_struct_type->getTypeAtIndex(j);
            
          if(isa<PointerType>(element_type)){
              
            Value* initializer_opd = struct_constant->getOperand(j);
            Value* operand_base = NULL;
            Value* operand_bound = NULL;
            Constant* given_constant = dyn_cast<Constant>(initializer_opd);
            assert(given_constant && 
                   "[handleGlobalStructTypeInitializer] not a constant?");
              
            GetConstantExprBaseBound(given_constant, operand_base, operand_bound);            
            // Creating the address of ptr
            Constant* index0 = 
              ConstantInt::get(Type::getInt32Ty(module.getContext()), 0);
            Constant* index1 = 
              ConstantInt::get(Type::getInt32Ty(module.getContext()), i);
            Constant* index2 = 
              ConstantInt::get(Type::getInt32Ty(module.getContext()), j);
              
            std::vector<Constant *> indices_addr_ptr;            
                            
            indices_addr_ptr.push_back(index0);
            indices_addr_ptr.push_back(index1);
            indices_addr_ptr.push_back(index2);

            Constant* Indices[3] = {index0, index1, index2};
            Constant* addr_of_ptr = ConstantExpr::getGetElementPtr(nullptr, gv, Indices);
            Type* initializer_type = initializer_opd->getType();
            Value* initializer_size = GetSizeOfType(initializer_type);
            
            AddStoreBaseBoundFunc(addr_of_ptr, operand_base, operand_bound, 
                                  initializer_opd, initializer_size, init_function_terminator);
          }                       
        } // Iterating over struct element ends 
      } // Iterating over array element ends         
    }/// Array of Structures Ends 

    if (isa<PointerType>(init_array_type->getElementType())){
      // It is a array of pointers
    }
  }  // Array type case ends 

  if(isa<PointerType>(raw_type)){
    // individual pointer stores 
    Value* initializer_base = NULL;
    Value* initializer_bound = NULL;
    Value* initializer = gv->getInitializer();
    Constant* given_constant = dyn_cast<Constant>(initializer);
    GetConstantExprBaseBound(given_constant, 
                             initializer_base, 
                             initializer_bound);
    Type* initializer_type = initializer->getType();
    Value* initializer_size = GetSizeOfType(initializer_type);
    
    AddStoreBaseBoundFunc(gv, initializer_base, initializer_bound,
                          initializer, initializer_size, 
                          init_function_terminator);        
  }

}

void Nova::AddBaseBoundGlobalValue(Module &M, Value *v){

    GlobalVariable* gv = dyn_cast<GlobalVariable>(v);
    
    if(!gv){
      return;
    }

    if(StringRef(gv->getSection()) == "llvm.metadata"){
      return;
    }
    if(gv->getName() == "llvm.global_ctors"){
      return;
    }
    
    if(!gv->hasInitializer())
      return;
    
    /* gv->hasInitializer() is true */
    
    Constant* initializer = dyn_cast<Constant>(gv->getInitializer());
    ConstantArray* constant_array = dyn_cast<ConstantArray>(initializer);
    Type* initTy = initializer->getType();
    if(initializer &&isa<StructType>(initTy) || isa<ArrayType>(initTy)){

      if(isa<StructType>(initializer->getType())){
        std::vector<Constant*> indices_addr_ptr;
        Constant* index1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0);
        indices_addr_ptr.push_back(index1);
        StructType* struct_type = dyn_cast<StructType>(initializer->getType());
        HandleGlobalStructTypeInitializer(M, struct_type, initializer, gv, indices_addr_ptr, 1);
        return;
      }
      if(isa<PointerType>(initializer->getType())||isa<ArrayType>(initializer->getType())){
        HandleGlobalSequentialTypeInitializer(M, gv);
      }
      
    }
    
    if(initializer && !constant_array){
        errs() <<"gv doesn't have constant_array initializer\n";
        initializer->getType()->dump();
      
      if(isa<PointerType>(initializer->getType())){
        errs() <<"gv has Pointer type initializer\n";
      } 
    }
    
    if(!constant_array)
      return;
    
    int num_ca_opds = constant_array->getNumOperands();
    errs() <<"gv has constant_array initializer, num_ca_opds: "<< num_ca_opds << "\n";
    
    for(int i = 0; i < num_ca_opds; i++){
      Value* initializer_opd = constant_array->getOperand(i);
      Instruction* first = GetGlobalInitInstruction(M);
      Value* operand_base = NULL;
      Value* operand_bound = NULL;
      
      Constant* global_constant_initializer = dyn_cast<Constant>(initializer_opd);
      if(!isa<PointerType>(global_constant_initializer->getType())){
        break;
      }
      GetConstantExprBaseBound(global_constant_initializer, operand_base, operand_bound);
      
      SmallVector<Value*, 8> args;
      Constant* index1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0);
      Constant* index2 = ConstantInt::get(Type::getInt32Ty(M.getContext()), i);

      std::vector<Constant*> indices_addr_ptr;
      indices_addr_ptr.push_back(index1);
      indices_addr_ptr.push_back(index2);

      Constant* addr_of_ptr = ConstantExpr::getGetElementPtr(nullptr, gv, indices_addr_ptr);
      Type* initializer_type = initializer_opd->getType();
      Value* initializer_size = GetSizeOfType(initializer_type);
      
      AddStoreBaseBoundFunc(addr_of_ptr, operand_base, operand_bound, initializer_opd, initializer_size, first);
    }
}

void Nova::CollectArrayBoundaryInfo(Module &M, Value *v) {
    AllocaInst *ai;
    Instruction *inst;


    if (isa<GlobalVariable>(v)) { // handle global variable here
        errs() << __func__ << " global v is :" << *v << "\n";
        AddBaseBoundGlobalValue(M, v);
    } else { // handle local variable here

        errs() << __func__ << " local v is :" << *v << "\n";

        ai = dyn_cast<AllocaInst>(v);
        assert(ai && " local array is not defined as alloca inst!");
        
        // get next inst after ai
        BasicBlock::iterator nextInst = GetInstIterator(v);
        nextInst++;
        Instruction *next = dyn_cast<Instruction>(nextInst);
        assert (next && "Cannot increment the instruction iterator?");

        // get basicblock that ai belongs to
        inst = dyn_cast<Instruction>(v);
        assert (inst && "v should be an instruction!");

        unsigned numOperands = ai->getNumOperands();

        /* For any alloca instruction, base is bitcast of alloca,
           bound is bitcast of alloca_ptr + 1. Refer to SoftboundsCETS */
        PointerType* ptrType = PointerType::get(ai->getAllocatedType(), 0);
        Type* ty1 = ptrType;
        BitCastInst* ptr = new BitCastInst(ai, ty1, ai->getName(), next);

        Value* ptrBase = CastToVoidPtr(v, next);
        Value* intBound;

        if(numOperands == 0) {
            // TODO: We only support 32-bit architechture
            intBound = ConstantInt::get(Type::getInt32Ty(ai->getType()->getContext()), 1, false);
        } else {
            // What can be operand of alloca instruction?
            intBound = ai->getOperand(0);
        }

        GetElementPtrInst* gep = GetElementPtrInst::Create(nullptr,
                                                           ptr,
                                                           intBound,
                                                           "mtmp",
                                                           next);
        Value *boundPtr = gep;
        Value* ptrBound = CastToVoidPtr(boundPtr, next);

        // TODOO instrument or leave it there?
        AssociateBaseBound(v, ptrBase, ptrBound);
    }

    return;
}

// Method: isStructOperand
// **Borrowed from SoftboundCETS
//
//
//Description: This function elides the checks for the structure
//accesses. This is safe when there are no casts in the program.
//
bool Nova::IsStructOperand(Value* pointer_operand) {
  
  if(isa<GetElementPtrInst>(pointer_operand)){
    GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(pointer_operand);
    Value* gep_operand = gep->getOperand(0);
    const PointerType* ptr_type = dyn_cast<PointerType>(gep_operand->getType());
    if(isa<StructType>(ptr_type->getNonOpaquePointerElementType())){
      return true;
    }
  }
  return false;
}

// for array type, we get boundary info immediately
void Nova::ArrayAccessCheck(Module &M, Value *v) {
    Value *pointerOperand = NULL;
    Instruction *inst = NULL;
    Value* tmpBase = NULL;
    Value* tmpBound = NULL;
    Value* bitcastBase = NULL;
    Value* bitcastBound = NULL;
    Value* castPointerValue = NULL;
    ValueSet kinSet; // propagated variable set with the same metadata as v.
    SmallVector<Value*, 8> args;
    // if array is global variable, we should initialize it before program exeucte
    // if array is local variable, we should insert code after the last alloca instruction
    // in the first bb.

    // collect array boundary info
    CollectArrayBoundaryInfo(M, v);

    // propagate metadata to a set of related variables
    kinSet.insert(v);
    for (User *UoV : v->users()) {
        if ((inst = (dyn_cast<Instruction>(UoV)))) {
            if (isa<GetElementPtrInst>(inst)) {
                GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(inst);
                assert(gepi && "Not a GEP instruction");
                pointerOperand = gepi->getPointerOperand();
                assert(v == pointerOperand && "not propagate from v to GEP's Pointer Operand ?");
                tmpBase = GetAssociatedBase(pointerOperand);
                tmpBound = GetAssociatedBound(pointerOperand);
                AssociateBaseBound(inst, tmpBase, tmpBound);   
                kinSet.insert(inst);
                errs() <<"add to kinSet: " <<*inst << "\n";
            } else if (isa<BitCastInst>(inst)) {
                BitCastInst *bci = dyn_cast<BitCastInst>(inst);
                assert(bci && "Not a BitCast instruction");
                pointerOperand = bci->getOperand(0);
                assert(v == pointerOperand && "not propagate from v to BitCast's Pointer Operand ?");
                tmpBase = GetAssociatedBase(pointerOperand);
                tmpBound = GetAssociatedBound(pointerOperand);
                AssociateBaseBound(inst, tmpBase, tmpBound); 
                kinSet.insert(inst);
                errs() <<"add to kinSet: " <<*inst << "\n";
            } else {
                errs() <<"not add to kinSet: " <<*inst << "\n";
            }
        } else {
            errs() <<"Attention: v("<< *v << ") is not used in any instruction:\n";
        }
    }

    // check array-based read&write
    // TODOO
    for (ValueSet::iterator it = kinSet.begin(), ie = kinSet.end(); it != ie; ++it) {
        for (User *UoV : (*it)->users()) {
            if ((inst = (dyn_cast<Instruction>(UoV)))) {
                errs() << "v is used in instruction:\n";
                errs() << *inst << "\n";

                if (isa<LoadInst>(inst)) {
                    LoadInst *ldi = dyn_cast<LoadInst>(inst);
                    assert(ldi && "Not a load instruction");
                    pointerOperand = ldi->getPointerOperand();
                } else if (isa<StoreInst>(inst)) {
                    StoreInst *sti = dyn_cast<StoreInst>(inst);
                    assert(sti && "Not a store instruction");
                    pointerOperand = sti->getPointerOperand();
                } else {
                    errs() << "not load nor store inst, skip\n";
                    continue;
                }

                assert(pointerOperand && "pointer operand null?");
                if (pointerOperand != (*it)) {
                    // pointerOperand isn't the sensitive var, need to handle its boundary collection
                    CollectArrayBoundaryInfo(M, pointerOperand);
                }

                // should we ignore stuct pointer check?
                // TODO
                if(IsStructOperand(pointerOperand))
                    continue;

                // If it is a null pointer which is being loaded, then it must seg
                // fault, no dereference check here. Refer to SoftboundCETS
                if(isa<ConstantPointerNull>(pointerOperand))
                    continue;

                tmpBase = GetAssociatedBase(pointerOperand);
                tmpBound = GetAssociatedBound(pointerOperand);

                // empty args
                args.clear();

                bitcastBase = CastToVoidPtr(tmpBase, inst);
                args.push_back(bitcastBase);
                
                bitcastBound = CastToVoidPtr(tmpBound, inst);    
                args.push_back(bitcastBound);
                 
                castPointerValue = CastToVoidPtr(pointerOperand, inst);    
                args.push_back(castPointerValue);

                // Pushing the size of the type 
                Type* pointerOperandType = pointerOperand->getType();
                Value* sizeOfType = GetSizeOfType(pointerOperandType);
                args.push_back(sizeOfType);

                // add check inst
                // TODOO create function symbol for it.
                errs() << "insert load/store dereference check \n";
                CallInst::Create(m_spatial_load_dereference_check, args, "", inst);
            }
        }
    }
}

// step : instrument its definition to collect boundary info
//void Nova::CollectArrayBoundaryInfo(Value *v) {
//    // TODOO
//    IRBuilder<> B(inst);
//    Module *M = B.GetInsertBlock()->getModule();
//    Type *VoidTy = B.getVoidTy();
//    Type *I64Ty = B.getInt64Ty();
//    Value *castAddr, *castVal;
//
//    //errs() << __func__ << " : "<< *inst << "\n";
//
//    Constant *RecordDefEvt = M->getOrInsertFunction("__record_defevt", VoidTy,
//                                                                      I64Ty,
//                                                                      I64Ty, 
//                                                                      nullptr);
//
//    Function *RecordDefEvtFunc = cast<Function>(RecordDefEvt);
//    castAddr = CastInst::Create(Instruction::PtrToInt, addr, I64Ty, "recptrtoint", inst);
//
//    if (val->getType()->isPointerTy()) {
//        castVal = CastInst::Create(Instruction::PtrToInt, val, I64Ty, "recptrtoint", inst);
//    } else if (val->getType()->isIntegerTy(64)) {
//        castVal = val;
//    } else if (val->getType()->isIntegerTy()) {
//        castVal = CastInst::Create(Instruction::ZExt, val, I64Ty, "reczexttoi64", inst);
//    } else
//	return;
//
//    B.CreateCall(RecordDefEvtFunc, {castAddr, castVal});
//
//    return;
//}

//
// Method: isFuncDefSoftBound
//
// Description: 
//
// This function checks if the input function name is a
// SoftBound/CETS defined function
//

bool Nova::IsFuncDefSoftBound(const std::string &str) {
  if (m_func_def_softbound.getNumItems() == 0) {

    m_func_wrappers_available["system"] = true;
    m_func_wrappers_available["setreuid"] = true;
    m_func_wrappers_available["mkstemp"] = true;
    m_func_wrappers_available["getuid"] = true;
    m_func_wrappers_available["getrlimit"] = true;
    m_func_wrappers_available["setrlimit"] = true;
    m_func_wrappers_available["fread"] = true;
    m_func_wrappers_available["umask"] = true;
    m_func_wrappers_available["mkdir"] = true;
    m_func_wrappers_available["chroot"] = true;
    m_func_wrappers_available["rmdir"] = true;
    m_func_wrappers_available["stat"] = true;
    m_func_wrappers_available["fputc"] = true;
    m_func_wrappers_available["fileno"] = true;
    m_func_wrappers_available["fgetc"] = true;
    m_func_wrappers_available["strncmp"] = true;
    m_func_wrappers_available["log"] = true;
    m_func_wrappers_available["fwrite"] = true;
    m_func_wrappers_available["atof"] = true;
    m_func_wrappers_available["feof"] = true;
    m_func_wrappers_available["remove"] = true;
    m_func_wrappers_available["acos"] = true;
    m_func_wrappers_available["atan2"] = true;
    m_func_wrappers_available["sqrtf"] = true;
    m_func_wrappers_available["expf"] = true;
    m_func_wrappers_available["exp2"] = true;
    m_func_wrappers_available["floorf"] = true;
    m_func_wrappers_available["ceil"] = true;
    m_func_wrappers_available["ceilf"] = true;
    m_func_wrappers_available["floor"] = true;
    m_func_wrappers_available["sqrt"] = true;
    m_func_wrappers_available["fabs"] = true;
    m_func_wrappers_available["abs"] = true;
    m_func_wrappers_available["srand"] = true;
    m_func_wrappers_available["srand48"] = true;
    m_func_wrappers_available["pow"] = true;
    m_func_wrappers_available["fabsf"] = true;
    m_func_wrappers_available["tan"] = true;
    m_func_wrappers_available["tanf"] = true;
    m_func_wrappers_available["tanl"] = true;
    m_func_wrappers_available["log10"] = true;
    m_func_wrappers_available["sin"] = true;
    m_func_wrappers_available["sinf"] = true;
    m_func_wrappers_available["sinl"] = true;
    m_func_wrappers_available["cos"] = true;
    m_func_wrappers_available["cosf"] = true;
    m_func_wrappers_available["cosl"] = true;
    m_func_wrappers_available["exp"] = true;
    m_func_wrappers_available["ldexp"] = true;
    m_func_wrappers_available["tmpfile"] = true;
    m_func_wrappers_available["ferror"] = true;
    m_func_wrappers_available["ftell"] = true;
    m_func_wrappers_available["fstat"] = true;
    m_func_wrappers_available["fflush"] = true;
    m_func_wrappers_available["fputs"] = true;
    m_func_wrappers_available["fopen"] = true;
    m_func_wrappers_available["fdopen"] = true;
    m_func_wrappers_available["fseek"] = true;
    m_func_wrappers_available["ftruncate"] = true;
    m_func_wrappers_available["popen"] = true;
    m_func_wrappers_available["fclose"] = true;
    m_func_wrappers_available["pclose"] = true;
    m_func_wrappers_available["rewind"] = true;
    m_func_wrappers_available["readdir"] = true;
    m_func_wrappers_available["opendir"] = true;
    m_func_wrappers_available["closedir"] = true;
    m_func_wrappers_available["rename"] = true;
    m_func_wrappers_available["sleep"] = true;
    m_func_wrappers_available["getcwd"] = true;
    m_func_wrappers_available["chown"] = true;
    m_func_wrappers_available["isatty"] = true;
    m_func_wrappers_available["chdir"] = true;
    m_func_wrappers_available["strcmp"] = true;
    m_func_wrappers_available["strcasecmp"] = true;
    m_func_wrappers_available["strncasecmp"] = true;
    m_func_wrappers_available["strlen"] = true;
    m_func_wrappers_available["strpbrk"] = true;
    m_func_wrappers_available["gets"] = true;
    m_func_wrappers_available["fgets"] = true;
    m_func_wrappers_available["perror"] = true;
    m_func_wrappers_available["strspn"] = true;
    m_func_wrappers_available["strcspn"] = true;
    m_func_wrappers_available["memcmp"] = true;
    m_func_wrappers_available["memchr"] = true;
    m_func_wrappers_available["rindex"] = true;
    m_func_wrappers_available["strtoul"] = true;
    m_func_wrappers_available["strtod"] = true;
    m_func_wrappers_available["strtol"] = true;
    m_func_wrappers_available["strchr"] = true;
    m_func_wrappers_available["strrchr"] = true;
    m_func_wrappers_available["strcpy"] = true;
    m_func_wrappers_available["abort"] = true;
    m_func_wrappers_available["rand"] = true;
    m_func_wrappers_available["atoi"] = true;
    //m_func_wrappers_available["puts"] = true;
    m_func_wrappers_available["exit"] = true;
    m_func_wrappers_available["strtok"] = true;
    m_func_wrappers_available["strdup"] = true;
    m_func_wrappers_available["strcat"] = true;
    m_func_wrappers_available["strncat"] = true;
    m_func_wrappers_available["strncpy"] = true;
    m_func_wrappers_available["strstr"] = true;
    m_func_wrappers_available["signal"] = true;
    m_func_wrappers_available["clock"] = true;
    m_func_wrappers_available["atol"] = true;
    m_func_wrappers_available["realloc"] = true;
    m_func_wrappers_available["calloc"] = true;
    m_func_wrappers_available["malloc"] = true;
    m_func_wrappers_available["mmap"] = true;

    m_func_wrappers_available["putchar"] = true;
    m_func_wrappers_available["times"] = true;
    m_func_wrappers_available["strftime"] = true;
    m_func_wrappers_available["localtime"] = true;
    m_func_wrappers_available["time"] = true;
    m_func_wrappers_available["drand48"] = true;
    m_func_wrappers_available["free"] = true;
    m_func_wrappers_available["lrand48"] = true;
    m_func_wrappers_available["ctime"] = true;
    m_func_wrappers_available["difftime"] = true;
    m_func_wrappers_available["toupper"] = true;
    m_func_wrappers_available["tolower"] = true;
    m_func_wrappers_available["setbuf"] = true;
    m_func_wrappers_available["getenv"] = true;
    m_func_wrappers_available["atexit"] = true;
    m_func_wrappers_available["strerror"] = true;
    m_func_wrappers_available["unlink"] = true;
    m_func_wrappers_available["close"] = true;
    m_func_wrappers_available["open"] = true;
    m_func_wrappers_available["read"] = true;
    m_func_wrappers_available["write"] = true;
    m_func_wrappers_available["lseek"] = true;
    m_func_wrappers_available["gettimeofday"] = true;
    m_func_wrappers_available["select"] = true;
    m_func_wrappers_available["__errno_location"] = true;
    m_func_wrappers_available["__ctype_b_loc"] = true;
    m_func_wrappers_available["__ctype_toupper_loc"] = true;
    m_func_wrappers_available["__ctype_tolower_loc"] = true;
    m_func_wrappers_available["qsort"] = true;

    m_func_def_softbound["puts"] = true;
    m_func_def_softbound["__softboundcets_intermediate"]= true;
    m_func_def_softbound["__softboundcets_dummy"] = true;
    m_func_def_softbound["__softboundcets_print_metadata"] = true;
    m_func_def_softbound["__softboundcets_introspect_metadata"] = true;
    m_func_def_softbound["__softboundcets_copy_metadata"] = true;
    m_func_def_softbound["__softboundcets_allocate_shadow_stack_space"] = true;
    m_func_def_softbound["__softboundcets_load_base_shadow_stack"] = true;
    m_func_def_softbound["__softboundcets_load_bound_shadow_stack"] = true;
    m_func_def_softbound["__softboundcets_load_key_shadow_stack"] = true;
    m_func_def_softbound["__softboundcets_load_lock_shadow_stack"] = true;
    m_func_def_softbound["__softboundcets_store_base_shadow_stack"] = true;      
    m_func_def_softbound["__softboundcets_store_bound_shadow_stack"] = true;      
    m_func_def_softbound["__softboundcets_store_key_shadow_stack"] = true;      
    m_func_def_softbound["__softboundcets_store_lock_shadow_stack"] = true;      
    m_func_def_softbound["__softboundcets_deallocate_shadow_stack_space"] = true;

    m_func_def_softbound["__softboundcets_trie_allocate"] = true;
    m_func_def_softbound["__shrinkBounds"] = true;
    m_func_def_softbound["__softboundcets_memcopy_check"] = true;

    m_func_def_softbound["__softboundcets_spatial_load_dereference_check"] = true;

    m_func_def_softbound["__softboundcets_spatial_store_dereference_check"] = true;
    m_func_def_softbound["__softboundcets_spatial_call_dereference_check"] = true;
    m_func_def_softbound["__softboundcets_temporal_load_dereference_check"] = true;
    m_func_def_softbound["__softboundcets_temporal_store_dereference_check"] = true;
    m_func_def_softbound["__softboundcets_stack_memory_allocation"] = true;
    m_func_def_softbound["__softboundcets_memory_allocation"] = true;
    m_func_def_softbound["__softboundcets_get_global_lock"] = true;
    m_func_def_softbound["__softboundcets_add_to_free_map"] = true;
    m_func_def_softbound["__softboundcets_check_remove_from_free_map"] = true;
    m_func_def_softbound["__softboundcets_allocation_secondary_trie_allocate"] = true;
    m_func_def_softbound["__softboundcets_allocation_secondary_trie_allocate_range"] = true;
    m_func_def_softbound["__softboundcets_allocate_lock_location"] = true;
    m_func_def_softbound["__softboundcets_memory_deallocation"] = true;
    m_func_def_softbound["__softboundcets_stack_memory_deallocation"] = true;

    m_func_def_softbound["__softboundcets_metadata_load_vector"] = true;
    m_func_def_softbound["__softboundcets_metadata_store_vector"] = true;
    
    m_func_def_softbound["__softboundcets_metadata_load"] = true;
    m_func_def_softbound["__softboundcets_metadata_store"] = true;
    m_func_def_softbound["__hashProbeAddrOfPtr"] = true;
    m_func_def_softbound["__memcopyCheck"] = true;
    m_func_def_softbound["__memcopyCheck_i64"] = true;

    m_func_def_softbound["__softboundcets_global_init"] = true;      
    m_func_def_softbound["__softboundcets_init"] = true;      
    m_func_def_softbound["__softboundcets_abort"] = true;      
    m_func_def_softbound["__softboundcets_printf"] = true;
    
    m_func_def_softbound["__softboundcets_stub"] = true;
    m_func_def_softbound["safe_mmap"] = true;
    m_func_def_softbound["safe_calloc"] = true;
    m_func_def_softbound["safe_malloc"] = true;
    m_func_def_softbound["safe_free"] = true;

    m_func_def_softbound["__assert_fail"] = true;
    m_func_def_softbound["assert"] = true;
    m_func_def_softbound["__strspn_c2"] = true;
    m_func_def_softbound["__strcspn_c2"] = true;
    m_func_def_softbound["__strtol_internal"] = true;
    m_func_def_softbound["__stroul_internal"] = true;
    m_func_def_softbound["ioctl"] = true;
    m_func_def_softbound["error"] = true;
    m_func_def_softbound["__strtod_internal"] = true;
    m_func_def_softbound["__strtoul_internal"] = true;
    
    
    m_func_def_softbound["fflush_unlocked"] = true;
    m_func_def_softbound["full_write"] = true;
    m_func_def_softbound["safe_read"] = true;
    m_func_def_softbound["_IO_getc"] = true;
    m_func_def_softbound["_IO_putc"] = true;
    m_func_def_softbound["__xstat"] = true;

    m_func_def_softbound["select"] = true;
    m_func_def_softbound["_setjmp"] = true;
    m_func_def_softbound["longjmp"] = true;
    m_func_def_softbound["fork"] = true;
    m_func_def_softbound["pipe"] = true;
    m_func_def_softbound["dup2"] = true;
    m_func_def_softbound["execv"] = true;
    m_func_def_softbound["compare_pic_by_pic_num_desc"] = true;
     
    m_func_def_softbound["wprintf"] = true;
    m_func_def_softbound["vfprintf"] = true;
    m_func_def_softbound["vsprintf"] = true;
    m_func_def_softbound["fprintf"] = true;
    m_func_def_softbound["printf"] = true;
    m_func_def_softbound["sprintf"] = true;
    m_func_def_softbound["snprintf"] = true;

    m_func_def_softbound["scanf"] = true;
    m_func_def_softbound["fscanf"] = true;
    m_func_def_softbound["sscanf"] = true;   

    m_func_def_softbound["asprintf"] = true;
    m_func_def_softbound["vasprintf"] = true;
    m_func_def_softbound["__fpending"] = true;
    m_func_def_softbound["fcntl"] = true;

    m_func_def_softbound["vsnprintf"] = true;
    m_func_def_softbound["fwrite_unlocked"] = true;
    m_func_def_softbound["__overflow"] = true;
    m_func_def_softbound["__uflow"] = true;
    m_func_def_softbound["execlp"] = true;
    m_func_def_softbound["execl"] = true;
    m_func_def_softbound["waitpid"] = true;
    m_func_def_softbound["dup"] = true;
    m_func_def_softbound["setuid"] = true;
    
    m_func_def_softbound["_exit"] = true;
    m_func_def_softbound["funlockfile"] = true;
    m_func_def_softbound["flockfile"] = true;

    m_func_def_softbound["__option_is_short"] = true;
    

  }

  // Is the function name in the above list?
  if (m_func_def_softbound.count(str) > 0) {
    return true;
  }

  // FIXME: handling new intrinsics which have isoc99 in their name
  if (str.find("isoc99") != std::string::npos){
    return true;
  }

  // If the function is an llvm intrinsic, don't transform it
  if (str.find("llvm.") == 0) {
    return true;
  }

  return false;
}

//
// Method: hasPtrArgRetType()
//
// Description:
//
// This function checks if the function has either pointer arguments
// or returns a pointer value. This function is used to determine
// whether shadow stack loads/stores need to be introduced for
// metadata propagation.
//

bool Nova::HasPtrArgRetType(Function* func) {
   
  const Type* ret_type = func->getReturnType();
  if (isa<PointerType>(ret_type))
    return true;

  for (Function::arg_iterator i = func->arg_begin(), e = func->arg_end(); 
      i != e; ++i) {
      
    if (isa<PointerType>(i->getType()))
      return true;
  }
  return false;
}

// 
// Method: identifyFuncToTrans
//
// Description: This function traverses the module and identifies the
// functions that need to be transformed by SoftBound/CETS
//

void Nova::IdentifyFuncToTrans(Module& module) {
    
  for (Module::iterator fb_it = module.begin(), fe_it = module.end(); 
      fb_it != fe_it; ++fb_it) {

    Function* func = dyn_cast<Function>(fb_it);
    assert(func && " Not a function");

    // Check if the function is defined in the module
    if (!func->isDeclaration()) {
      if (IsFuncDefSoftBound(func->getName().str())) 
        continue;
      
      m_func_softboundcets_transform[func->getName()] = true;
      if (HasPtrArgRetType(func)) {
        m_func_to_transform[func->getName()] = true;
      }
    }
  }
}

/* Identify the initial globals present in the program before we add
 * extra base and bound for all globals
 */
void Nova::IdentifyInitialGlobals(Module& module) {

  for(Module::global_iterator it = module.global_begin(), 
        ite = module.global_end();
      it != ite; ++it) {
      
    GlobalVariable* gv = dyn_cast<GlobalVariable>(it);
    if(gv) {
      m_initial_globals[gv] = true;
    }      
  }
}

// Currently just a placeholder for functions introduced by us
bool Nova::CheckIfFunctionOfInterest(Function* func) {

  if(IsFuncDefSoftBound(func->getName().str()))
    return false;

  if(func->isDeclaration())
    return false;


  /* TODO: URGENT: Need to do base and bound propagation in variable
   * argument functions
   */
#if 0
  if(func.isVarArg())
    return false;
#endif

  return true;
}

void Nova::IdentifyOriginalInst (Function * func) {

  for(Function::iterator bb_begin = func->begin(), bb_end = func->end();
      bb_begin != bb_end; ++bb_begin) {

    for(BasicBlock::iterator i_begin = bb_begin->begin(),
          i_end = bb_begin->end(); i_begin != i_end; ++i_begin){

      Value* insn = dyn_cast<Value>(i_begin);
      if(!m_present_in_original.count(insn)) {
        m_present_in_original[insn] = 1;
      }
      else {
        assert(0 && "present in original map already has the insn?");
      }

      if(isa<PointerType>(insn->getType())) {
        if(!m_is_pointer.count(insn)){
          m_is_pointer[insn] = 1;
        }
      }
    } /* BasicBlock ends */
  }/* Function ends */
}

bool Nova::CheckPtrsInST(StructType* struct_type){
  
  StructType::element_iterator I = struct_type->element_begin();

  bool ptr_flag = false;
  for(StructType::element_iterator E = struct_type->element_end(); I != E; ++I){
    
    Type* element_type = *I;

    if(isa<StructType>(element_type)){
      StructType* struct_element_type = dyn_cast<StructType>(element_type);
      bool recursive_flag = CheckPtrsInST(struct_element_type);
      ptr_flag = ptr_flag | recursive_flag;
    }
    if(isa<PointerType>(element_type)){
      ptr_flag = true;
    }
    if(isa<ArrayType>(element_type)){
      ptr_flag = true;      
    }
  }
  return ptr_flag;
}

bool Nova::CheckTypeHasPtrs(Argument* ptr_argument){

  if(!ptr_argument->hasByValAttr())
    return false;

  // SequentialType* seq_type = dyn_cast<SequentialType>(ptr_argument->getType());
  // assert(seq_type && "byval attribute with non-sequential type pointer, not handled?");
  Type* argType=ptr_argument->getType();
  StructType* struct_type;
  if(PointerType * ptr_Type=dyn_cast<PointerType>(argType)){
    struct_type = dyn_cast<StructType>(ptr_Type->getNonOpaquePointerElementType());
  }

  if(struct_type){
    bool has_ptrs = CheckPtrsInST(struct_type);
    return has_ptrs;
  }
  else{
    assert(0 && "non-struct byval parameters?");
  }

  // By default we assume any struct can return pointers 
  return true;                                              

}

void Nova::HandleAlloca (AllocaInst* alloca_inst,
                         BasicBlock* bb, 
                         BasicBlock::iterator& i) {

  Value *alloca_inst_value = alloca_inst;

  /* Get the base type of the alloca object For alloca instructions,
   * instructions need to inserted after the alloca instruction LLVM
   * provides interface for inserting before.  So use the iterators
   * and handle the case
   */
  
  BasicBlock::iterator nextInst = i;
  nextInst++;
  Instruction* next = dyn_cast<Instruction>(nextInst);
  assert(next && "Cannot increment the instruction iterator?");
  
  unsigned num_operands = alloca_inst->getNumOperands();
  
  /* For any alloca instruction, base is bitcast of alloca, bound is bitcast of alloca_ptr + 1
   */
  PointerType* ptr_type = PointerType::get(alloca_inst->getAllocatedType(), 0);
  Type* ty1 = ptr_type;
  //    Value* alloca_inst_temp_value = alloca_inst;
  BitCastInst* ptr = new BitCastInst(alloca_inst, ty1, alloca_inst->getName(), next);
  
  Value* ptr_base = CastToVoidPtr(alloca_inst_value, next);
  
  Value* intBound;
  
  if(num_operands == 0) {
      intBound = ConstantInt::get(Type::getInt32Ty(alloca_inst->getType()->getContext()), 1, false);
  } else {
    // What can be operand of alloca instruction?
    intBound = alloca_inst->getOperand(0);
  }

  GetElementPtrInst* gep = GetElementPtrInst::Create(nullptr,
  					                                 ptr,
                                                     intBound,
                                                     "mtmp",
                                                     next);
  Value *bound_ptr = gep;
  
  Value* ptr_bound = CastToVoidPtr(bound_ptr, next);
  
  AssociateBaseBound(alloca_inst_value, ptr_base, ptr_bound);
}

// 
// Method: getNextInstruction
// 
// Description:
// This method returns the next instruction after the input instruction.
//

Instruction* Nova::GetNextInstruction(Instruction* I){
  
  if (I->isTerminator()) {
    return I;
  } else {
    // 使用更安全的迭代方式
    BasicBlock::iterator BBI(I);
    if (++BBI == I->getParent()->end()) {
      return nullptr;  // 或者根据需求处理边界情况
    }
    return &*BBI;
  }    
}

void Nova::InsertMetadataLoad(LoadInst* load_inst){

  AllocaInst* base_alloca;
  AllocaInst* bound_alloca;

  SmallVector<Value*, 8> args;

  Value* load_inst_value = load_inst;
  Value* pointer_operand = load_inst->getPointerOperand();
  Instruction* load = load_inst;    
  Instruction* insert_at = GetNextInstruction(load);

  /* If the load returns a pointer, then load the base and bound
   * from the shadow space
   */
  Value* pointer_operand_bitcast =  CastToVoidPtr(pointer_operand, insert_at);      
  Instruction* first_inst_func = dyn_cast<Instruction>(load_inst->getParent()->getParent()->begin()->begin());
  assert(first_inst_func && "function doesn't have any instruction and there is load???");
  
  /* address of pointer being pushed */
  args.push_back(pointer_operand_bitcast);

  base_alloca = new AllocaInst(m_void_ptr_type, 0, "base.alloca", first_inst_func);
  bound_alloca = new AllocaInst(m_void_ptr_type, 0, "bound.alloca", first_inst_func);

  /* base */
  args.push_back(base_alloca);
  /* bound */
  args.push_back(bound_alloca);

  CallInst::Create(m_load_base_bound_func, args, "", insert_at);

  Instruction* base_load = new LoadInst(m_void_ptr_type, base_alloca, "base.load", insert_at);
  Instruction* bound_load = new LoadInst(m_void_ptr_type, bound_alloca, "bound.load", insert_at);
  AssociateBaseBound(load_inst_value, base_load, bound_load);
}

/* handleLoad Takes a load_inst If the load is through a pointer
 * which is a global then inserts base and bound for that global
 * Also if the loaded value is a pointer then loads the base and
 * bound for for the pointer from the shadow space
 */

void Nova::HandleLoad(LoadInst* load_inst) { 


  if(!isa<VectorType>(load_inst->getType()) && !isa<PointerType>(load_inst->getType())){
    return;
  }
  
  if(isa<PointerType>(load_inst->getType())){
    InsertMetadataLoad(load_inst);
    return;
  }
 
  if(isa<VectorType>(load_inst->getType())){
    
#if 0
    if(!spatial_safety || !temporal_safety){
      assert(0 && "Loading and Storing Pointers as a first-class types");            
      return;
    }

#endif
    
    // It should be a vector if here
    const VectorType* vector_ty = dyn_cast<VectorType>(load_inst->getType());
    // Introduce a series of metadata loads and associated it pointers
    if(!isa<PointerType>(vector_ty->getElementType()))
       return;
 
#if 0   
    Value* load_inst_value = load_inst;
    Instruction* load = load_inst;    
#endif

    Value* pointer_operand = load_inst->getPointerOperand();
    Instruction* insert_at = GetNextInstruction(load_inst);
        
    Value* pointer_operand_bitcast =  CastToVoidPtr(pointer_operand, insert_at);      
    Instruction* first_inst_func = dyn_cast<Instruction>(load_inst->getParent()->getParent()->begin()->begin());
    assert(first_inst_func && "function doesn't have any instruction and there is load???");
   
    uint64_t num_elements = vector_ty->getElementCount().getKnownMinValue();

    
    SmallVector<Value*, 8> vector_base;
    SmallVector<Value*, 8> vector_bound;

    for(uint64_t i = 0; i < num_elements; i++){
      AllocaInst* base_alloca;
      AllocaInst* bound_alloca;
      
      SmallVector<Value*, 8> args;
      
      args.push_back(pointer_operand_bitcast);
      
      base_alloca = new AllocaInst(m_void_ptr_type, 0,"base.alloca", first_inst_func);
      bound_alloca = new AllocaInst(m_void_ptr_type, 0,"bound.alloca", first_inst_func);
	 
      /* base */
      args.push_back(base_alloca);
      /* bound */
      args.push_back(bound_alloca);

      Constant* index = ConstantInt::get(Type::getInt32Ty(load_inst->getContext()), i);

      args.push_back(index);
          
      CallInst::Create(m_metadata_load_vector_func, args, "", insert_at);
      
      Instruction* base_load = new LoadInst(m_void_ptr_type,base_alloca, "base.load", insert_at);
      Instruction* bound_load = new LoadInst(m_void_ptr_type,bound_alloca, "bound.load", insert_at);
      
      vector_base.push_back(base_load);
      vector_bound.push_back(bound_load);
    }
    
    if (num_elements > 2){
      assert(0 && "Loading and Storing Pointers as a first-class types with more than 2 elements");      
    }
    
    // 假设 num_elements 是 uint64_t 类型
    ElementCount EC = ElementCount::getFixed(num_elements);
    VectorType* metadata_ptr_type = VectorType::get(m_void_ptr_type, EC);
    
    Value *CV0 = ConstantInt::get(Type::getInt32Ty(load_inst->getContext()), 0);
    Value *CV1 = ConstantInt::get(Type::getInt32Ty(load_inst->getContext()), 1);

    Value* base_vector = InsertElementInst::Create(UndefValue::get(metadata_ptr_type),     vector_base[0],  CV0, "", insert_at);
    Value* base_vector_final = InsertElementInst::Create(base_vector, vector_base[1], CV1, "", insert_at);
  
    m_vector_pointer_base[load_inst] = base_vector_final;

    Value* bound_vector = InsertElementInst::Create(UndefValue::get(metadata_ptr_type),     vector_bound[0],  CV0, "", insert_at);
    Value* bound_vector_final = InsertElementInst::Create(bound_vector, vector_bound[1], CV1, "", insert_at); 
    m_vector_pointer_bound[load_inst] = bound_vector_final;

    return;
  }

#if 0
  if(unsafe_byval_opt && isByValDerived(load_inst->getOperand(0))) {

    if(spatial_safety){
      associateBaseBound(load_inst, m_void_null_ptr, m_infinite_bound_ptr);
    }
    if(temporal_safety){
      Value* func_lock = getAssociatedFuncLock(load_inst);
      associateKeyLock(load_inst, m_constantint64ty_one, func_lock);
    }
    return;
  }
#endif

}

//
// Method: checkBaseBoundMetadataPresent()
//
// Description:
// Checks if the metadata is present in the SoftBound/CETS maps.

bool Nova::CheckBaseBoundMetadataPresent(Value* pointer_operand){

  if(m_pointer_base.count(pointer_operand) && 
     m_pointer_bound.count(pointer_operand)){
      return true;
  }
  return false;
}

//
// Method: propagateMetadata
//
// Descripton;
//
// This function propagates the metadata from the source to the
// destination in the map for pointer arithmetic operations~(gep) and
// bitcasts. This is the place where we need to shrink bounds.
//

void Nova::PropagateMetadata(Value* pointer_operand, 
                                      Instruction* inst, 
                                      int instruction_type){

  // Need to just propagate the base and bound here if I am not
  // shrinking bounds
  if(CheckBaseBoundMetadataPresent(inst)){
    // Metadata added to the map in the first pass
    return;
  }

  if(isa<ConstantPointerNull>(pointer_operand)) {
    AssociateBaseBound(inst, m_void_null_ptr, m_void_null_ptr);
    return;
  }

  if (CheckBaseBoundMetadataPresent(pointer_operand)) {
    Value* tmp_base = GetAssociatedBase(pointer_operand); 
    Value* tmp_bound = GetAssociatedBound(pointer_operand);       
    AssociateBaseBound(inst, tmp_base, tmp_bound);
  } else{
    if(isa<Constant>(pointer_operand)) {
      
      Value* tmp_base = NULL;
      Value* tmp_bound = NULL;
      Constant* given_constant = dyn_cast<Constant>(pointer_operand);
      GetConstantExprBaseBound(given_constant, tmp_base, tmp_bound);
      assert(tmp_base && "gep with cexpr and base null?");
      assert(tmp_bound && "gep with cexpr and bound null?");
      tmp_base = CastToVoidPtr(tmp_base, inst);
      tmp_bound = CastToVoidPtr(tmp_bound, inst);        
  
      AssociateBaseBound(inst, tmp_base, tmp_bound);
    } // Constant case ends here
    // Could be in the first pass, do nothing here
  }
}

void Nova::HandleGEP(GetElementPtrInst* gep_inst) {
  Value* getelementptr_operand = gep_inst->getPointerOperand();
  PropagateMetadata(getelementptr_operand, gep_inst, SBCETS_GEP);
}

//
// Method: handleBitCast
//
// Description: Propagate metadata from source to destination with
// pointer bitcast operations.

void Nova::HandleBitCast(BitCastInst* bitcast_inst) {

  Value* pointer_operand = bitcast_inst->getOperand(0);  
  PropagateMetadata(pointer_operand, bitcast_inst, SBCETS_BITCAST);
}

//
// The metadata propagation for PHINode occurs in two passes. In the
// first pass, SoftBound/CETS transformation just creates the metadata
// PHINodes and records it in the maps maintained by
// SoftBound/CETS. In the second pass, it populates the incoming
// values of the PHINodes. This two pass approach ensures that every
// incoming value of the original PHINode will have metadata in the
// SoftBound/CETS maps
// 

//
// Method: handlePHIPass1()
//
// Description:
//
// This function creates a PHINode for the metadata in the bitcode for
// pointer PHINodes. It is important to note that this function just
// creates the PHINode and does not populate the incoming values of
// the PHINode, which is handled by the handlePHIPass2.
//

void Nova::HandlePHIPass1(PHINode* phi_node) {

  // Not a Pointer PHINode, then just return
  if (!isa<PointerType>(phi_node->getType()))
    return;

  unsigned num_incoming_values = phi_node->getNumIncomingValues();

  PHINode* base_phi_node = PHINode::Create(m_void_ptr_type,
                                           num_incoming_values,
                                           "phi.base",
                                           phi_node);
  
  PHINode* bound_phi_node = PHINode::Create(m_void_ptr_type, 
                                            num_incoming_values,
                                            "phi.bound", 
                                            phi_node);
  
  Value* base_phi_node_value = base_phi_node;
  Value* bound_phi_node_value = bound_phi_node;
  
  AssociateBaseBound(phi_node, base_phi_node_value, bound_phi_node_value);
}

void Nova::AddMemcopyMemsetCheck(CallInst* call_inst, Function* called_func) {
  SmallVector<Value*, 8> args;

  if(called_func->getName().find("llvm.memcpy") == 0 || 
     called_func->getName().find("llvm.memmove") == 0){

    CallBase* call_base = dyn_cast<CallBase>(call_inst);
    assert(call_base && "Expected CallBase!");
    Value* dest_ptr = call_base->getArgOperand(0);
    Value* src_ptr  = call_base->getArgOperand(1);
    Value* size_ptr = call_base->getArgOperand(2);
    
    args.push_back(dest_ptr);
    args.push_back(src_ptr);

    Value* cast_size_ptr = size_ptr;
    if(size_ptr->getType() != m_key_type){
      BitCastInst* bitcast = new BitCastInst(size_ptr, m_key_type, "", call_inst);
      cast_size_ptr = bitcast;
    }

    args.push_back(cast_size_ptr);

    Value* dest_base = GetAssociatedBase(dest_ptr);
    Value* dest_bound =GetAssociatedBound(dest_ptr);
    
    Value* src_base = GetAssociatedBase(src_ptr);
    Value* src_bound = GetAssociatedBound(src_ptr);

    args.push_back(dest_base);
    args.push_back(dest_bound);
    
    args.push_back(src_base);
    args.push_back(src_bound);

    CallInst::Create(m_memcopy_check, args, "", call_inst);
    return;
  }

  if(called_func->getName().find("llvm.memset") == 0){

    args.clear();
    CallBase* call_base = dyn_cast<CallBase>(call_inst);
    Value* dest_ptr = call_base->getArgOperand(0);
    // Whats cs.getArgrument(1) return? Why am I not using it?
    Value* size_ptr = call_base->getArgOperand(2);

    Value* cast_size_ptr = size_ptr;
    assert(size_ptr != NULL);

    if(size_ptr->getType() != m_key_type){
      BitCastInst* bitcast = new BitCastInst(size_ptr, m_key_type, "", call_inst);
      cast_size_ptr = bitcast;
    }

    args.push_back(dest_ptr);
    args.push_back(cast_size_ptr);
    
    Value* dest_base = GetAssociatedBase(dest_ptr);
    Value* dest_bound = GetAssociatedBound(dest_ptr);
    args.push_back(dest_base);
    args.push_back(dest_bound);   

    CallInst::Create(m_memset_check, args, "", call_inst);
    return;
  }
}

void Nova::HandleMemcpy(CallInst* call_inst){
  Function* func = call_inst->getCalledFunction();
  if(!func)
    return;

  assert(func && "function is null?");

  CallBase* call_base = dyn_cast<CallBase>(call_inst);
  Value* arg1 = call_base->getArgOperand(0);
  Value* arg2 = call_base->getArgOperand(1);
  Value* arg3 = call_base->getArgOperand(2);

  SmallVector<Value*, 8> args;
  args.push_back(arg1);
  args.push_back(arg2);
  args.push_back(arg3);

  if(arg3->getType() == Type::getInt32Ty(arg3->getContext())){
    CallInst::Create(m_copy_metadata, args, "", call_inst);
  }
  else{
    //    CallInst::Create(m_copy_metadata, args, "", call_inst);
  }
  args.clear();

#if 0

  Value* arg1_base = castToVoidPtr(getAssociatedBase(arg1), call_inst);
  Value* arg1_bound = castToVoidPtr(getAssociatedBound(arg1), call_inst);
  Value* arg2_base = castToVoidPtr(getAssociatedBase(arg2), call_inst);
  Value* arg2_bound = castToVoidPtr(getAssociatedBound(arg2), call_inst);
  args.push_back(arg1);
  args.push_back(arg1_base);
  args.push_back(arg1_bound);
  args.push_back(arg2);
  args.push_back(arg2_base);
  args.push_back(arg2_bound);
  args.push_back(arg3);

  CallInst::Create(m_memcopy_check,args.begin(), args.end(), "", call_inst);

#endif
  return;
    
}

//
// Method: introduceShadowStackAllocation
//
// Description: For every function call that has a pointer argument or
// a return value, shadow stack is used to propagate metadata. This
// function inserts the shadow stack allocation C-handler that
// reserves space in the shadow stack by reserving the requiste amount
// of space based on the input passed to it(number of pointer
// arguments/return).


void Nova::IntroduceShadowStackAllocation(CallInst* call_inst){
    
  // Count the number of pointer arguments and whether a pointer return     
  int pointer_args_return = GetNumPointerArgsAndReturn(call_inst);
  if(pointer_args_return == 0)
    return;
  Value* total_ptr_args;    
  total_ptr_args = 
    ConstantInt::get(Type::getInt32Ty(call_inst->getType()->getContext()), 
                     pointer_args_return, false);

  SmallVector<Value*, 8> args;
  args.push_back(total_ptr_args);
  CallInst::Create(m_shadow_stack_allocate, args, "", call_inst);
}

//
// Method: introduceShadowStackStores
//
// Description: This function inserts a call to the shadow stack store
// C-handler that stores the metadata, before the function call in the
// bitcode for pointer arguments.

void Nova::IntroduceShadowStackStores(Value* ptr_value, 
                                              Instruction* insert_at, 
                                              int arg_no){
  if(!isa<PointerType>(ptr_value->getType()))
    return;

  Value* argno_value;    
  argno_value = 
    ConstantInt::get(Type::getInt32Ty(ptr_value->getType()->getContext()), 
                     arg_no, false);

  Value* ptr_base = GetAssociatedBase(ptr_value);
  Value* ptr_bound = GetAssociatedBound(ptr_value);
  
  Value* ptr_base_cast = CastToVoidPtr(ptr_base, insert_at);
  Value* ptr_bound_cast = CastToVoidPtr(ptr_bound, insert_at);

  SmallVector<Value*, 8> args;
  args.push_back(ptr_base_cast);
  args.push_back(argno_value);
  CallInst::Create(m_shadow_stack_base_store, args, "", insert_at);
  
  args.clear();
  args.push_back(ptr_bound_cast);
  args.push_back(argno_value);
  CallInst::Create(m_shadow_stack_bound_store, args, "", insert_at);    
}

//
// Method: introduceShadowStackDeallocation
//
// Description: This function inserts a call to the C-handler that
// deallocates the shadow stack space on function exit.
  

void Nova::IntroduceShadowStackDeallocation(CallInst* call_inst, Instruction* insert_at){

  int pointer_args_return = GetNumPointerArgsAndReturn(call_inst);
  if(pointer_args_return == 0)
    return;
  SmallVector<Value*, 8> args;    
  CallInst::Create(m_shadow_stack_deallocate, args, "", insert_at);
}

//
// Method: getNumPointerArgsAndReturn
//
// Description: Returns the number of pointer arguments and return.
//
int Nova::GetNumPointerArgsAndReturn(CallInst* call_inst){

  int total_pointer_count = 0;
  CallBase* call_base = dyn_cast<CallBase>(call_inst);
  for(unsigned i = 0; i <call_base->arg_size(); i++){
    Value* arg_value = call_base->getArgOperand(i);
    if(isa<PointerType>(arg_value->getType())){
      total_pointer_count++;
    }
  }

  if (total_pointer_count != 0) {
    // Reserve one for the return address if it has atleast one
    // pointer argument 
    total_pointer_count++;
  } else{
    // Increment the pointer arg return if the call instruction
    // returns a pointer
    if(isa<PointerType>(call_inst->getType())){
      total_pointer_count++;
    }
  }
  return total_pointer_count;
}

// 
// Method: introduceShadowStackLoads
//
// Description: This function introduces calls to the C-handlers that
// performs the loads from the shadow stack to retrieve the metadata.
// This function also associates the loaded metadata with the pointer
// arguments in the SoftBound/CETS maps.

void Nova::IntroduceShadowStackLoads(Value* ptr_value, 
                                             Instruction* insert_at, 
                                             int arg_no){
    
  if (!isa<PointerType>(ptr_value->getType()))
    return;
      
  Value* argno_value;    
  argno_value = 
    ConstantInt::get(Type::getInt32Ty(ptr_value->getType()->getContext()), 
                     arg_no, false);
    
  SmallVector<Value*, 8> args;
  args.clear();
  args.push_back(argno_value);
  Value* base = CallInst::Create(m_shadow_stack_base_load, args, "", 
                                 insert_at);    
  args.clear();
  args.push_back(argno_value);
  Value* bound = CallInst::Create(m_shadow_stack_bound_load, args, "", 
                                  insert_at);
  AssociateBaseBound(ptr_value, base, bound);
}

void Nova::IterateCallSiteIntroduceShadowStackStores(CallInst* call_inst){
    
  int pointer_args_return = GetNumPointerArgsAndReturn(call_inst);

  if(pointer_args_return == 0)
    return;
    
  int pointer_arg_no = 1;

  CallBase* call_base = dyn_cast<CallBase>(call_inst);
  for(unsigned i = 0; i < call_base->arg_size(); i++){
    Value* arg_value = call_base->getArgOperand(i);
    if(isa<PointerType>(arg_value->getType())){
      IntroduceShadowStackStores(arg_value, call_inst, pointer_arg_no);
      pointer_arg_no++;
    }
  }    
}

void Nova::HandleCallInst(CallInst* call_inst) {
  // Function* func = call_inst->getCalledFunction();
  Value* mcall = call_inst;

#if 0
  CallingConv::ID id = call_inst->getCallingConv();


  if(id == CallingConv::Fast){
    printf("fast calling convention not handled\n");
    exit(1);
  }
#endif 
    
  Function* func = call_inst->getCalledFunction();
  if(func && ((func->getName().find("llvm.memcpy") == 0) || 
              (func->getName().find("llvm.memmove") == 0))){
    AddMemcopyMemsetCheck(call_inst, func);
    HandleMemcpy(call_inst);
    return;
  }

  if(func && func->getName().find("llvm.memset") == 0){
    AddMemcopyMemsetCheck(call_inst, func);
  }

  if(func && IsFuncDefSoftBound(func->getName().str())){
    if(!isa<PointerType>(call_inst->getType())){
      return;
    }
    
    AssociateBaseBound(call_inst, m_void_null_ptr, m_void_null_ptr);
    return;
  }

  Instruction* insert_at = GetNextInstruction(call_inst);
  //  call_inst->setCallingConv(CallingConv::C);

  IntroduceShadowStackAllocation(call_inst);
  IterateCallSiteIntroduceShadowStackStores(call_inst);
    
  if(isa<PointerType>(mcall->getType())) {

      /* ShadowStack for the return value is 0 */
      IntroduceShadowStackLoads(call_inst, insert_at, 0);       
  }
  IntroduceShadowStackDeallocation(call_inst,insert_at);
}

//
// Method: handleSelect
//
// This function propagates the metadata with Select IR instruction.
// Select  instruction is also handled in two passes.

void Nova::HandleSelect(SelectInst* select_ins, int pass) {

  if (!isa<PointerType>(select_ins->getType())) 
    return;
    
  Value* condition = select_ins->getOperand(0);
  Value* operand_base[2];
  Value* operand_bound[2];    

  for(unsigned m = 0; m < 2; m++) {
    Value* operand = select_ins->getOperand(m+1);

    operand_base[m] = NULL;
    operand_bound[m] = NULL;

    if (CheckBaseBoundMetadataPresent(operand)) {      
      operand_base[m] = GetAssociatedBase(operand);
      operand_bound[m] = GetAssociatedBound(operand);
    }
    
    if (isa<ConstantPointerNull>(operand) && 
        !CheckBaseBoundMetadataPresent(operand)) {            
      operand_base[m] = m_void_null_ptr;
      operand_bound[m] = m_void_null_ptr;
    }        
      
    Constant* given_constant = dyn_cast<Constant>(operand);
    if(given_constant) {
      GetConstantExprBaseBound(given_constant, 
                               operand_base[m], 
                               operand_bound[m]);     
    }    
    assert(operand_base[m] != NULL && 
           "operand doesn't have base with select?");
    assert(operand_bound[m] != NULL && 
           "operand doesn't have bound with select?");
    
    // Introduce a bit cast if the types don't match 
    if (operand_base[m]->getType() != m_void_ptr_type) {          
      operand_base[m] = new BitCastInst(operand_base[m], m_void_ptr_type,
                                        "select.base", select_ins);          
    }
    
    if (operand_bound[m]->getType() != m_void_ptr_type) {
      operand_bound[m] = new BitCastInst(operand_bound[m], m_void_ptr_type,
                                         "select_bound", select_ins);
    }
  } // for loop ends
    
    SelectInst* select_base = SelectInst::Create(condition, 
                                                 operand_base[0], 
                                                 operand_base[1], 
                                                 "select.base",
                                                 select_ins);
    
    SelectInst* select_bound = SelectInst::Create(condition, 
                                                  operand_bound[0], 
                                                  operand_bound[1], 
                                                  "select.bound",
                                                  select_ins);
    AssociateBaseBound(select_ins, select_base, select_bound);
}

void Nova::HandleIntToPtr(IntToPtrInst* inttoptrinst) {
    
  Value* inst = inttoptrinst;
    
  AssociateBaseBound(inst, m_void_null_ptr, m_void_null_ptr);
}

//
// Method: handleReturnInst
//
// Description: 
// This function inserts C-handler calls to store
// metadata for return values in the shadow stack.

void Nova::HandleReturnInst(ReturnInst* ret){

  Value* pointer = ret->getReturnValue();
  if(pointer == NULL){
    return;
  }
  if(isa<PointerType>(pointer->getType())){
    IntroduceShadowStackStores(pointer, ret, 0);
  }
}

void Nova::HandleExtractElement(ExtractElementInst* EEI){
  
  if(!isa<PointerType>(EEI->getType()))
     return;
  
  Value* EEIOperand = EEI->getOperand(0);
  
  if(isa<VectorType>(EEIOperand->getType())){
    
    if(!m_vector_pointer_base.count(EEIOperand) ||
       !m_vector_pointer_bound.count(EEIOperand)){
      assert(0 && "Extract element does not have vector metadata");
    }

    Constant* index = dyn_cast<Constant>(EEI->getOperand(1));
    
    Value* vector_base = m_vector_pointer_base[EEIOperand];
    Value* vector_bound = m_vector_pointer_bound[EEIOperand];
    
    Value* ptr_base = ExtractElementInst::Create(vector_base, index, "", EEI);
    Value* ptr_bound = ExtractElementInst::Create(vector_bound, index, "", EEI);
    
    AssociateBaseBound(EEI, ptr_base, ptr_bound);
    return;
  }
     
  assert (0 && "ExtractElement is returning a pointer, possibly some vectorization going on, not handled, try running with O0 or O1 or O2");    
}

void Nova::HandleExtractValue(ExtractValueInst* EVI){
    if(isa<PointerType>(EVI->getType())){
        assert(0 && "ExtractValue is returning a pointer, possibly some vectorization going on, not handled, try running with O0 or O1 or O2");
    }
  
    AssociateBaseBound(EVI, m_void_null_ptr, m_infinite_bound_ptr);

  return;  
}

void Nova::GatherBaseBoundPass1 (Function * func) {
  int arg_count= 0;
    
  //    std::cerr<<"transforming function with name:"<<func->getName()<< "\n";
  /* Scan over the pointer arguments and introduce base and bound */

  for(Function::arg_iterator ib = func->arg_begin(), ie = func->arg_end();
      ib != ie; ++ib) {

    if(!isa<PointerType>(ib->getType())) 
      continue;

    /* it is a pointer, so increment the arg count */
    arg_count++;

    Argument* ptr_argument = dyn_cast<Argument>(ib);
    Value* ptr_argument_value = ptr_argument;
    //Instruction* fst_inst = &*(func->begin()->begin());
      
    /* Urgent: Need to think about what we need to do about byval attributes */
    if(ptr_argument->hasByValAttr()){
      
      if(CheckTypeHasPtrs(ptr_argument)){
        assert(0 && "Pointer argument has byval attributes and the underlying structure returns pointers");
      }
      
      AssociateBaseBound(ptr_argument_value, m_void_null_ptr, m_infinite_bound_ptr);
    }
    else{
      // TODOO: don't know what byval attributes means here.
      // introduceShadowStackLoads(ptr_argument_value, fst_inst, arg_count);
      //      introspectMetadata(func, ptr_argument_value, fst_inst, arg_count);
    }
  }

  /* WorkList Algorithm for propagating the base and bound. Each
   * basic block is visited only once. We start by visiting the
   * current basic block, then push all the successors of the
   * current basic block on to the queue if it has not been visited
   */
  std::set<BasicBlock*> bb_visited;
  std::queue<BasicBlock*> bb_worklist;
  Function:: iterator bb_begin = func->begin();

  BasicBlock* bb = dyn_cast<BasicBlock>(bb_begin);
  assert( bb && "Not a basic block and I am gathering base and bound?");
  bb_worklist.push(bb);

  while(bb_worklist.size() != 0) {

    bb = bb_worklist.front();
    assert(bb && "Not a BasicBlock?");

    bb_worklist.pop();
    if( bb_visited.count(bb)) {
      /* Block already visited */
      continue;
    }
    /* If here implies basic block not visited */
      
    /* Insert the block into the set of visited blocks */
    bb_visited.insert(bb);

    /* Iterating over the successors and adding the successors to
     * the work list
     */
    for(succ_iterator si = succ_begin(bb), se = succ_end(bb); si != se; ++si) {

      BasicBlock* next_bb = *si;
      assert(next_bb && "Not a basic block and I am adding to the base and bound worklist?");
      bb_worklist.push(next_bb);
    }
      
    for(BasicBlock::iterator i = bb->begin(), ie = bb->end(); i != ie; ++i){
        Value* v1 = dyn_cast<Value>(i);
        Instruction* new_inst = dyn_cast<Instruction>(i);


        /* If the instruction is not present in the original, no
         * instrumentaion 
         */
        if(!m_present_in_original.count(v1)) {
          continue;
        }

        /* All instructions have been defined here as defining it in
         * switch causes compilation errors. Assertions have been in
         * the inserted in the specific cases
         */
        //errs() <<"inst: " << *new_inst << "\n";

        switch(new_inst->getOpcode()) {
          
        case Instruction::Alloca:
          {
            AllocaInst* alloca_inst = dyn_cast<AllocaInst>(v1);
            assert(alloca_inst && "Not an Alloca inst?");
            HandleAlloca(alloca_inst, bb, i);
          }
          break;

        case Instruction::Load:
          {
            LoadInst* load_inst = dyn_cast<LoadInst>(v1);            
            assert(load_inst && "Not a Load inst?");
            HandleLoad(load_inst);
          }
          break;

        case Instruction::GetElementPtr:
          {
            GetElementPtrInst* gep_inst = dyn_cast<GetElementPtrInst>(v1);
            assert(gep_inst && "Not a GEP inst?");
            HandleGEP(gep_inst);
          }
          break;
	
        case BitCastInst::BitCast:
          {
            BitCastInst* bitcast_inst = dyn_cast<BitCastInst>(v1);
            assert(bitcast_inst && "Not a BitCast inst?");
            HandleBitCast(bitcast_inst);
          }
          break;

        case Instruction::PHI:
          {
            PHINode* phi_node = dyn_cast<PHINode>(v1);
            assert(phi_node && "Not a phi node?");
            //printInstructionMap(v1);
            HandlePHIPass1(phi_node);
          }
          /* PHINode ends */
          break;
          
        case Instruction::Call:
          {
            CallInst* call_inst = dyn_cast<CallInst>(v1);
            assert(call_inst && "Not a Call inst?");
            HandleCallInst(call_inst);
          }
          break;

        case Instruction::Select:
          {
            SelectInst* select_insn = dyn_cast<SelectInst>(v1);
            assert(select_insn && "Not a select inst?");
            int pass = 1;
            HandleSelect(select_insn, pass);
          }
          break;

        case Instruction::Store:
          {
            break;
          }

        case Instruction::IntToPtr:
          {
            IntToPtrInst* inttoptrinst = dyn_cast<IntToPtrInst>(v1);
            assert(inttoptrinst && "Not a IntToPtrInst?");
            HandleIntToPtr(inttoptrinst);
            break;
          }

        case Instruction::Ret:
          {
            ReturnInst* ret = dyn_cast<ReturnInst>(v1);
            assert(ret && "not a return inst?");
            HandleReturnInst(ret);
          }
          break;
	
        case Instruction::ExtractElement:
	    {
	      ExtractElementInst * EEI = dyn_cast<ExtractElementInst>(v1);
	      assert(EEI && "ExtractElementInst inst?");
	      HandleExtractElement(EEI);
	    }
	    break;

        case Instruction::ExtractValue:
	    {
	      ExtractValueInst * EVI = dyn_cast<ExtractValueInst>(v1);
	      assert(EVI && "handle extract value inst?");
	      HandleExtractValue(EVI);
	    }
	    break;
            
        default:
          if(isa<PointerType>(v1->getType()))
            assert(!isa<PointerType>(v1->getType())&&
                   " Generating Pointer and not being handled");
        } // for-end
    }/* Basic Block iterator Ends */
  } /* Function iterator Ends */
}

void Nova::HandleVectorStore(StoreInst* store_inst){

  Value* operand = store_inst->getOperand(0);
  Value* pointer_dest = store_inst->getOperand(1);
  Instruction* insert_at = GetNextInstruction(store_inst);

  if(!m_vector_pointer_base.count(operand)){
    assert(0 && "vector base not found");
  }
  if(!m_vector_pointer_bound.count(operand)){
    assert(0 && "vector bound not found");
  }
  
  Value* vector_base = m_vector_pointer_base[operand];
  Value* vector_bound = m_vector_pointer_bound[operand];

  const VectorType* vector_ty = dyn_cast<VectorType>(operand->getType());
  ElementCount EC = vector_ty->getElementCount();
  uint64_t num_elements = EC.getKnownMinValue(); // 如果你只处理固定长度向量
  if (num_elements > 2){
    assert(0 && "more than 2 element vectors not handled");
  }

  Value* pointer_operand_bitcast = CastToVoidPtr(pointer_dest, insert_at);
  for (uint64_t i = 0; i < num_elements; i++){
    Constant* index = ConstantInt::get(Type::getInt32Ty(store_inst->getContext()), i);

    Value* ptr_base = ExtractElementInst::Create(vector_base, index,"", insert_at);
    Value* ptr_bound = ExtractElementInst::Create(vector_bound, index, "", insert_at);
    
    SmallVector<Value*, 8> args;
    args.clear();

    args.push_back(pointer_operand_bitcast);
    args.push_back(ptr_base);
    args.push_back(ptr_bound);
    args.push_back(index);

    CallInst::Create(m_metadata_store_vector_func, args, "", insert_at);    
  }
}   

void Nova::HandleStore(StoreInst* store_inst) {
  Value* operand = store_inst->getOperand(0);
  Value* pointer_dest = store_inst->getOperand(1);
  Instruction* insert_at = GetNextInstruction(store_inst);
    
  if(isa<VectorType>(operand->getType())){
    const VectorType* vector_ty = dyn_cast<VectorType>(operand->getType());
    if(isa<PointerType>(vector_ty->getElementType())){
      HandleVectorStore(store_inst);
      return;
    }
  }

  /* If a pointer is being stored, then the base and bound
   * corresponding to the pointer must be stored in the shadow space
   */
  if(!isa<PointerType>(operand->getType()))
    return;
      

  if(isa<ConstantPointerNull>(operand)) {
    /* it is a constant pointer null being stored
     * store null to the shadow space
     */
#if 0    
    StructType* ST = dyn_cast<StructType>(operand->getType());

    if(ST){
      if(ST->isOpaque()){
        DEBUG(errs()<<"Opaque type found\n");        
      }

    }
      Value* size_of_type = getSizeOfType(operand->getType());
#endif

      Value* size_of_type = NULL;

      AddStoreBaseBoundFunc(pointer_dest, m_void_null_ptr, 
                            m_void_null_ptr, m_void_null_ptr, 
                            size_of_type, insert_at);

    return; 
  }

      
  /* if it is a global expression being stored, then add add
   * suitable base and bound
   */
    
  Value* tmp_base = NULL;
  Value* tmp_bound = NULL;

  //  Value* xmm_base_bound = NULL;
  //  Value* xmm_key_lock = NULL;
    
  Constant* given_constant = dyn_cast<Constant>(operand);
  if(given_constant) {      
      GetConstantExprBaseBound(given_constant, tmp_base, tmp_bound);
      assert(tmp_base && "global doesn't have base");
      assert(tmp_bound && "global doesn't have bound");        
  } else {      
    /* storing an external function pointer */
      if(!CheckBaseBoundMetadataPresent(operand)) {
        return;
      }

      tmp_base = GetAssociatedBase(operand);
      tmp_bound = GetAssociatedBound(operand);              
  }    
  
  /* Store the metadata into the metadata space
   */
  

  //  Type* stored_pointer_type = operand->getType();
  Value* size_of_type = NULL;
  //    Value* size_of_type  = getSizeOfType(stored_pointer_type);
  AddStoreBaseBoundFunc(pointer_dest, tmp_base, tmp_bound, operand,  size_of_type, insert_at);    
  
}

//
// Method: getGlobalVariableBaseBound

// Description: This function returns the base and bound for the
// global variables in the input reference arguments. This function
// may now be obsolete. We should try to use getConstantExprBaseBound
// instead in all places.
void Nova::GetGlobalVariableBaseBound(Value* operand, 
                                              Value* & operand_base, 
                                              Value* & operand_bound){

  GlobalVariable* gv = dyn_cast<GlobalVariable>(operand);
  Module* module = gv->getParent();
  assert(gv && "[getGlobalVariableBaseBound] not a global variable?");
    
  std::vector<Constant*> indices_base;
  Constant* index_base = 
    ConstantInt::get(Type::getInt32Ty(module->getContext()), 0);
  indices_base.push_back(index_base);

  Constant* base_exp = ConstantExpr::getGetElementPtr(nullptr, gv, indices_base);
        
  std::vector<Constant*> indices_bound;
  Constant* index_bound = 
    ConstantInt::get(Type::getInt32Ty(module->getContext()), 1);
  indices_bound.push_back(index_bound);

  Constant* bound_exp = ConstantExpr::getGetElementPtr(nullptr, gv, indices_bound);
    
  operand_base = base_exp;
  operand_bound = bound_exp;    
}

//
// Method: handlePHIPass2()
//
// Description: This pass fills the incoming values for the metadata
// PHINodes inserted in the first pass. There are four cases that
// needs to be handled for each incoming value.  First, if the
// incoming value is a ConstantPointerNull, then base, bound, key,
// lock will be default values.  Second, the incoming value can be an
// undef which results in default metadata values.  Third, Global
// variables need to get the same base and bound for each
// occurence. So we maintain a map which maps the base and boundfor
// each global variable in the incoming value.  Fourth, by default it
// retrieves the metadata from the SoftBound/CETS maps.

// Check if we need separate global variable and constant expression
// cases.

void Nova::HandlePHIPass2(PHINode* phi_node) {
  // Work to be done only for pointer PHINodes.
  if (!isa<PointerType>(phi_node->getType())) 
    return;

  PHINode* base_phi_node = NULL;
  PHINode* bound_phi_node  = NULL;

  // Obtain the metada PHINodes 
  base_phi_node = dyn_cast<PHINode>(GetAssociatedBase(phi_node));
  bound_phi_node = dyn_cast<PHINode>(GetAssociatedBound(phi_node));

  std::map<Value*, Value*> globals_base;
  std::map<Value*, Value*> globals_bound;
 
  unsigned num_incoming_values = phi_node->getNumIncomingValues();
  for (unsigned m = 0; m < num_incoming_values; m++) {

    Value* incoming_value = phi_node->getIncomingValue(m);
    BasicBlock* bb_incoming = phi_node->getIncomingBlock(m);

    if (isa<ConstantPointerNull>(incoming_value)) {
        base_phi_node->addIncoming(m_void_null_ptr, bb_incoming);
        bound_phi_node->addIncoming(m_void_null_ptr, bb_incoming);
      continue;
    } // ConstantPointerNull ends
   
    // The incoming vlaue can be a UndefValue
    if (isa<UndefValue>(incoming_value)) {        
        base_phi_node->addIncoming(m_void_null_ptr, bb_incoming);
        bound_phi_node->addIncoming(m_void_null_ptr, bb_incoming);
      continue;
    } // UndefValue ends
      
    Value* incoming_value_base = NULL;
    Value* incoming_value_bound = NULL;
    
    // handle global variables      
    GlobalVariable* gv = dyn_cast<GlobalVariable>(incoming_value);
    if (gv) {
        if (!globals_base.count(gv)) {
          Value* tmp_base = NULL;
          Value* tmp_bound = NULL;
          GetGlobalVariableBaseBound(incoming_value, tmp_base, tmp_bound);
          assert(tmp_base && "base of a global variable null?");
          assert(tmp_bound && "bound of a global variable null?");
          
          Function * PHI_func = phi_node->getParent()->getParent();
          Instruction* PHI_func_entry = &*(PHI_func->begin()->begin());
          
          incoming_value_base = CastToVoidPtr(tmp_base, PHI_func_entry);                                               
          incoming_value_bound = CastToVoidPtr(tmp_bound, PHI_func_entry);
            
          globals_base[incoming_value] = incoming_value_base;
          globals_bound[incoming_value] = incoming_value_bound;       
        } else {
          incoming_value_base = globals_base[incoming_value];
          incoming_value_bound = globals_bound[incoming_value];          
        }
    } // global variable ends
      
    // handle constant expressions 
    Constant* given_constant = dyn_cast<Constant>(incoming_value);
    if (given_constant) {
        if (!globals_base.count(incoming_value)) {
          Value* tmp_base = NULL;
          Value* tmp_bound = NULL;
          GetConstantExprBaseBound(given_constant, tmp_base, tmp_bound);
          assert(tmp_base && tmp_bound  &&
                 "[handlePHIPass2] tmp_base tmp_bound, null?");
          
          Function* PHI_func = phi_node->getParent()->getParent();
          Instruction* PHI_func_entry = &*(PHI_func->begin()->begin());

          incoming_value_base = CastToVoidPtr(tmp_base, PHI_func_entry);
          incoming_value_bound = CastToVoidPtr(tmp_bound, PHI_func_entry);
          
          globals_base[incoming_value] = incoming_value_base;
          globals_bound[incoming_value] = incoming_value_bound;        
        }
        else{
          incoming_value_base = globals_base[incoming_value];
          incoming_value_bound = globals_bound[incoming_value];          
        }
    }
    
    // handle values having map based pointer base and bounds 
    if(CheckBaseBoundMetadataPresent(incoming_value)){
      incoming_value_base = GetAssociatedBase(incoming_value);
      incoming_value_bound = GetAssociatedBound(incoming_value);
    }

    assert(incoming_value_base &&
           "[handlePHIPass2] incoming_value doesn't have base?");
    assert(incoming_value_bound && 
           "[handlePHIPass2] incoming_value doesn't have bound?");
    
    base_phi_node->addIncoming(incoming_value_base, bb_incoming);
    bound_phi_node->addIncoming(incoming_value_bound, bb_incoming);
  } // Iterating over incoming values ends 

  assert(base_phi_node && "[handlePHIPass2] base_phi_node null?");
  assert(bound_phi_node && "[handlePHIPass2] bound_phi_node null?");

  unsigned n_values = phi_node->getNumIncomingValues();
  unsigned n_base_values = base_phi_node->getNumIncomingValues();
  unsigned n_bound_values = bound_phi_node->getNumIncomingValues();    
  assert((n_values == n_base_values)  && 
         "[handlePHIPass2] number of values different for base");
  assert((n_values == n_bound_values) && 
         "[handlePHIPass2] number of values different for bound");
}
  

void Nova::GatherBaseBoundPass2(Function* func){

  /* WorkList Algorithm for propagating base and bound. Each basic
   * block is visited only once
   */
  std::set<BasicBlock*> bb_visited;
  std::queue<BasicBlock*> bb_worklist;
  Function::iterator bb_begin = func->begin();

  BasicBlock* bb = dyn_cast<BasicBlock>(bb_begin);
  assert(bb && "Not a basic block and gathering base bound in the next pass?");
  bb_worklist.push(bb);
    
  while( bb_worklist.size() != 0) {

    bb = bb_worklist.front();
    assert(bb && "Not a BasicBlock?");

    bb_worklist.pop();
    if( bb_visited.count(bb)) {
      /* Block already visited */

      continue;
    }
    /* If here implies basic block not visited */
      
    /* Insert the block into the set of visited blocks */
    bb_visited.insert(bb);

    /* Iterating over the successors and adding the successors to
     * the work list
     */
    for(succ_iterator si = succ_begin(bb), se = succ_end(bb); si != se; ++si) {

      BasicBlock* next_bb = *si;
      assert(next_bb && "Not a basic block and I am adding to the base and bound worklist?");
      bb_worklist.push(next_bb);
    }

    for(BasicBlock::iterator i = bb->begin(), ie = bb->end(); i != ie; ++i) {
      Value* v1 = dyn_cast<Value>(i);
      Instruction* new_inst = dyn_cast<Instruction>(i);

      // If the instruction is not present in the original, no instrumentaion
      if(!m_present_in_original.count(v1))
        continue;

      switch(new_inst->getOpcode()) {

      case Instruction::GetElementPtr:
        {
          GetElementPtrInst* gep_inst = dyn_cast<GetElementPtrInst>(v1);         
          assert(gep_inst && "Not a GEP instruction?");
          HandleGEP(gep_inst);
        }
        break;
          
      case Instruction::Store:
        {
          StoreInst* store_inst = dyn_cast<StoreInst>(v1);
          assert(store_inst && "Not a Store instruction?");
          HandleStore(store_inst);
        }
        break;

      case Instruction::PHI:
        {
          PHINode* phi_node = dyn_cast<PHINode>(v1);
          assert(phi_node && "Not a PHINode?");
          HandlePHIPass2(phi_node);
        }
        break;
 
      case BitCastInst::BitCast:
        {
          BitCastInst* bitcast_inst = dyn_cast<BitCastInst>(v1);
          assert(bitcast_inst && "Not a bitcast instruction?");
          HandleBitCast(bitcast_inst);
        }
        break;

      case SelectInst::Select:
        {
        }
        break;
          
      default:
        break;
      }/* Switch Ends */
    }/* BasicBlock iterator Ends */
  }/* Function iterator Ends */
}

void Nova::AddBaseBoundGlobals(Module& M){
  /* iterate over the globals here */

  for(Module::global_iterator it = M.global_begin(), ite = M.global_end(); it != ite; ++it){
    
    GlobalVariable* gv = dyn_cast<GlobalVariable>(it);
    
    if(!gv){
      continue;
    }

    if(StringRef(gv->getSection()) == "llvm.metadata"){
      continue;
    }
    if(gv->getName() == "llvm.global_ctors"){
      continue;
    }
    
    if(!gv->hasInitializer())
      continue;
    
    /* gv->hasInitializer() is true */
    
    Constant* initializer = dyn_cast<Constant>(it->getInitializer());
    ConstantArray* constant_array = dyn_cast<ConstantArray>(initializer);    
    if(initializer && (isa<StructType>(initializer->getType())||isa<ArrayType>(initializer->getType()))){

      if(isa<StructType>(initializer->getType())){
        std::vector<Constant*> indices_addr_ptr;
        Constant* index1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0);
        indices_addr_ptr.push_back(index1);
        StructType* struct_type = dyn_cast<StructType>(initializer->getType());
        HandleGlobalStructTypeInitializer(M, struct_type, initializer, gv, indices_addr_ptr, 1);
        continue;
      }
      
      if(isa<ArrayType>(initializer->getType())||isa<VectorType>(initializer->getType())||isa<PointerType>(initializer->getType())){
        HandleGlobalSequentialTypeInitializer(M, gv);
      }
    }
    
    if(initializer && !constant_array){
      
      if(isa<PointerType>(initializer->getType())){
        //        std::cerr<<"Pointer type initializer\n";
      }
    }
    
    if(!constant_array)
      continue;
    
    int num_ca_opds = constant_array->getNumOperands();
    
    for(int i = 0; i < num_ca_opds; i++){
      Value* initializer_opd = constant_array->getOperand(i);
      Instruction* first = GetGlobalInitInstruction(M);
      Value* operand_base = NULL;
      Value* operand_bound = NULL;
      
      Constant* global_constant_initializer = dyn_cast<Constant>(initializer_opd);
      if(!isa<PointerType>(global_constant_initializer->getType())){
        break;
      }
      GetConstantExprBaseBound(global_constant_initializer, operand_base, operand_bound);
      
      SmallVector<Value*, 8> args;
      Constant* index1 = ConstantInt::get(Type::getInt32Ty(M.getContext()), 0);
      Constant* index2 = ConstantInt::get(Type::getInt32Ty(M.getContext()), i);

      std::vector<Constant*> indices_addr_ptr;
      indices_addr_ptr.push_back(index1);
      indices_addr_ptr.push_back(index2);

      Constant* addr_of_ptr = ConstantExpr::getGetElementPtr(nullptr, gv, indices_addr_ptr);
      Type* initializer_type = initializer_opd->getType();
      Value* initializer_size = GetSizeOfType(initializer_type);
      
      AddStoreBaseBoundFunc(addr_of_ptr, operand_base, operand_bound, initializer_opd, initializer_size, first);
      
    }
  }

}

//
//
// Method: addLoadStoreChecks
//
// Description: This function inserts calls to C-handler spatial
// safety check functions and elides the check if the map says it is
// not necessary to check.

void Nova::AddLoadStoreChecks(Instruction* load_store, 
                                      std::map<Value*, int>& FDCE_map) {
  SmallVector<Value*, 8> args;
  Value* pointer_operand = NULL;
    
  if(isa<LoadInst>(load_store)) {
    LoadInst* ldi = dyn_cast<LoadInst>(load_store);
    assert(ldi && "not a load instruction");
    pointer_operand = ldi->getPointerOperand();
  }
    
  if(isa<StoreInst>(load_store)){
    StoreInst* sti = dyn_cast<StoreInst>(load_store);
    assert(sti && "not a store instruction");
    // The pointer where the element is being stored is the second
    // operand
    pointer_operand = sti->getOperand(1);
  }
    
  assert(pointer_operand && "pointer operand null?");

  // If it is a null pointer which is being loaded, then it must seg
  // fault, no dereference check here
  
  
  if(isa<ConstantPointerNull>(pointer_operand))
    return;

  // Find all uses of pointer operand, then check if it dominates and
  //if so, make a note in the map
  
  GlobalVariable* gv = dyn_cast<GlobalVariable>(pointer_operand);    
  if(gv && GLOBALCONSTANTOPT && !isa<ArrayType>(gv->getType())&&!isa<PointerType>(gv->getType())&&!isa<VectorType>(gv->getType())){ 
    return;
  }
  
  if(BOUNDSCHECKOPT) {
    // Enable dominator based dereference check optimization only when
    // suggested
    
    if(FDCE_map.count(load_store)) {
      return;
    }
    
    // FIXME: Add more comments here Iterate over the uses
    
    for(Value::use_iterator ui = pointer_operand->use_begin(), 
          ue = pointer_operand->use_end(); 
        ui != ue; ++ui) {
      
      Instruction* temp_inst = dyn_cast<Instruction>(*ui);       
      if(!temp_inst)
        continue;
      
      if(temp_inst == load_store)
        continue;
      
      if(!isa<LoadInst>(temp_inst) && !isa<StoreInst>(temp_inst))
        continue;
      
      if(isa<StoreInst>(temp_inst)){
        if(temp_inst->getOperand(1) != pointer_operand){
          // When a pointer is a being stored at at a particular
          // address, don't elide the check
          continue;
        }
      }
      
#if 0
        if(m_dominator_tree->dominates(load_store, temp_inst)) {
          if(!FDCE_map.count(temp_inst)) {
            FDCE_map[temp_inst] = true;
            continue;
          }                  
        }
#endif
    } // Iterating over uses ends 
  } // BOUNDSCHECKOPT ends 
    
  Value* tmp_base = NULL;
  Value* tmp_bound = NULL;
    
  Constant* given_constant = dyn_cast<Constant>(pointer_operand);    
  if(given_constant ) {
    if(GLOBALCONSTANTOPT)
      return;      

    GetConstantExprBaseBound(given_constant, tmp_base, tmp_bound);
  }
  else {
    tmp_base = GetAssociatedBase(pointer_operand);
    tmp_bound = GetAssociatedBound(pointer_operand);
  }

  Value* bitcast_base = CastToVoidPtr(tmp_base, load_store);
  args.push_back(bitcast_base);
  
  Value* bitcast_bound = CastToVoidPtr(tmp_bound, load_store);    
  args.push_back(bitcast_bound);
   
  Value* cast_pointer_operand_value = CastToVoidPtr(pointer_operand, 
                                                    load_store);    
  args.push_back(cast_pointer_operand_value);
    
  // Pushing the size of the type 
  Type* pointer_operand_type = pointer_operand->getType();
  Value* size_of_type = GetSizeOfType(pointer_operand_type);
  args.push_back(size_of_type);

  if(isa<LoadInst>(load_store)){
            
    CallInst::Create(m_spatial_load_dereference_check, args, "", load_store);
  }
  else{    
    CallInst::Create(m_spatial_store_dereference_check, args, "", load_store);
  }

  return;
}

void Nova::AddDereferenceChecks(Function* func, ValueSet &vs) {
  Function &F = *func;
  Value* pointer_operand = NULL;
  
  if(func->isVarArg())
    return;

#if 0
  if(Blacklist->isIn(F))
    return;

#endif

  std::vector<Instruction*> CheckWorkList;
  std::map<Value*, bool> ElideSpatialCheck;
  std::map<Value*, bool> ElideTemporalCheck;

  // identify all the instructions where we need to insert the spatial checks
  for(inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i){
    Instruction* I = &*i;

    if(!m_present_in_original.count(I)){
      continue;
    }
    // add check optimizations here
    // add checks for memory fences and atomic exchanges
    if(isa<LoadInst>(I) || isa<StoreInst>(I)){
      CheckWorkList.push_back(I);
    }     
    if(isa<AtomicCmpXchgInst>(I) || isa<AtomicRMWInst>(I)){
      assert(0 && "Atomic Instructions not handled");
    }    
  }

#if 0
  // spatial check optimizations here 

  for(std::vector<Instruction*>::iterator i = CheckWorkList.begin(), 
	e = CheckWorkList.end(); i!= e; ++i){

    Instruction* inst = *i;
    Value* pointer_operand = NULL;
    
    if(ElideSpatialCheck.count(inst))
      continue;
    
    if(isa<LoadInst>(inst)){
      LoadInst* ldi = dyn_cast<LoadInst>(inst);
      pointer_operand = ldi->getPointerOperand();
    }
    if(isa<StoreInst>(inst)){
      StoreInst* st = dyn_cast<StoreInst>(inst);
      pointer_operand = st->getOperand(1);      
    }

    for(Value::use_iterator ui = pointer_operand->use_begin(),  
	  ue = pointer_operand->use_end();
	ui != ue; ++ui){

      Instruction* use_inst = dyn_cast<Instruction>(*ui);
      if(!use_inst || (use_inst == inst))
	continue;

      if(!isa<LoadInst>(use_inst)  && !isa<StoreInst>(use_inst))
	continue;

      if(isa<StoreInst>(use_inst)){
	if(use_inst->getOperand(1) != pointer_operand)
	  continue;
      }

#if 0
      if(m_dominator_tree->dominates(inst, use_inst)){
	if(!ElideSpatialCheck.count(use_inst))
	  ElideSpatialCheck[use_inst] = true;		
      }
    }
#endif  
  }

#endif

  //Temporal Check Optimizations

  
#if 0

#endif

  /* intra-procedural load dererference check elimination map */
  std::map<Value*, int> func_deref_check_elim_map;
  std::map<Value*, int> func_temporal_check_elim_map;

  /* WorkList Algorithm for adding dereference checks. Each basic
   * block is visited only once. We start by visiting the current
   * basic block, then pushing all the successors of the current
   * basic block on to the queue if it has not been visited
   */
    
  std::set<BasicBlock*> bb_visited;
  std::queue<BasicBlock*> bb_worklist;
  Function:: iterator bb_begin = func->begin();

  BasicBlock* bb = dyn_cast<BasicBlock>(bb_begin);
  assert(bb && "Not a basic block  and I am adding dereference checks?");
  bb_worklist.push(bb);

    
  while(bb_worklist.size() != 0) {
      
    bb = bb_worklist.front();
    assert(bb && "Not a BasicBlock?");
    bb_worklist.pop();

    if(bb_visited.count(bb)) {
      /* Block already visited */
      continue;
    }

    /* If here implies basic block not visited */
    /* Insert the block into the set of visited blocks */
    bb_visited.insert(bb);

    /* Iterating over the successors and adding the successors to
     * the worklist
     */
    for(succ_iterator si = succ_begin(bb), se = succ_end(bb); si != se; ++si) {
        
      BasicBlock* next_bb = *si;
      assert(next_bb && "Not a basic block and I am adding to the base and bound worklist?");
      bb_worklist.push(next_bb);
    }

    /* basic block load deref check optimization */
    std::map<Value*, int> bb_deref_check_map;
    std::map<Value*, int> bb_temporal_check_elim_map;
    /* structure check optimization */
    std::map<Value*, int> bb_struct_check_opt;

    for(BasicBlock::iterator i = bb->begin(), ie = bb->end(); i != ie; ++i){
      Value* v1 = dyn_cast<Value>(i);
      Instruction* new_inst = dyn_cast<Instruction>(i);
      
      /* Do the dereference check stuff */
      if(!m_present_in_original.count(v1))
        continue;
      
      if(isa<LoadInst>(new_inst)){
        LoadInst* ldi = dyn_cast<LoadInst>(new_inst);
        pointer_operand = ldi->getPointerOperand();
        if (vs.count(pointer_operand) == 0) {
            //errs() << "skip check for non-sensitive pointers\n" ;
            continue;
        }
        AddLoadStoreChecks(new_inst, func_deref_check_elim_map);
        continue;
      }

      if(isa<StoreInst>(new_inst)){
        StoreInst* st = dyn_cast<StoreInst>(new_inst);
        pointer_operand = st->getOperand(1);      
        if (vs.count(pointer_operand) == 0) {
            //errs() << "skip check for non-sensitive pointers\n" ;
            continue;
        }
        AddLoadStoreChecks(new_inst, func_deref_check_elim_map);
        continue;
      }

      /* check call through function pointers */
      if(isa<CallInst>(new_inst)) {
          
        // ZC:COMMENT OUT CALL CHECK DUE TO COMPLAINS ABOUT NO BASE AND BOUND INFO
        if(CALLCHECKS) {
          continue;
        }          
	  

        SmallVector<Value*, 8> args;
        CallInst* call_inst = dyn_cast<CallInst>(new_inst);
        Value* tmp_base = NULL;
        Value* tmp_bound = NULL;
        
        assert(call_inst && "call instruction null?");
        errs() << "DEBUG: " << *call_inst;
        
        if(!INDIRECTCALLCHECKS)
          continue;

        /* TODO:URGENT : indirect function call checking commented
         * out for the time being to test other aspect of the code,
         * problem was with spec benchmarks perl and h264. They were
         * primarily complaining that the use of a function did not
         * have base and bound in the map
         */


        /* here implies its an indirect call */
        Value* indirect_func_called = call_inst->getOperand(0);
            
        Constant* func_constant = dyn_cast<Constant>(indirect_func_called);
        if(func_constant) {
          GetConstantExprBaseBound(func_constant, tmp_base, tmp_bound);           
        }
        else {
          tmp_base = GetAssociatedBase(indirect_func_called);
          tmp_bound = GetAssociatedBound(indirect_func_called);
        }
        /* Add BitCast Instruction for the base */
        Value* bitcast_base = CastToVoidPtr(tmp_base, new_inst);
        args.push_back(bitcast_base);
            
        /* Add BitCast Instruction for the bound */
        Value* bitcast_bound = CastToVoidPtr(tmp_bound, new_inst);
        args.push_back(bitcast_bound);
        Value* pointer_operand_value = CastToVoidPtr(indirect_func_called, new_inst);
        args.push_back(pointer_operand_value);            
        CallInst::Create(m_call_dereference_func, args, "", new_inst);
        continue;
      } /* Call check ends */
    }
  }  
}

//
// Method: transformMain()
//
// Description:
//
// This method renames the function "main" in the module as
// pseudo_main. The C-handler has the main function which calls
// pseudo_main. Actually transformation of the main takes places in
// two steps.  Step1: change the name to pseudo_main and Step2:
// Function renaming to append the function name with softboundcets_
//
// Inputs:
// module: Input module with the function main
//
// Outputs:
//
// Changed module with any function named "main" is changed to
// "pseudo_main"
//
// Comments:
//
// This function is doing redundant work. We should probably use
// renameFunction to accomplish the task. The key difference is that
// transform renames it the function as either pseudo_main or
// softboundcets_pseudo_main which is subsequently renamed to
// softboundcets_pseudo_main in the first case by renameFunction
//
//org code llvm 4.0
// void Nova::TransformMain(Module& module) {
    
//   Function* main_func = module.getFunction("main");

//   // 
//   // If the program doesn't have main then don't do anything
//   //
//   if (!main_func) return;

//   Type* ret_type = main_func->getReturnType();
//   const FunctionType* fty = main_func->getFunctionType();
//   std::vector<Type*> params;

//   SmallVector<AttributeSet, 8> param_attrs_vec;
//   const AttributeSet& pal = main_func->getAttributes();

//   //
//   // Get the attributes of the return value
//   //

//   if(pal.hasAttributes(AttributeSet::ReturnIndex))
//     param_attrs_vec.push_back(AttributeSet::get(main_func->getContext(), pal.getRetAttributes()));

//   // Get the attributes of the arguments 
//   int arg_index = 1;
//   for(Function::arg_iterator i = main_func->arg_begin(), 
//         e = main_func->arg_end();
//       i != e; ++i, arg_index++) {
//     params.push_back(i->getType());

//     AttributeSet attrs = pal.getParamAttributes(arg_index);

//     if(attrs.hasAttributes(arg_index)){
//       AttrBuilder B(attrs, arg_index);
//       param_attrs_vec.push_back(AttributeSet::get(main_func->getContext(), params.size(), B));
//     }
//   }

//   FunctionType* nfty = FunctionType::get(ret_type, params, fty->isVarArg());
//   Function* new_func = NULL;

//   // create the new function 
//   new_func = Function::Create(nfty, main_func->getLinkage(), 
//                               "softboundcets_pseudo_main");

//   // set the new function attributes 
//   new_func->copyAttributesFrom(main_func);
//   new_func->setAttributes(AttributeSet::get(main_func->getContext(), param_attrs_vec));
    
//   main_func->getParent()->getFunctionList().insert(main_func->getIterator(), new_func);
//   main_func->replaceAllUsesWith(new_func);

//   // 
//   // Splice the instructions from the old function into the new
//   // function and set the arguments appropriately
//   // 
//   new_func->getBasicBlockList().splice(new_func->begin(), 
//                                        main_func->getBasicBlockList());
//   Function::arg_iterator arg_i2 = new_func->arg_begin();
//   for(Function::arg_iterator arg_i = main_func->arg_begin(), 
//         arg_e = main_func->arg_end(); 
//       arg_i != arg_e; ++arg_i) {      
//     arg_i->replaceAllUsesWith(&*arg_i2);
//     arg_i2->takeName(&*arg_i);
//     ++arg_i2;
//     arg_index++;
//   }  
//   //
//   // Remove the old function from the module
//   //
//   main_func->eraseFromParent();
// }
//modify llvm 16.0
void Nova::TransformMain(Module& module) {
  Function* main_func = module.getFunction("main");

  // If the program doesn't have main then don't do anything
  if (!main_func) return;

  Type* ret_type = main_func->getReturnType();
  const FunctionType* fty = main_func->getFunctionType();
  std::vector<Type*> params;

  // Collect parameter types
  for (auto& arg : main_func->args()) {
    params.push_back(arg.getType());
  }

  FunctionType* nfty = FunctionType::get(ret_type, params, fty->isVarArg());

  // Create the new function
  Function* new_func = Function::Create(nfty, main_func->getLinkage(), 
                                        "softboundcets_pseudo_main", module);

  // Copy linkage and other non-attribute properties
  new_func->copyAttributesFrom(main_func);

  // Rebuild AttributeList
  const AttributeList& pal = main_func->getAttributes();
  LLVMContext& ctx = main_func->getContext();
  AttributeList newAttrs;

  // Return attributes
  if (pal.hasRetAttrs()) {
    AttrBuilder retB(ctx, pal.getRetAttrs());
    newAttrs = newAttrs.addRetAttributes(ctx, retB);
  }

  // Parameter attributes
  unsigned idx = 0;
  for (auto& arg : main_func->args()) {
    if (pal.hasParamAttrs(idx)) {
      AttrBuilder paramB(ctx, pal.getParamAttrs(idx));
      newAttrs = newAttrs.addParamAttributes(ctx, idx, paramB);
    }
    ++idx;
  }

  // Apply new attributes
  new_func->setAttributes(newAttrs);

  // Replace uses of main_func with new_func
  main_func->replaceAllUsesWith(new_func);

  // Move body from old to new function
  //new_func->getBasicBlockList().splice(new_func->begin(), main_func->getBasicBlockList());
  // Move body from old to new function
  new_func->splice(new_func->begin(), main_func);

  // Update argument uses and names
  auto new_arg_iter = new_func->arg_begin();
  for (auto& old_arg : main_func->args()) {
    old_arg.replaceAllUsesWith(&*new_arg_iter);
    new_arg_iter->takeName(&old_arg);
    ++new_arg_iter;
  }

  // Remove old function
  main_func->eraseFromParent();
}

void Nova::PrepareForBoundsCheck(Module &M, ValueSet &vs) {
    TransformMain(M);
    IdentifyFuncToTrans(M);
    IdentifyInitialGlobals(M);
    AddBaseBoundGlobals(M);

    for(Module::iterator ff_begin = M.begin(), ff_end = M.end(); 
        ff_begin != ff_end; ++ff_begin){
      Function* func_ptr = dyn_cast<Function>(ff_begin);
      assert(func_ptr && "Not a function??");
      
      //
      // No instrumentation for functions introduced by us for updating
      // and retrieving the shadow space
      //
        
      if (!CheckIfFunctionOfInterest(func_ptr)) {
        continue;
      }  
      //
      // Iterating over the instructions in the function to identify IR
      // instructions in the original program In this pass, the pointers
      // in the original program are also identified
      //
        
      IdentifyOriginalInst(func_ptr);
        
      //
      // Iterate over all basic block and then each insn within a basic
      // block We make two passes over the IR for base and bound
      // propagation and one pass for dereference checks
      //
  
      GatherBaseBoundPass1(func_ptr);
      GatherBaseBoundPass2(func_ptr);
      //AddDereferenceChecks(func_ptr, vs);
    }
}

void Nova::PointerBoundaryCheck(Module &M, ValueSet &vs) {
//    Value *v;

    errs() << __func__ << " : "<< "\n";

    // Note! we almost need to fully implement Softbound here, the
    // only difference is that we selectively add check for sensitive vars.
    // we simplify the implementation as follows:
    //  1. we only support ARM32bit architecture.
    //  2. we only support spatial safty check and don't support temporal safty.
    //  3. we don't support shadow stack.
    // Attention:
    //  1. we might need to transform all functions that take pointer as params
    //     or ret_type is pointer.
    //  2. such functions can't be exported to outside module due to the transformation.
    //  3. library calls, leave it as it is.
    PrepareForBoundsCheck(M, vs);

    // identify pointer types
#if 0
    for (ValueSet::iterator it = vs.begin(), ie = vs.end();
                                              it != ie; ++it) {
        v = *it;
        errs() <<"var :" << (*v) << "\n";
        if (v->getType()->isPointerTy()) {
            errs() << v->getName() << " is pointer \n";
            if (v->getType()->getPointerElementType()->isPointerTy()) {
                errs() << v->getName() << " points to a pointer type\n";
                //PointerAccessCheck(M, v);
                ArrayAccessCheck(M, v);
            } else if (v->getType()->getPointerElementType()->isArrayTy()) {
                errs() << v->getName() << " points to a array type\n";
                ArrayAccessCheck(M, v);
            }
        }
    }
#endif

    return;
}
// ...existing code...

// Helper: 尽量找到指针的底层对象（去掉 bitcast/gep 的包装）
static Value* StripCastsAndGEPs(Value* V) {
  while (true) {
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC  = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *CI = dyn_cast<CastInst>(V)) {
      V = CI->getOperand(0);
      continue;
    }
    break;
  }
  return V;
}

// 递归回溯一个值的“来源”集合：参数/全局/alloca/调用返回/从内存读到的底层对象等
void Nova::CollectValueSources(GlobalStateRef gs, Value* V, 
                               std::set<Value*>& out,
                               std::set<Value*>& visited,
                               unsigned depth) {
  
  if (!V) return;
  if (!visited.insert(V).second) return;

  // 直接视为来源的叶子
  if (isa<GlobalVariable>(V) || isa<AllocaInst>(V)) {
    out.insert(V);
    // if(isa<GlobalVariable>(V)){
    //   errs()<<"global var source: "<<*V<<"\n";
    //   top_dependency_value.insert(V);
    //   return ;
    // }
    // if (auto* Arg = dyn_cast<Argument>(V)) {
    //   if (Function* F = Arg->getParent()) {
    //     const std::string fname = F->getName().str();
    //     if (std::find(critical_operations.begin(), critical_operations.end(), fname) != critical_operations.end()) {
    //       errs() << "critical func arg source: " << *V << " in " << fname << "\n";
    //       top_dependency_value.insert(V);
    //       return;
    //     }
    //   }
    // }
    if(auto* Alloca=dyn_cast<AllocaInst>(V)){
      if (BasicBlock* BB = Alloca->getParent()){
        if (Function* F = BB->getParent()) {
          const std::string fname = F->getName().str();
          if (std::find(critical_operations.begin(), critical_operations.end(), fname) != critical_operations.end()) {
            errs() << "critical func alloca source: " << *V << " in " << fname << "\n";
            //top_dependency_value.insert(V);
            return;
          }
        }
      }
    }
    return;
  }
  if (isa<Constant>(V)) {
    return;
  }
  if (auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->isCast()) {
      CollectValueSources(gs, CE->getOperand(0), out, visited, depth + 1);
      return;
    }
    if (CE->getOpcode() == Instruction::GetElementPtr) {
      CollectValueSources(gs, CE->getOperand(0), out, visited, depth + 1);
      return;
    }
  }
  // 指令类处理
  if (auto *I = dyn_cast<Instruction>(V)) 
  {
      // 调用返回：本身视为“来源”（跨函数难以展开）
    // 指令类处理
    // 跨函数展开：尝试将返回值溯源到被调函数的返回值成分（并映射形参与实参）
    //找到实参即可，其传入的实参会影响返回值
    if (auto *CI = dyn_cast<CallInst>(I)) {
      {
        // // Collect actual arguments as potential sources
        // CallBase* CB = CI;
        // for (unsigned ai = 0; ai < CB->arg_size(); ++ai) {
        //   Value* arg = CB->getArgOperand(ai);
        //   if (arg && !isa<Constant>(arg)) {
        //     CollectValueSources(gs, arg, out, visited, depth + 1);
        //   }
        // }

        // // Also consider the called operand (e.g., function pointer) as a source
        // Value* calleeOp = CI->getCalledOperand();
        // if (calleeOp && !isa<Function>(calleeOp)) {
        //   CollectValueSources(gs, calleeOp, out, visited, depth + 1);
        // }
        out.insert(V);
        return;
      }
    }

    // load：其来源是指针指向的底层对象；补充用 points-to 提升召回率
    if (auto *LI = dyn_cast<LoadInst>(I)) {
      Value* ptr = LI->getPointerOperand();
      Value* base = StripCastsAndGEPs(ptr);
      // out.insert(base);
      if (isa<Argument>(base) || isa<GlobalVariable>(base) || isa<AllocaInst>(base)) {
        out.insert(base);
        if(isa<GlobalVariable>(base))//如果base是一个全局变量，则将V加入到top_dependency_value中
        {
          // @g = global i32 42, align 4

          // define void @foo() {
          // entry:
          //   %x = alloca i32, align 4        ; 局部变量 x
          //   %0 = load i32, i32* @g          ; 读全局 g 的值
          //   store i32 %0, i32* %x           ; 把值存到局部变量 x
          //   ret void
          // }
          errs()<<"global var source: "<<*V<<"\n";
          CriticalDataPointRef cdp = new struct CriticalDataPoint();
          cdp->var = V;
          cdp->type = 1; // 1标识由load分支添加
          top_dependency_value.insert(cdp);
        }
      }
      // // 利用 points-to 映射补充
      if (gs && gs->pMap && gs->pMap->find(ptr) != gs->pMap->end()) {
        TupleSet* ts = (*(gs->pMap))[ptr];
        if (ts) {
          for (auto tsit = ts->begin(), tsie = ts->end(); tsit != tsie; ++tsit) {
            if (*tsit && (*tsit)->ao && (*tsit)->ao->val) {
              // out.insert((*tsit)->ao->val);
              CollectValueSources(gs, (*tsit)->ao->val, out, visited, depth + 1);
            }
          }
        }
      }
      // 递归地把指针的来源也并入（处理 PHI/Select 等组合场景）
      CollectValueSources(gs, ptr, out, visited, depth + 1);
      return;
    }
    if (auto *SI = dyn_cast<StoreInst>(I)) {

      Value* storedValue = SI->getValueOperand();   // 被存储的值
      Value* storeAddress = SI->getPointerOperand(); // 存储的地址
      // //如果storedValue是一个全局变量，则将storeAddress加入到top_dependency_value中
      // // @g = global i32 0, align 4

      // //   define void @foo() {
      // //   entry:
      // //   %p = alloca i32*, align 8       ; 局部变量 p
      // //   store i32* @g, i32** %p         ; p = &g
      // //   ret void
      // // }
      // out.insert(storedValue);
      if(isa<GlobalVariable>(storedValue)){//在后面判断其值
        errs()<<"global var source: "<<*storedValue<<"\n";
        CriticalDataPointRef cdp = new struct CriticalDataPoint();
        cdp->var = storedValue;
        cdp->type = 2; // 2标识由store分支添加
        top_dependency_value.insert(cdp);
      }
      // 递归分析存储的值的来源
      CollectValueSources(gs, storedValue, out, visited, depth + 1);
      
      // 也分析存储地址的来源
      //CollectValueSources(gs, storeAddress, out, visited, depth + 1);
      return;
    }
    // 一元/类型变换：向操作数回溯
    if (isa<BitCastInst>(I) || isa<CastInst>(I) || isa<GetElementPtrInst>(I)) {
      // out.insert(I->getOperand(0));
      CollectValueSources(gs, I->getOperand(0), out, visited, depth + 1);
      return;
    }

    // 合并类：递归所有可见操作数
    if (isa<PHINode>(I) || isa<SelectInst>(I) || I->isBinaryOp()) {
      for (unsigned oi = 0; oi < I->getNumOperands(); ++oi) {
        // out.insert(I->getOperand(oi));
        CollectValueSources(gs, I->getOperand(oi), out, visited, depth + 1);
      }
      return;
    }
  }

  // 兜底：遍历其所有操作数
  if (auto *U = dyn_cast<User>(V)) {
    errs()<<"兜底分支\n";
    errs()<<"user: "<<*U<<"\n";
    for (unsigned oi = 0; oi < U->getNumOperands(); ++oi) {
      CollectValueSources(gs, U->getOperand(oi), out, visited, depth + 1);
    }
  }
}

// 将来自“写入敏感变量”的值来源加入敏感集合与别名映射
void Nova::AugmentSensitiveSetWithDataflow(Module &M, GlobalStateRef gs,
                                           ValueSet &senVarSet) {
  std::vector<Value*> toAdd;                 // 待加入的新敏感变量
  //std::vector<std::pair<Value*,Value*>> aliasEdges; // 记录 v <- src 的边

  for (auto it = senVarSet.begin(), ie = senVarSet.end(); it != ie; ++it) {
    Value* v = *it;
    if (!v) continue;
    #if defined(SMALLTEST) ||defined(COPTER_ATTACK)
    //debug for 2025.12.26 enable for small test
    // if(isa<StoreInst>(v)||isa<LoadInst>(v)){
      errs()<<" store or load inst in senVarSet: "<<*v<<"\n";
      std::set<Value*> srcs, visited;
      CollectValueSources(gs, v, srcs, visited, 0);
      for (Value* s : srcs) {
        errs()<<"source: "<<*s<<"\n";
        if (s && !senVarSet.count(s)) {
          toAdd.push_back(s);
        }
      }
      continue;
    // }
    //end debug for 2025.12.26
    #endif
    // 直写：store <val>, <addr = v>
    for (User* U : v->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getPointerOperand() == v) {
          Value* val = SI->getValueOperand();
          std::set<Value*> srcs, visited;
          CollectValueSources(gs, val, srcs, visited, 0);
          errs()<<"sensitive var: "<<*v<<"\n";
          for (Value* s : srcs) {
            errs()<<"store source: "<<*s<<"\n";
            if (s && !senVarSet.count(s)) {
              toAdd.push_back(s);
            }
            //aliasEdges.emplace_back(v, s);
          }
        }
      }
      // 指针写：load tmp, v; store <val>, tmp
      else if (auto *LI = dyn_cast<LoadInst>(U)) {
        if (LI->getPointerOperand() == v) {
          for (User* UL : LI->users()) {
            if (auto *SI2 = dyn_cast<StoreInst>(UL)) {
              if (SI2->getPointerOperand() == LI) {
                Value* val = SI2->getValueOperand();
                std::set<Value*> srcs, visited;
                CollectValueSources(gs, val, srcs, visited, 0);
                errs()<<"sensitive var: "<<*v<<"\n";
                for (Value* s : srcs) {
                  if (s && !senVarSet.count(s)) {
                    errs()<<"store source: "<<*s<<"\n";
                    toAdd.push_back(s);
                  }
                  //aliasEdges.emplace_back(v, s);
                }
              }
            }
          }
        }
      }
    }
  }

  // 写回集合与别名映射
  for (Value* nv : toAdd) {
    senVarSet.insert(nv);
  }
 
  // for (auto &e : aliasEdges) {
  //   Value* v = e.first;
  //   Value* s = e.second;
  //   if (!v || !s) continue;
  //   sensitiveVarAliases[v].insert(s);
  // }
  #ifdef MULTIOPERATION
  //debug for 2025.12.26
  // for (auto it = gs->tMap->begin(), ie = gs->tMap->end(); it != ie; ++it) {
  //   Value* v = it->first;
  //   InstSet* taintSet = (*(gs->tMap))[v];
  //   if (senVarSet.count(v)) {
  //     llvm::errs()<<"扩展敏感变量的污点集: "<<*v<<"\n";
  //     for (InstSet::iterator isit = taintSet->begin(), isie = taintSet->end();isit != isie; ++isit)
  //       senVarSet.insert(*isit);
  //   }
  // }
  //end debug for 2025.12.26
  #endif
  // 打印调试信息
  if (!toAdd.empty()) {
    errs() << "[NOVA][DF] 新增敏感来源变量: " << toAdd.size() << "\n";
    for (auto *nv : toAdd) {
      errs() << "  + " << nv->getName() << "\n";
    }
  }

}

// 新增函数：获取敏感变量的别名信息
void Nova::GetSensitiveVariableAliases(Module &M, GlobalStateRef gs, 
                                      std::map<Value*, std::set<Value*>>& aliasMap) {
    Value *v;
    AliasMapRef aMap;
    TupleSet *ts = NULL;
    AliasObjectSet *aos = NULL;
    unsigned int offset;
    
    errs() << "\n========== 获取敏感变量别名信息 ==========\n";
    
    // 遍历所有注释的敏感变量
    for(ValueSet::iterator it = gs->senVarSet->begin(), ie = gs->senVarSet->end();
                                                        it != ie; ++it) {
        v = *it;
        assert(v != NULL);
        
        std::set<Value*> aliases;
        errs() << "\n敏感变量: " << v->getName() << " (类型: ";
        v->getType()->print(errs());
        errs() << ")\n";
        
        ts = (*(gs->pMap))[v];
        if (ts == NULL) {
            errs() << "  警告: 在指针分析映射中未找到该变量\n";
            aliasMap[v] = aliases; // 空的别名集合
            continue;
        }
        
        // 遍历该变量的所有元组
        for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                             tsit != tsie; ++tsit) {
            if (*tsit == NULL || (*tsit)->ao == NULL) {
                continue;
            }
            
            aMap = (*tsit)->ao->aliasMap;
            offset = (*tsit)->offset;
            
            if (aMap != NULL && aMap->find(offset) != aMap->end()) {
                aos = (*aMap)[offset];
                if (aos == NULL) continue;
                
                errs() << "  偏移量 " << offset << " 的别名:\n";
                for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                    aosit != aosie; ++aosit) {
                    if ((*aosit) != NULL && (*aosit)->val != NULL) {
                        Value* aliasVar = (*aosit)->val;
                        aliases.insert(aliasVar);
                        errs() << "    - " << aliasVar->getName() << " (类型: ";
                        aliasVar->getType()->print(errs());
                        errs() << ")\n";
                    }
                }
            }
            
            // 处理结构体类型变量
            if ((*tsit)->ao->type != NULL && (*tsit)->ao->type->isStructTy()) {
                errs() << "  结构体字段别名:\n";
                for (AliasMap::iterator ait = aMap->begin(), aie = aMap->end();
                                                ait != aie; ++ait) {
                    offset = ait->first;
                    aos = ait->second;
                    
                    if (aos == NULL) continue;
                    
                    errs() << "    字段偏移量 " << offset << ":\n";
                    for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                        aosit != aosie; ++aosit) {
                        if ((*aosit) != NULL && (*aosit)->val != NULL) {
                            Value* aliasVar = (*aosit)->val;
                            aliases.insert(aliasVar);
                            errs() << "      - " << aliasVar->getName() << " (类型: ";
                            aliasVar->getType()->print(errs());
                            errs() << ")\n";
                        }
                    }
                }
            }
        }
        
        // 存储该敏感变量的所有别名
        aliasMap[v] = aliases;
        errs() << "  总计别名数量: " << aliases.size() << "\n";
    }
    
    errs() << "\n========== 别名信息获取完成 ==========\n\n";
}

// 新增函数：获取单个变量的别名信息（辅助函数）
std::set<Value*> Nova::GetSingleVariableAliases(GlobalStateRef gs, Value* var) {
    std::set<Value*> aliases;
    AliasMapRef aMap;
    TupleSet *ts = NULL;
    AliasObjectSet *aos = NULL;
    unsigned int offset;
    
    if (var == NULL) return aliases;
    
    ts = (*(gs->pMap))[var];
    if (ts == NULL) return aliases;
    
    // 遍历该变量的所有元组
    for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                         tsit != tsie; ++tsit) {
        if (*tsit == NULL || (*tsit)->ao == NULL) {
            continue;
        }
        
        aMap = (*tsit)->ao->aliasMap;
        offset = (*tsit)->offset;
        
        if (aMap != NULL && aMap->find(offset) != aMap->end()) {
            aos = (*aMap)[offset];
            if (aos == NULL) continue;
            
            for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                aosit != aosie; ++aosit) {
                if ((*aosit) != NULL && (*aosit)->val != NULL) {
                    Value *s = (*aosit)->val;
                    // 与DefUseCheck保持一致：检查是否为跨函数别名
                    if (((s->getType()->isPointerTy() || var->getType()->isPointerTy()) && 
                         (isa<AllocaInst>(s) || isa<AllocaInst>(var))) || 
                        !isCrossFunctionValue(var, s)) {
                        aliases.insert(s);
                    }
                }
            }
        }
        
        // 处理结构体类型变量
        if ((*tsit)->ao->type != NULL && (*tsit)->ao->type->isStructTy()) {
            for (AliasMap::iterator ait = aMap->begin(), aie = aMap->end();
                                            ait != aie; ++ait) {
                aos = ait->second;
                if (aos == NULL) continue;
                
                for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                    aosit != aosie; ++aosit) {
                    if ((*aosit) != NULL && (*aosit)->val != NULL) {
                        Value *s = (*aosit)->val;
                        // 与DefUseCheck保持一致：检查是否为跨函数别名
                        if (((s->getType()->isPointerTy() || var->getType()->isPointerTy()) && 
                             (isa<AllocaInst>(s) || isa<AllocaInst>(var))) || 
                            !isCrossFunctionValue(var, s)) {
                            aliases.insert(s);
                        }
                    }
                }
            }
        }
    }
    
    return aliases;
}
// 辅助函数：判断两个 Value 是否跨函数
bool Nova::isCrossFunctionValue(Value* v1, Value* v2) {
  if (!v1 || !v2) return false;

  Function* f1 = getValueFunction(v1);
  Function* f2 = getValueFunction(v2);

  // 如果任一值不属于任何函数（如全局变量），认为不跨函数
  if (!f1 || !f2) return false;

  return f1 != f2;
}

// 辅助函数：获取 Value 所属的函数
Function* Nova::getValueFunction(Value* v) {
  if (!v) return nullptr;

  // 全局变量
  if (isa<GlobalVariable>(v)) return nullptr;

  // 函数参数
  if (auto* Arg = dyn_cast<Argument>(v)) {
    return Arg->getParent();
  }

  // 指令
  if (auto* Inst = dyn_cast<Instruction>(v)) {
    return Inst->getFunction();
  }

  return nullptr;
}
// 辅助函数：检查变量集合是否有store或load指令
bool Nova::HasStoreOrLoadInSet(const std::set<Value*>& varSet) {
    for (Value* var : varSet) {
        if (!var) continue;
        
        // 检查该变量的所有使用
        for (User* U : var->users()) {
            if (Instruction* Inst = dyn_cast<Instruction>(U)) {
                // 检查是否是store指令
                if (isa<StoreInst>(Inst)) {
                    StoreInst* SI = cast<StoreInst>(Inst);
                    // 检查var是作为地址操作数还是值操作数
                    if (SI->getPointerOperand() == var || SI->getValueOperand() == var) {
                        return true;
                    }
                }
                // 检查是否是load指令
                else if (isa<LoadInst>(Inst)) {
                    LoadInst* LI = cast<LoadInst>(Inst);
                    if (LI->getPointerOperand() == var) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
// enforce def-use check based on analysis result stored in gs
void Nova::DefUseCheck(Module &M, GlobalStateRef gs) {
    ValueSet senVarSet;
    Value *v;
    AliasMapRef aliasMap;
    TupleSet *ts = NULL;
    AliasObjectSet *aos = NULL;
    unsigned int offset;
    
    // 新增：获取敏感变量别名映射
    std::map<Value*, std::set<Value*>> sensitiveVarAliases;
    // step1: get the scope of sensitive variables
    // start from annotated vars, get all its alias vars
    // and get it's dependecies util reach fixed point
    #ifndef MULTIOPERATION
    while(true)//enable for ardupilot
    {
    #endif
      for(ValueSet::iterator it = gs->senVarSet->begin(), ie = gs->senVarSet->end();
                                                          it != ie; ++it) 
      {
        v = *it;
        if (v) senVarSet.insert(v);
      }
      unsigned int old_size = senVarSet.size();
    
      // ========== 第一阶段：收集每个敏感变量的指针别名 ==========
      ValueSet newAliases;
      for(ValueSet::iterator it = senVarSet.begin(), ie = senVarSet.end();
                                                          it != ie; ++it) 
      {//该循环主要是处理由程序员注释的敏感变量
          v = *it;
          if (!v) continue;
          std::set<Value*> aliases;
          // assert(v != NULL);
          // errs() << "insert1!!!!!!!!!!!!!!!!!!!!!"<<*v << "\n";
          // senVarSet.insert(v); // Already in set

          ts = (*(gs->pMap))[v];//ts 是一个AliasObjectTuple的集合，实际上是TupleSet

          //assert(ts != NULL);
          if (ts == NULL) {
              if (v) errs() << "NOTE: can't find tupleSet in pointsToMap for v = " << *v;
              continue;
          }
        //遍历tuple_set,根据里面的AliasObjectTuple来找到每个alias的aliasMap，再根据aliasmap中的值找到aliasobjectset，该aliasObjectset就是该alia对应的别名集合
        //然后遍历aliasobjectset集合，将里面的aliasobject加入到senvarset中
          for (TupleSet::iterator tsit = ts->begin(), tsie = ts->end();
                                              tsit != tsie; ++tsit) 
          {//遍历ts（TupleSet），tsit是一个AliasObjectTuple
              if (*tsit == NULL || (*tsit)->ao == NULL) 
              {
                  errs() << "*tsit == NULL || *tsit->ao == NULL, skip!" << "\n";
                  continue;
              }

              aliasMap = (*tsit)->ao->aliasMap;//aliasMap是一个AliasMapRef
              offset = (*tsit)->offset;//offset是一个unsigned int，表示偏移量
              if (aliasMap != NULL && aliasMap->find(offset) != aliasMap->end()) 
              {
                  aos = (*aliasMap)[offset];//aos是一个AliasObjectSet，表示该偏移量对应的别名对象集合
                  if (aos == NULL) {
                      errs() << "aos == NULL!\n";
                      continue;
                  }

                  for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();
                                                      aosit != aosie; ++aosit) {//aosit是一个AliasObject
                      if ((*aosit) == NULL) {
                          errs() << "aosit == NULL!\n";
                          continue;
                      }
                      Value * s= (*aosit)->val;
                      // errs() << "insert2!!!!!!!!!!!!!!!!!!!!!"<<*((*aosit)->val) << "\n";
                      newAliases.insert((*aosit)->val);
                        if (((s->getType()->isPointerTy()||v->getType()->isPointerTy()) && (isa<AllocaInst>(s)||isa<AllocaInst>(v))) || !isCrossFunctionValue(v, s))
                        {
                          aliases.insert((*aosit)->val); // 收集别名
                        }
                          
                  }
              } 
              else 
              {
                  //errs() << "location object ?" << "\n";
              }
              //处理结构体类型变量
              if ((*tsit)->ao->type != NULL && (*tsit)->ao->type->isStructTy()) 
              {//处理结构体类型的敏感变量
                  // print struct field aliasMap
                  for (AliasMap::iterator ait = aliasMap->begin(), aie = aliasMap->end();
                                                  ait != aie; ++ait) 
                  {
                      offset = ait->first;//取aliasmap的key
                      aos = ait->second;//取aliasmap的value，也就是该结构体中每个filed对应的aliasobjectset

                      if (aos == NULL) 
                      {
                          errs() << "aos == NULL!\n";
                          continue;
                      }
                      //遍历每个filed对应的aliasobjectset，找到AliasObject
                      for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();//aos是一个AliasObjectset，aosit是一个aliasobject
                                                          aosit != aosie; ++aosit) 
                      {
                          if ((*aosit) == NULL) {
                              errs() << "aosit == NULL!\n";
                              continue;
                          }
                          // errs() << "insert3!!!!!!!!!!!!!!!!!!!!!"<<*((*aosit)->val) << "\n";
                          newAliases.insert((*aosit)->val);
                          Value * s= (*aosit)->val;
                         if (((s->getType()->isPointerTy()||v->getType()->isPointerTy()) && (isa<AllocaInst>(s)||isa<AllocaInst>(v))) || !isCrossFunctionValue(v, s))
                          {
                            aliases.insert((*aosit)->val); // 收集别名
                          }
                      }
                  }
              }
          }
          sensitiveVarAliases[v]=aliases;// 将敏感变量及其别名存入映射表
      }
      for(auto *val : newAliases) {
          senVarSet.insert(val);
      }

      // scan over all vars, put those whose alias is in the senVar set into senVar, too
      for (PointsToMap::iterator it = gs->pMap->begin(), ie = gs->pMap->end();//it是一个映射表，其组成为Value*, TupleSet *
                                                          it != ie; ++it) { 
          // skip sensitive var, they are already in senVarSet.
          if (senVarSet.count(it->first) != 0)//it->first是一个Value*，表示变量,该语句表示该变量已经在senVarset中了
              continue;
          
          //errs() << it->first->getName() << " : " << "\n";

          if (it->second == NULL) {
              errs() << "it->second == NULL\n";
              continue;
          }

          for (TupleSet::iterator tsit = it->second->begin(), tsie = it->second->end();//tsit是一个 TupleSet的迭代器，表示一个AliasObjectTuple
                                                      tsit != tsie; ++tsit) {
              if ((*tsit) == NULL || (*tsit)->ao == NULL) {
                  errs() << "(*tsit)->ao == NULL!\n";
                  continue;
              }

              //errs() << "(" << (*tsit)->offset << ", " << (*tsit)->ao->val->getName() << ")" << "\n";
              aliasMap = (*tsit)->ao->aliasMap;// aliasMap是一个AliasMapRef，表示该AliasObjectTuple对应的别名映射表
              offset = (*tsit)->offset;
              if (aliasMap != NULL && aliasMap->find(offset) != aliasMap->end()) {
                  aos = (*aliasMap)[offset];////aos是一个AliasObjectSet，表示该偏移量对应的别名对象集合
                  if (aos == NULL) {
                          errs() << "aos == NULL"<<"\n";
                          continue;
                  }
                  //errs() << "alias object set: ";
                  for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();//遍历aos（Aliasobjectset），判断其中是否有被标记为敏感变量的别名
                                                      aosit != aosie; ++aosit) {
                      // errs() << (*aosit)->val->getName() << ",";
                      if (senVarSet.count((*aosit)->val) != 0) {//有别名被标记为了敏感变量
                          // var contains sensitive var as its alias object, so add it to senVarSet
                          errs() << " added to senVarSet1!!\n";
                          Value * s= (*aosit)->val;
                          senVarSet.insert(it->first);
                          if (((s->getType()->isPointerTy()||v->getType()->isPointerTy()) && (isa<AllocaInst>(s)||isa<AllocaInst>(v))) || !isCrossFunctionValue(v, s))
                          {
                            if (senVarSet.count(it->first) == 0) {
                                if(sensitiveVarAliases.count(it->first)==0)//如果敏感变量别名映射表中没有该变量，则添加
                                {
                                  sensitiveVarAliases[it->first] = std::set<Value*>();
                                }
                                sensitiveVarAliases[it->first].insert((*aosit)->val);//将该别名加入到敏感变量别名映射表中
                                //senVarSet.insert((*aosit)->val); 
                              }
                          }

                      }
                  }
                  //errs() << "\n";
              } else {
                  //errs() << "location object ?" << "\n";
              }
              //处理结构体变量，如果一个结构体中的变量是敏感变量，则其该结构体也为敏感变量
              if ((*tsit)->ao->type != NULL && (*tsit)->ao->type->isStructTy()) {
                  // print struct field aliasMap
                  //errs() << "struct internal alias map: \n";
                  for (AliasMap::iterator ait = aliasMap->begin(), aie = aliasMap->end();
                                                  ait != aie; ++ait) {
                      offset = ait->first;
                      aos = ait->second;
                      //errs() << "alias object set at offset " << offset << " : ";
                      for (AliasObjectSet::iterator aosit = aos->begin(), aosie = aos->end();//aosit是一个AliasObject
                                                          aosit != aosie; ++aosit) {
                          //errs() << (*aosit)->val->getName() << ",";
                          if (senVarSet.count((*aosit)->val) != 0) {
                              // var contains sensitive var as its alias object, so add it to senVarSet
                              Value * s= (*aosit)->val;
                              errs() << " added to senVarSet2!!\n";
                              if (senVarSet.count(it->first) == 0) {
                                  senVarSet.insert(it->first);
                                  if(sensitiveVarAliases.count(it->first)==0)//如果敏感变量别名映射表中没有该变量，则添加
                                  {
                                    sensitiveVarAliases[it->first] = std::set<Value*>();
                                  }
                                  if (((s->getType()->isPointerTy()||v->getType()->isPointerTy()) && (isa<AllocaInst>(s)||isa<AllocaInst>(v))) || !isCrossFunctionValue(v, s))
                                  {
                                    sensitiveVarAliases[it->first].insert((*aosit)->val);//将该别名加入到敏感变量别名映射表中
                                    //senVarSet.insert((*aosit)->val);
                                  }

                              }

                          }
                      }
                      //errs() << "\n";
                  }
              }
          }
          //errs() << "\n";
      }
      //打印senVarSet
      errs() << "\n当前敏感变量集合0: \n";
      for (Value* sv : senVarSet) {
        if (sv) errs() << "  - " << *sv << "\n";
      } 

      // ========== 新增:关联指向同一全局变量的GEP指令 ==========
      std::map<Value*, std::set<Value*>> globalVarUsers; // 全局变量 -> 访问它的GEP指令集合
      
      // 遍历所有函数,找到访问全局变量的GEP指令
      for (Module::iterator F = M.begin(), FE = M.end(); F != FE; ++F) {
          for (Function::iterator BB = F->begin(), BBE = F->end(); BB != BBE; ++BB) {
              for (BasicBlock::iterator I = BB->begin(), IE = BB->end(); I != IE; ++I) {
                  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I)) {
                      Value *ptrOperand = GEP->getPointerOperand();
                      
                      // 如果是全局变量
                      if (isa<GlobalVariable>(ptrOperand)) {
                          globalVarUsers[ptrOperand].insert(GEP);
                      }
                  }
                  // 也处理直接load/store全局变量的情况
                  else if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
                      Value *ptrOperand = LI->getPointerOperand();
                      if (isa<GlobalVariable>(ptrOperand)) {
                          globalVarUsers[ptrOperand].insert(LI);
                      }
                  }
                  else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
                      Value *ptrOperand = SI->getPointerOperand();
                      if (isa<GlobalVariable>(ptrOperand)) {
                          globalVarUsers[ptrOperand].insert(SI);
                      }
                  }
              }
          }
      }
      
      // 如果某个GEP/Load/Store在敏感变量集合中,将同一全局变量的其他访问也加入
      for (auto &entry : globalVarUsers) {
          Value *globalVar = entry.first;
          std::set<Value*> &users = entry.second;
          
          bool hasSensitive = false;
          for (Value *user : users) {
              if (senVarSet.count(user) != 0) {
                  hasSensitive = true;
                  break;
              }
          }
          
          if (hasSensitive) {
              errs() << "发现敏感全局变量: " << *globalVar << "\n";
              // 将全局变量本身标记为敏感
              senVarSet.insert(globalVar);
              
              // 将所有访问该全局变量的指令都标记为敏感
              for (Value *user : users) {
                  if (senVarSet.count(user) == 0) {
                      errs() << "  关联访问指令: " << *user << "\n";
                      senVarSet.insert(user);
                      
                      // 更新别名映射表
                      if (sensitiveVarAliases.count(user) == 0) {
                          sensitiveVarAliases[user] = std::set<Value*>();
                      }
                      sensitiveVarAliases[user].insert(globalVar);
                      
                      // 将其他访问同一全局变量的指令也作为别名
                      for (Value *otherUser : users) {
                          if (otherUser != user) {
                              sensitiveVarAliases[user].insert(otherUser);
                          }
                      }
                  }
              }
          }
      }
      
      errs() << "\n=== 全局变量别名关联完成 ===\n";
      
      //use back-ward edge analysis to get dependecy
      AugmentSensitiveSetWithDataflow(M, gs, senVarSet);
      errs() << "\n当前敏感变量集合1: \n";
      for (Value* sv : senVarSet) {
        if (sv) errs() << "  - " << *sv << "\n";
      } 
      //use forward analysis to get propolation
      //extendSenVarSet(M, gs, senVarSet);
      errs() << "\n当前敏感变量集合2: \n";
      for (Value* sv : senVarSet) {
        if (sv) errs() << "  - " << *sv << "\n";
      } 
    #ifdef MULTIOPERATION
    //重建敏感变量别名映射表
      for(Value* sv : senVarSet)
      {
        if(sensitiveVarAliases.count(sv)==0)
        {
          std::set<Value*> aliases = GetSingleVariableAliases(gs, sv);
          sensitiveVarAliases[sv] = aliases;
        }
      }
    #endif
   #ifndef MULTIOPERATION
      unsigned int new_size = senVarSet.size();
      if(old_size == new_size)//reach fixed point
      {
        break;
      }
    }//end while
    #endif
    // ========== 第二阶段：合并敏感变量别名集合 ==========

    // step2: merge sensitive variables with their aliases,if value A has aliasSet B,and C has aliasSet D,if B and D both has a value e,then merege set B and D.
    std::map<int, std::set<Value*>> aliasSetIDMap;
    std::map<Value*, int> varToSetID; // 记录每个变量对应的集合ID
    int nextSetID = 0;
    // 使用并查集来处理集合合并
    std::map<int, int> parent; // 并查集的父节点映射
    std::function<int(int)> findRoot = [&](int x) -> int {
        if (parent.find(x) == parent.end()) {
            parent[x] = x;
            return x;
        }
        if (parent[x] != x) {
            parent[x] = findRoot(parent[x]); // 路径压缩
        }
        return parent[x];
    };

    auto unionSets = [&](int x, int y) {
        int rootX = findRoot(x);
        int rootY = findRoot(y);
        if (rootX != rootY) {
            parent[rootY] = rootX; // 将y的根节点指向x的根节点
        }
    };
    for (ValueSet::iterator it = senVarSet.begin(), ie = senVarSet.end();it != ie; ++it) 
    {
      v = *it;
  
      // 如果变量还没有分配集合ID，分配一个新的
      if (varToSetID.find(v) == varToSetID.end()) {
          varToSetID[v] = nextSetID;
          aliasSetIDMap[nextSetID].insert(v);
          
          // 将该变量的所有别名也加入到同一个集合中
          if (sensitiveVarAliases.find(v) != sensitiveVarAliases.end()) {
              for (Value* alias : sensitiveVarAliases[v]) {
                  aliasSetIDMap[nextSetID].insert(alias);
                  varToSetID[alias] = nextSetID;
              }
          }
          
          errs() << "变量 " << v->getName() << " 分配到集合 " << nextSetID 
                << "，包含 " << aliasSetIDMap[nextSetID].size() << " 个元素\n";
          nextSetID++;
      }

    }
    // 第二步：检查集合之间是否有公共元素，如果有则合并
    errs() << "\n检查并合并有公共元素的集合...\n";

    // 为了检测公共元素，创建一个从值到集合ID列表的映射
    std::map<Value*, std::set<int>> valueToSets;

    for (auto& pair : aliasSetIDMap) {
        int setID = pair.first;
        std::set<Value*>& aliasSet = pair.second;
        
        for (Value* val : aliasSet) {
            valueToSets[val].insert(setID);
        }
    }
    // 找到需要合并的集合
    for (auto& pair : valueToSets) {
        Value* commonValue = pair.first;
        std::set<int>& setIDs = pair.second;
        
        if (setIDs.size() > 1) {
            errs() << "发现公共元素 " << commonValue->getName() 
                  << " 在 " << setIDs.size() << " 个集合中\n";
            
            // 合并所有包含这个公共元素的集合
            auto setIt = setIDs.begin();
            int firstSetID = *setIt;
            ++setIt;
            
            while (setIt != setIDs.end()) {
                int currentSetID = *setIt;
                unionSets(firstSetID, currentSetID);
                errs() << "  合并集合 " << firstSetID << " 和 " << currentSetID << "\n";
                ++setIt;
            }
        }
    }
    // 第三步：根据并查集结果重新组织最终的集合
    std::map<int, std::set<Value*>> mergedAliasSetMap;

    for (auto& pair : aliasSetIDMap) {
        int originalSetID = pair.first;
        int rootSetID = findRoot(originalSetID);
        std::set<Value*>& aliasSet = pair.second;
        
        // 将所有元素加入到根集合中
        for (Value* val : aliasSet) {
            mergedAliasSetMap[rootSetID].insert(val);
        }
    }
    
    // 第四步：过滤掉没有store/load的集合，并重新编号
    errs() << "\n过滤并重新编号集合...\n";
    std::map<int, std::set<Value*>> finalAliasSetIDMap;
    int finalID = 0;
    
    for (auto& pair : mergedAliasSetMap) {
        int tempID = pair.first;
        std::set<Value*>& varSet = pair.second;
        
        if (varSet.empty()) {
            errs() << "  临时集合 " << tempID << " 为空，跳过\n";
            continue;
        }
        
        // 检查集合中是否有store或load指令
        if (!HasStoreOrLoadInSet(varSet)) {
            errs() << "  临时集合 " << tempID << " 没有store/load操作，跳过分配ID\n";
            errs() << "    包含变量: ";
            for (Value* v : varSet) {
                if (v->hasName()) {
                    errs() << v->getName() << " ";
                } else {
                    errs() << "<unnamed> ";
                }
            }
            errs() << "\n";
            continue;
        }
        
        // 分配最终ID
        finalAliasSetIDMap[finalID] = varSet;
        
        errs() << "  临时集合 " << tempID << " -> 最终集合 " << finalID 
               << "，包含 " << varSet.size() << " 个变量\n";
        errs() << "    变量列表: ";
        for (Value* v : varSet) {
            if (v->hasName()) {
                errs() << v->getName() << " ";
            } else {
                errs() << "<unnamed> ";
            }
        }
        errs() << "\n";
        
        finalID++;
    }
    
    aliasSetIDMap = std::move(finalAliasSetIDMap);
    errs() << "\n最终分配了 " << aliasSetIDMap.size() << " 个有效集合ID\n";
  
    // step3: for all sensitive variables
    //          do def/use check
    errs() << "List PointsToMap vars:" <<"\n";
#ifdef INSTRUMENT_ALL
    for (PointsToMap::iterator it = gs->pMap->begin(), ie = gs->pMap->end();
                                                         it != ie; ++it) { 
        errs() <<"PointsToMap element:"<<(it->first)->getName() <<"\n";
        RecordDefineEvent(M, (it->first));
        CheckUseEvent(M, (it->first));
    }
#elif defined(INSTRUMENT_HALF)
    int i = 0;
    for (PointsToMap::iterator it = gs->pMap->begin(), ie = gs->pMap->end();
                                                         it != ie; ++it) { 
        if (i++ % 2 == 0)
            continue;
        errs() <<(it->first)->getName() <<"\n";
        RecordDefineEvent(M, (it->first));
        CheckUseEvent(M, (it->first));
    }
#else
    errs() << "senVarSet : \n";
    //modify here
    // for (ValueSet::iterator it = senVarSet.begin(), ie = senVarSet.end();
    //                                 it != ie; ++it) {
        
    //     errs() <<(*it)->getName() <<"\n\n";
    //     RecordDefineEvent(M, *it);
    //     CheckUseEvent(M, *it);
    // }
    //对top_dependency_value进行去重，根据var指针
      llvm::SetVector<CriticalDataPoint*> uniqueCDPs;
      std::set<Value*> seenVars;

      for (auto cdp : top_dependency_value) {
          if (seenVars.insert(cdp->var).second) { // 只插入第一次出现的 var
              uniqueCDPs.insert(cdp);
          }
      }
      top_dependency_value = uniqueCDPs; // 直接赋值，类型一致
      SetVector<CriticalDataPoint*>idxList;
      //扩展top_dependency_value,var不是一个指向一个结构体的指针，而是一个结构体的局部变量，通过getelementptr来获取该变量中的某个字段，将其字段也加入top_dependency_value
      for(auto cdp:top_dependency_value)
      {
        Value* var=cdp->var;
        //找到var的user
        for(User* Uov :var->users())
        {
          //判断是否是gep指令
          if(Instruction *Inst = dyn_cast<Instruction>(Uov))
          {
            if(auto*GEP=dyn_cast<GetElementPtrInst>(Inst))
            {
              Value* ptrOp=GEP->getPointerOperand();
              if(ptrOp==var)
              {
                Type* resultType = GEP->getResultElementType();
                if(resultType->isIntegerTy() || resultType->isFloatTy())
                {
                    CriticalDataPoint* newcdp=new CriticalDataPoint();
                    newcdp->var=GEP;
                    newcdp->type=0;//表示该变量是通过gep获取的
                    idxList.insert(newcdp);
                }
              }
            }
          }
        }
      }   
      for(auto cdp:idxList)
      {
        top_dependency_value.insert(cdp);
      }
      for(auto*v :top_dependency_value){
        errs() << "top_dependency_value: "<<*v->var<<"\n";
      }
      if(NovaDynamicCollect==CollectValue){
        // insert dynamic collect value

        for(auto*v :top_dependency_value)
        {
          // RecordDefineEvent(M, v,-1);
          // CheckUseEvent(M, v,-1);
          instrument_to_collect_data(M,v,-1);
          
        }
        
      } 
      else if(NovaDynamicCollect==DefUseCheck1)
      {
        SetVector<Value *> non_sensitive_var;
        // for(auto critical_operation:critical_operations)
        // {
        //   Function* F=M.getFunction(critical_operation);
        //   if(!F) continue;
        //   get_non_sensitive_var(M,*F,non_sensitive_var,senVarSet);
        // }
        // for(auto v:non_sensitive_var)
        // {
        //   errs() << "non_sensitive_var: "<<*v<<"\n";
        //   CheckNoSenVarDef(M,v);
        // }

        
        for(auto & pair : aliasSetIDMap) 
        {
          int setID = pair.first;
          std::set<Value*>& aliasSet = pair.second;

          errs() << "集合 " << setID << " 包含的变量:\n";
          for (Value* var : aliasSet) {
            if(senVarSet.count(var) == 0) {
              errs()<<"var not in senVarSet, skip"<<*var<<"\n";
              continue; // 如果变量不在敏感变量集合中，则跳过
            }
            

            
              errs() << "  - ";
              if (var->hasName()) {
                  errs() << var->getName()<<"\n";
              } else {
                  errs() << "<unnamed>: ";
                  var->print(errs());  // 打印出具体的IR形式
                  errs()<<"\n";
              }
              //判断该var是不是在top_dependency_value中
              bool inTopDependency=false;
              CriticalDataPointRef cdp_found=nullptr;
              for(auto cdp:top_dependency_value)
              {
                if(cdp->var==var)
                {
                  inTopDependency=true;
                  cdp_found=cdp;
                  break;
                }
              }
              if(!inTopDependency)
              {
                RecordDefineEvent(M, var,setID);
                CheckUseEvent(M, var,setID);
              }
              else{
                RecordDefineEvent(M, var,setID);
                CheckUseEvent(M, var,setID);
                instrument_to_collect_data(M,cdp_found,setID);
              }
          }
        }
      }

#endif

    return;
}
void Nova::collect_argument(Module &M)
{
  for(auto critical_operation:critical_operations)
  {
    Function* F=M.getFunction(critical_operation);
    if(!F) continue;
    BasicBlock &Entry = F->getEntryBlock();
    IRBuilder<> B(&*Entry.getFirstInsertionPt());
    Value *thisPtr = F->getArg(0); // %0，就是AC_PosControl* this
    expandStructPtr(M, B, thisPtr, -1);
  }
}
void Nova::get_non_sensitive_var(Module &M,Function& F,SetVector<Value *> &non_sensitive_var,SetVector<Value *> &senVarSet)
{
  for (auto &BB : F) {
    for (auto &I : BB) {
      // 检查是否为Alloca指令
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        Value* var=AI;
        if(senVarSet.count(var)==0)//该变量不是敏感变量
        {
          //判断是否是一个二级指针
          Type *elemTy = var->getType()->getPointerElementType();
          if ((var->getType()->isPointerTy()&&elemTy->isPointerTy()&&elemTy->getPointerElementType()->isStructTy())) //如果 op 是一个指针，并且它指向的类型是结构体
          {
            non_sensitive_var.insert(var);
          }
        }
      }
      //检查是否是全局变量
      else if(GlobalVariable* GV=dyn_cast<GlobalVariable>(&I))
      {
        Value* var=GV;
        if(senVarSet.count(var)==0)//该变量不是敏感变量
        {
          //判断是否是一个二级指针
          Type *elemTy = var->getType()->getPointerElementType();
          if ((var->getType()->isPointerTy()&&elemTy->isPointerTy()&&elemTy->getPointerElementType()->isStructTy())) //如果 op 是一个指针，并且它指向的类型是结构体
          {
            non_sensitive_var.insert(var);
          }
        }
      }
      //如果是call指令，则递归进去
      else if(CallInst* CI=dyn_cast<CallInst>(&I))
      {
        Function* callee=CI->getCalledFunction();
        if(callee&&!callee->isDeclaration()&&!callee->isIntrinsic())
        {
          get_non_sensitive_var(M,*callee,non_sensitive_var,senVarSet);
        }
      }
    }
  }
}
//Var此时是top_dependency_value中的一个变量
//下述该函数需要在var变量第一次被def的地方插入对该变量的收集代码
//注意，如果手动注释中Var不是一个全局变量，则Var不可能是一个全局变量
void Nova::instrument_to_collect_data(Module &M,CriticalDataPoint *cdp,int setID)
{
  llvm::errs() << "Instrument to collect data for var: "  << "\n";
  Value* Var=cdp->var; 
  Value *op=Var;
  int type=cdp->type;
  llvm::errs() << "Instrument to collect data for var: " <<*op << "\n";
  //工具函数：判断是否在关键函数中
  // auto isCriticalFunction=[&](Function*F)->bool
  // {
  //   if(!F) return false;
  //   const std::string fname=F->getName().str();
  //   return std::find(critical_operations.begin(),critical_operations.end(),fname)!=critical_operations.end();
  // }
  if (!op->getType()->isPointerTy())
  {
    return;
  }
  Type *elemTy = op->getType()->getPointerElementType();
  llvm::errs() << "Instrument to collect data for var: " <<*op << "\n";
  if ((op->getType()->isPointerTy() && elemTy->isStructTy())) //如果 op 是一个指针，并且它指向的类型是结构体
  {
    errs()<<"find struct type var:"<<*op<<"\n";
    // 处理结构体变量，
    //Var可能是两种情况下收集进top_dependency_value的
    //第一种情况：var是一个alloca的结果，也就意味着是一个局部变量，对其的修改就是store value，*var,手动注释的
    //第二种情况：var是由collectvaluesources中LoadInst分支添加进来的，也就意味着var是全局变量的值
    //第三种情况，var是由collectvaluesources中store分支添加进来的，意味着var是全局变量的的一个指针
    //如果var是一个结构体，则store时，其一定是一个指针，故而直接展开就可以，但需要对第二种情况特殊处理
    IRBuilder<> B(M.getContext());
    if(type==0)//对于手动注释的变量，var是一个alloca，需要找到其第一次定义的地方，这种情况一般不可能出现，因为op实际上是一个结构体变量，而不是一个指针，这种情况下对op的修改一般为一个一个赋值其内部字段
    {
      for(User* Uov :op->users())
      {
        if(Instruction *Inst = dyn_cast<Instruction>(Uov))
        {
          if(auto*SI=dyn_cast<StoreInst>(Inst))
          {
            Value* ptrOp=SI->getPointerOperand();
            Value* val=SI->getValueOperand();
            B.SetInsertPoint(Inst->getNextNode());
            expandStructPtr(M,B,op,setID);
          }
        }
      }
    }
    else if (type==1)//其是一个load指令.load的地址是一个全局变量
    {
      if(Instruction *Inst = dyn_cast<Instruction>(op))
      {
        if(auto *LI=dyn_cast<LoadInst>(Inst))
        {
          for(User*UoL :LI->users())
          {
            if(Instruction *Inst1 = dyn_cast<Instruction>(UoL))
            {
              if(auto*SI=dyn_cast<StoreInst>(Inst1))
              {
                Value* ptrOp=SI->getPointerOperand();
                Value* val=SI->getValueOperand();
                if(val==LI&&isa<GlobalVariable>(val))//对于第二种情况，var是由load分支添加进来的，读取了全局变量中的一个指针，然后将该指针存储进了ptrOp
                {
                  B.SetInsertPoint(Inst1->getNextNode());
                  expandStructPtr(M,B,op,setID);
                }
              }
            }
          }
        }
      }
    }
    else if(type==2)
    { //对于第三种情况，var实际上是一个store指令的目标地址,但存放的可能是一个全局变量
      for(User* Uov :op->users())
      {
        if(Instruction *Inst = dyn_cast<Instruction>(Uov))
        {
          if(auto*SI=dyn_cast<StoreInst>(Inst))
          {
            Value* ptrOp=SI->getPointerOperand();
            Value* val=SI->getValueOperand();
            if(ptrOp==op&&isa<GlobalVariable>(val))//对于第三种情况，val是一个全局变量指针
            {
                B.SetInsertPoint(Inst->getNextNode());
                expandStructPtr(M,B,op,setID);
            }
          }
        }
      }
    }
  } 
  else if((elemTy->isIntegerTy() || elemTy->isFloatTy())) //var是一个基础类型的变量
  {
    for(User* Uov :op->users())
    {
      if(Instruction *Inst = dyn_cast<Instruction>(Uov))
      {
        if(auto*SI=dyn_cast<StoreInst>(Inst))
        {
          Value* ptrOp=SI->getPointerOperand();
          Value* val=SI->getValueOperand();
          if(ptrOp==op)//修改局部变量的值
          {
              errs()<<"find store instruction for basic type var:"<<*Inst<<"\n";
              IRBuilder<> B(Inst->getNextNode());
              if(NovaDynamicCollect==CollectValue)
              {
                insertInstrumentation_to_collect(M,B,val,CriticalVarIndex);
              }
              else if(NovaDynamicCollect==DefUseCheck1)
              {
                // 先初始化为 0
                  float maxvalue = 0.0f;
                  float minvalue = 0.0f;

                  // 查找是否存在该 Index
                  auto it = m_cdp_value_range.find(CriticalVarIndex);

                  // 如果找到了 (it 不等于 end)，则取实际值
                  if (it != m_cdp_value_range.end()) {
                      // it->second 代表 map 中的值 (即那个 tuple/pair)
                      minvalue = std::get<0>(it->second);
                      maxvalue = std::get<1>(it->second);
                  }
                //读取m_cdp_value_range
                // float maxvalue = std::get<1>(m_cdp_value_range[CriticalVarIndex]);
                // float minvalue = std::get<0>(m_cdp_value_range[CriticalVarIndex]);
                insertInstrumentation_to_check(M,B,op,val,setID,maxvalue,minvalue);
              }
              //insertInstrumentation_to_collect(M,B,op,CriticalVarIndex);
              CriticalVarIndex++;
          }
        }
      }
    }
  }
  else if (elemTy->isPointerTy() && elemTy->getPointerElementType()->isStructTy()) //多级指针
  {
    errs()<<"find multi-level struct pointer type var:"<<*op<<"\n";
    IRBuilder<> B(M.getContext());
    if(type==0)//对于手动注释的变量，var是一个alloca，需要找到其第一次定义的地方
    {
      for(User* Uov :op->users())
      {
        if(Instruction *Inst = dyn_cast<Instruction>(Uov))
        {
          if(auto*SI=dyn_cast<StoreInst>(Inst))
          {
            Value* ptrOp=SI->getPointerOperand();
            Value* val=SI->getValueOperand();
            if(ptrOp==op)//对于第三种情况，val是一个全局变量指针
            {
                B.SetInsertPoint(Inst->getNextNode());
                //获取op指向的结构体
                //Value *structPtr = B.CreatePointerCast(op, elemTy->getPointerTo(), "structPtr");
                Value *loaded = B.CreateLoad(elemTy, op, "ld_struct_ptr");
                // errs()<<"structPtr:"<<*structPtr<<"\n";
                // errs()<<"to instrument instruction is :"<<*Inst<<"\n";
                // if(!structPtr->getType()->isPointerTy()||!structPtr->getType()->getPointerElementType()->isStructTy())
                // {
                //   errs()<<"Error: structPtr is not a struct pointer type:"<<*structPtr<<"\n";
                // }
                // if(!op->getType()->getPointerElementType()->isStructTy())
                // {
                //   errs()<<"Error: op is not a struct pointer type:"<<*op<<"\n";
                // }
                //expandAndInstrument(M,B,op,op->getType()->getPointerElementType(),"var_");
                if (loaded->getType()->isPointerTy() &&
                  loaded->getType()->getPointerElementType()->isStructTy()) {
                  expandStructPtr(M, B, loaded,setID);
                }
            }
          }
        }
      }
    }
    else if (type==1)//其是一个load指令.load的地址是一个全局变量，%p=load * class.A ,%Global, 然后store %p,*%a
    {
      llvm::errs()<<"process multi-level struct pointer load instruction type is 1:"<<*op<<"\n";
      if(Instruction *Inst = dyn_cast<Instruction>(op))
      {
        if(auto *LI=dyn_cast<LoadInst>(Inst))
        {
          for(User*UoL :LI->users())
          {
            if(Instruction *Inst1 = dyn_cast<Instruction>(UoL))
            {
              if(auto*SI=dyn_cast<StoreInst>(Inst1))
              {
                Value* ptrOp=SI->getPointerOperand();
                Value* val=SI->getValueOperand();
                if(val==LI)//对于第二种情况，var是由load分支添加进来的，读取了全局变量中的一个指针，然后将该指针存储进了ptrOp
                {
                  B.SetInsertPoint(Inst1->getNextNode());
                  Value *loaded = B.CreateLoad(elemTy, ptrOp, "ld_struct_ptr");
                  if (loaded->getType()->isPointerTy() &&
                    loaded->getType()->getPointerElementType()->isStructTy()) {
                    expandStructPtr(M, B, loaded,setID);
                  }
                  //expandStructPtr(M,B,op,index);
                }
              }
            }
          }
        }
      }
    }
    else if(type==2)
    { //对于第三种情况，var实际上是一个store指令的目标地址,但存放的可能是一个全局变量,store %Global,**%p
      llvm::errs()<<"process multi-level struct pointer store instruction:"<<*op<<"\n";
      for(User* Uov :op->users())
      {
        if(Instruction *Inst = dyn_cast<Instruction>(Uov))
        {
          if(auto*SI=dyn_cast<StoreInst>(Inst))
          {
            Value* ptrOp=SI->getPointerOperand();
            Value* val=SI->getValueOperand();
            if(ptrOp==op&&isa<GlobalVariable>(val))//对于第三种情况，val是一个全局变量指针
            {
                B.SetInsertPoint(Inst->getNextNode());
                Value *loaded = B.CreateLoad(elemTy, ptrOp, "ld_struct_ptr");
                if (loaded->getType()->isPointerTy() &&
                  loaded->getType()->getPointerElementType()->isStructTy()) {
                  expandStructPtr(M, B, loaded,setID);
                }
            }
          }
        }
      }
    }
  }
  else {
    errs()<<"Skip unsupported var type for instrumentation: "<<*op<<"\n";
  }
}
void Nova::expandStructPtr(Module &M, IRBuilder<> &B, Value *ptr,int setID) {
  Type *elemTy = ptr->getType()->getPointerElementType();
  if (!elemTy->isStructTy()) 
  {
    errs()<<"expandStructPtr: Not a struct type: \n ";
    return;
  }
  StructType *ST = cast<StructType>(elemTy);

  for (unsigned i = 0; i < ST->getNumElements(); i++) {
    Type *fieldTy = ST->getElementType(i);
    Value *gep = B.CreateStructGEP(ST, ptr, i, "fld" + std::to_string(i));

    if (fieldTy->isStructTy()) {
      // 子结构体，递归
      expandStructPtr(M, B, gep,setID);

    } else if (fieldTy->isArrayTy()) {
      // 数组展开
      ArrayType *AT = cast<ArrayType>(fieldTy);
      for (unsigned j = 0; j < AT->getNumElements(); j++) {
        Value *idxs[] = {B.getInt32(0), B.getInt32(i), B.getInt32(j)};
        Value *gepElem = B.CreateGEP(ST, ptr, idxs, "arr" + std::to_string(j));
        expandStructPtr(M, B, gepElem,setID);
      }

    } else if (fieldTy->isPointerTy()) {
      // 处理多级指针
      Value *curPtr = B.CreateLoad(fieldTy, gep, "ldptr");
      Type *pointeeTy = fieldTy->getPointerElementType();

      while (pointeeTy->isPointerTy()) {
        // 如果是函数指针，直接跳过
        if (pointeeTy->getPointerElementType()->isFunctionTy()) {
          errs() << "Skip function pointer field at index " << i << "\n";
          curPtr = nullptr;
          break;
        }

        // 多级指针，继续解引用
        curPtr = B.CreateLoad(pointeeTy, curPtr, "ldptr");
        pointeeTy = pointeeTy->getPointerElementType();
      }

      if (!curPtr) continue;

      if (pointeeTy->isStructTy()) {
        // 指向结构体，递归展开
        expandStructPtr(M, B, curPtr,setID);

      } else if (pointeeTy->isIntegerTy() || pointeeTy->isFloatTy()) {
        // 指向基础类型，load 最终值并插桩
        Value *derefVal = B.CreateLoad(pointeeTy, curPtr, "deref");
        if(NovaDynamicCollect==CollectValue)
        {
          insertInstrumentation_to_collect(M, B, derefVal,CriticalVarIndex);
        }
        else if (NovaDynamicCollect==DefUseCheck1)
        {
        // 先初始化为 0
          float maxvalue = 0.0f;
          float minvalue = 0.0f;

          // 查找是否存在该 Index
          auto it = m_cdp_value_range.find(CriticalVarIndex);

          // 如果找到了 (it 不等于 end)，则取实际值
          if (it != m_cdp_value_range.end()) {
              // it->second 代表 map 中的值 (即那个 tuple/pair)
              minvalue = std::get<0>(it->second);
              maxvalue = std::get<1>(it->second);
          }
          // float maxvalue = std::get<1>(m_cdp_value_range[CriticalVarIndex]);
          // float minvalue = std::get<0>(m_cdp_value_range[CriticalVarIndex]);
          Value* addr=curPtr;
          insertInstrumentation_to_check(M,B,addr,derefVal,setID,maxvalue,minvalue);
        }
        CriticalVarIndex++;

      } else {
        errs() << "Skip unsupported pointer pointee type at index " << i << "\n";
      }

    } else {
      // 叶子字段，只处理基础类型
      if (fieldTy->isIntegerTy() || fieldTy->isFloatTy()) {
        Value *loaded = B.CreateLoad(fieldTy, gep, "ldfld");
        if(NovaDynamicCollect==CollectValue){
          insertInstrumentation_to_collect(M, B,loaded,CriticalVarIndex);
        }
        else if(NovaDynamicCollect==DefUseCheck1)
        {
          // 先初始化为 0
          float maxvalue = 0.0f;
          float minvalue = 0.0f;

          // 查找是否存在该 Index
          auto it = m_cdp_value_range.find(CriticalVarIndex);

          // 如果找到了 (it 不等于 end)，则取实际值
          if (it != m_cdp_value_range.end()) {
              // it->second 代表 map 中的值 (即那个 tuple/pair)
              minvalue = std::get<0>(it->second);
              maxvalue = std::get<1>(it->second);
          }
          // float maxvalue = std::get<1>(m_cdp_value_range[CriticalVarIndex]);
          // float minvalue = std::get<0>(m_cdp_value_range[CriticalVarIndex]);
          //获取指针的地址
          Value* addr=gep;
          insertInstrumentation_to_check(M,B,addr,loaded,setID,maxvalue,minvalue);
        }
        CriticalVarIndex++;
      } else {
        errs() << "Skip unsupported field type at index " << i << "\n";
      }
    }
  }
}
void Nova::expandAndInstrument(Module &M, IRBuilder<> &B, Value *ptr, Type *ty, std::string prefix ) {
  if (!ty) return;

  if (ty->isStructTy()) {
    // 结构体，逐字段展开
    StructType *ST = cast<StructType>(ty);
    for (unsigned i = 0; i < ST->getNumElements(); i++) {
      Type *fieldTy = ST->getElementType(i);
      Value *gep = B.CreateStructGEP(ST, ptr, i, prefix + "fld" + std::to_string(i));
      expandAndInstrument(M, B, gep, fieldTy, prefix + "fld" + std::to_string(i) + "_");
    }

  } else if (ty->isArrayTy()) {
    // 数组，逐元素展开
    ArrayType *AT = cast<ArrayType>(ty);
    Type *elemTy = AT->getElementType();
    for (unsigned j = 0; j < AT->getNumElements(); j++) {
      Value *idxs[] = {B.getInt32(0), B.getInt32(j)};
      Value *gep = B.CreateGEP(ty, ptr, idxs, prefix + "arr" + std::to_string(j));
      expandAndInstrument(M, B, gep, elemTy, prefix + "arr" + std::to_string(j) + "_");
    }

  } else if (ty->isPointerTy()) {
    // 指针类型
    Type *pointeeTy = ty->getPointerElementType();
    if (pointeeTy->isFunctionTy()) {
      errs() << "Skip function pointer: " << prefix << "\n";
      return;
    }

    // load 出指针指向的值
    Value *loadedPtr = B.CreateLoad(pointeeTy, ptr, prefix + "ldptr");
    expandAndInstrument(M, B, loadedPtr, pointeeTy, prefix + "deref_");

  } else if (ty->isIntegerTy() || ty->isFloatingPointTy()) {
    // 基础类型，load 并插桩
    Value *loadedVal = B.CreateLoad(ty, ptr, prefix + "val");
    insertInstrumentation_to_collect(M,B,loadedVal,CriticalVarIndex);
    CriticalVarIndex++;

  } else {
    // 不支持的类型
    errs() << "Skip unsupported type at: " << prefix << "\n";
  }
}
void Nova::insertInstrumentation_to_check(Module &M,IRBuilder<> &B,Value* addr,Value *val,int setID,float maxvalue,float minvalue)
{
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *I32Ty  = Type::getInt32Ty(Ctx);
  Type *I64Ty  = Type::getInt64Ty(Ctx);
  errs() << "^^^^^^^^^^ insertInstrumentation to check ^^^^^^^^^^^\n";
  FunctionCallee RecordDefEvt = M.getOrInsertFunction("Critical_def_check",FunctionType::get(VoidTy, {I32Ty,I32Ty, I32Ty,I32Ty,I32Ty}, false));
  Function *RecordDefEvtFunc = cast<Function>(RecordDefEvt.getCallee());
  RecordDefEvtFunc->addFnAttr(Attribute::NoInline);

  FunctionCallee RecordDefEvtForFloat=M.getOrInsertFunction(
      "Critical_def_check_for_float",
      FunctionType::get(VoidTy, {I32Ty,Type::getFloatTy(Ctx), I32Ty,I32Ty,I32Ty}, false));
  Function *RecordDefEvtFuncForFloat = cast<Function>(RecordDefEvtForFloat.getCallee());
  RecordDefEvtFuncForFloat->addFnAttr(Attribute::NoInline);
  // result value that must be i64 to pass into def_check
  Value *castVal = nullptr;
  Type *vt = val->getType();
  bool is_float=false;
  // ----- pointer -----
  if (vt->isPointerTy()) {
    // 将指针直接转换为 i32（intptr）
    castVal = B.CreatePtrToInt(val, I32Ty, "ptr_to_i32");
    errs() << "Instrumenting pointer value: " << *val << "\n";
  }
  // ----- integers -----
  else if (vt->isIntegerTy()) {
    unsigned bw = vt->getIntegerBitWidth();
    if (bw == 32) {
      castVal = val; // already i32
      errs() << "Instrumenting i32 value: " << *val << "\n";
    } else if (bw < 32) {
      // zero-extend to i64 (you can use sign-extend if needed)
      castVal = B.CreateZExt(val, I32Ty, "zext_to_i32");
      errs() << "Instrumenting integer (zext->i64): " << *val << "\n";
    } else { // bw > 64
      // truncate to i64 (be careful about overflow)
      castVal = B.CreateTrunc(val, I32Ty, "trunc_to_i32");
      errs() << "Instrumenting integer (trunc->i32): " << *val << "\n";
    }
  }
  // ----- float -----
  else if (vt->isFloatTy()) {
    // bitcast float -> i32, then zext to i64
    is_float=true;
    castVal = val;
    //castVal = B.CreateZExt(asI32, I64Ty, "zext_f_i64");
    errs() << "Instrumenting float value: " << *val << "\n";
  }
  // // ----- double -----
  // else if (vt->isDoubleTy()) {
  //   // bitcast double -> i64
  //   castVal = B.CreateBitCast(val, I64Ty, "double_bitcast_i64");
  //   errs() << "Instrumenting double value: " << *val << "\n";
  // }
  // ----- other: try load if pointer-like passed in as Value* (defensive) -----
  else {
    errs() << "Unsupported value type for instrumentation: " << *vt << "\n";
    return;
  }
  Value *castAddr;
  //将addr转换为i32
  castAddr = B.CreatePtrToInt(addr, I32Ty, "ptr_to_i32");
  // second arg: index
  Value *castSetID = ConstantInt::get(I32Ty, setID);
  //将maxvalue和minvalue转换为i64
  Value *castMaxValue = ConstantInt::get(I32Ty, *(uint32_t*)&maxvalue);
  Value *castMinValue = ConstantInt::get(I32Ty, *(uint32_t*)&minvalue);
  // 调用收集函数
  if(is_float)
  {
    B.CreateCall(RecordDefEvtFuncForFloat, {castAddr,castVal, castSetID,castMaxValue,castMinValue});
  }
  else
  {
    B.CreateCall(RecordDefEvtFunc, {castAddr,castVal, castSetID,castMaxValue,castMinValue});
  }
  
}

void Nova::insertInstrumentation_to_collect(Module &M, IRBuilder<> &B, Value *val, int index) {
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *I32Ty  = Type::getInt32Ty(Ctx);
  Type *I64Ty  = Type::getInt64Ty(Ctx);
  Type *FloatTy= Type::getFloatTy(Ctx);
  errs() << "^^^^^^^^^^ insertInstrumentation to collect ^^^^^^^^^^^\n";

  // def_check(i64, i32)
  FunctionCallee RecordDefEvtForFloat=M.getOrInsertFunction(
      "def_collect_for_float",
      FunctionType::get(VoidTy, {Type::getFloatTy(Ctx), I32Ty}, false));
  Function *RecordDefEvtFuncForFloat = cast<Function>(RecordDefEvtForFloat.getCallee());
  RecordDefEvtFuncForFloat->addFnAttr(Attribute::NoInline);

  FunctionCallee RecordDefEvt = M.getOrInsertFunction(
      "def_collect",
      FunctionType::get(VoidTy, {I32Ty, I32Ty}, false));
  Function *RecordDefEvtFunc = cast<Function>(RecordDefEvt.getCallee());
  RecordDefEvtFunc->addFnAttr(Attribute::NoInline);

  // result value that must be i64 to pass into def_check
  Value *castVal = nullptr;

  Type *vt = val->getType();
  bool is_float=false;
  // ----- pointer -----
  if (vt->isPointerTy()) {
    // 将指针直接转换为 i32（intptr）
    castVal = B.CreatePtrToInt(val, I32Ty, "ptr_to_i32");
    errs() << "Instrumenting pointer value: " << *val << "\n";
  }
  // ----- integers -----
  else if (vt->isIntegerTy()) {
    unsigned bw = vt->getIntegerBitWidth();
    if (bw == 32) {
      castVal = val;
      errs() << "Instrumenting i32 value: " << *val << "\n";
    } else if (bw < 32) {
      castVal = B.CreateZExt(val, I32Ty, "zext_to_i32");
      errs() << "Instrumenting integer (zext->i32): " << *val << "\n";
    } else { // bw > 32
      castVal = B.CreateTrunc(val, I32Ty, "trunc_to_i32");
      errs() << "Instrumenting integer (trunc->i32): " << *val << "\n";
    }
  }
  // ----- float -----
  else if (vt->isFloatTy()) {
    // bitcast float -> i32, then zext to i64
    //castVal = B.CreateBitCast(val, Type::getInt32Ty(Ctx), "float_bitcast_i32");
    //castVal = B.CreateZExt(asI32, I64Ty, "zext_f_i64");
    is_float=true;
    castVal=val;
    errs() << "Instrumenting float value: " << *val << "\n";
  }
  // // ----- double -----
  // else if (vt->isDoubleTy()) {
  //   // bitcast double -> i64
  //   castVal = B.CreateBitCast(val, I64Ty, "double_bitcast_i64");
  //   errs() << "Instrumenting double value: " << *val << "\n";
  // }
  // ----- other: try load if pointer-like passed in as Value* (defensive) -----
  else {
    errs() << "Unsupported value type for instrumentation: " << *vt << "\n";
    return;
  }

  // second arg: index
  Value *castIndex = ConstantInt::get(I32Ty, index);
  if(is_float)
  {
    // 调用收集函数
    B.CreateCall(RecordDefEvtFuncForFloat, {castVal,castIndex});
    return;
  }
  else{
    // 调用收集函数
    B.CreateCall(RecordDefEvtFunc, {castVal, castIndex});
    return;
  }

}


void Nova::CheckNoSenVarDef(Module &M, Value *var)
{
  Value *op, *val;
  for (User *UoV : var->users()) {// 遍历使用变量 var 的所有用户
      errs()<<"RecordDefineEvent: UoV:" << *UoV <<"\n";
      if (Instruction *Inst = dyn_cast<Instruction>(UoV)) {// 如果用户是指令
          errs()<<"RecordDefineEvent: Inst:" << *Inst <<"\n";
          // normal variable
          if (isa<StoreInst>(Inst)){
              op = cast<StoreInst>(Inst)->getPointerOperand();
              val = cast<StoreInst>(Inst)->getValueOperand();
              if (op == var) {//如果 Store 操作的地址操作数等于变量 var
                  errs() << "StoreInst: " << *Inst << "\n";
                  // define event :insert call to void def_check(uint32 addr, uint32 val)如果存储的地址是变量 var 本身，插入记录定义事件的代码
                  InstrumentNoSenStoreVar(Inst, val);
              }
          } 
          // else if (isa<LoadInst>(Inst)) {// 处理指针变量的定义
          //     for (User *UoL : Inst->users()) {
          //         if (Instruction *Inst1 = dyn_cast<Instruction>(UoL)) {
          //             if (isa<StoreInst>(Inst1)){// 处理 Load 指令的用户中的 Store 指令
          //               // int **pp;
          //               // void foo() {
          //               //     int *p = *pp;  // load
          //               //     *p = 42;       // store, 地址操作数用的就是 load 的结果
          //               // }
          //               //对应IR
          //               // %0 = load i32*, i32** @pp          ; loadInst (Inst)
          //               // store i32 42, i32* %0              ; StoreInst (Inst1), PointerOperand = %0
          //                 val = cast<StoreInst>(Inst1)->getValueOperand();
          //                 op = cast<StoreInst>(Inst1)->getPointerOperand();
          //                 if (op == Inst) {// 如果 Store 操作的地址操作数等于 Load 操作的结果
          //                     errs() << "LoadInst Users: " << *Inst1 << "\n";
          //                     InstrumentNoSenStoreVar(Inst1, val);
          //                 }
          //             }
          //         } else {
          //             //errs() <<"    " << UoL->getName() << " is not an instruction\n";
          //         }
          //     }
          // }
      }
  }
}
void Nova::InstrumentNoSenStoreVar(Instruction *inst, Value *addr)
{
    IRBuilder<> B(inst);
    Module *M = B.GetInsertBlock()->getModule();
    Type *VoidTy = B.getVoidTy();
    Type *I32Ty = B.getInt32Ty();
    Type *I64Ty = B.getInt64Ty(); // 获取 64 位整数类型
    Value *castAddr, *castVal,* castSetID;

    //errs() << __func__ << " : "<< *inst << "\n";
    errs() << "^^^^^^^^^^InstrumentStoreInst^^^^^^^^^^^\n";
    //jinwen comment
    // Constant *RecordDefEvt = M->getOrInsertFunction("__record_defevt", VoidTy,
    //                                                                   I32Ty,
    //                                                                   I32Ty, 
    //                                                                   nullptr);

    FunctionCallee RecordDefEvt = M->getOrInsertFunction("non_sen_def_check", VoidTy, I32Ty);
    Function *RecordDefEvtFunc = cast<Function>(RecordDefEvt.getCallee());
    RecordDefEvtFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联

    castAddr = CastInst::Create(Instruction::PtrToInt, addr, I32Ty, "recptrtoint", inst);

    //enable data-flow integrity.
    B.CreateCall(RecordDefEvtFunc, {castAddr});

    return;
}
// For var, there are two cases:
//  # int var;  store val, var
//  # int *var; load tmp, var; store val, tmp;
// Explanation: 
//  Because var could be normal variable or a pointer
//  For normal variable, we consider define event as a store inst using var as address
//  For pointer variable,  we also consider pointer based write, which first load var's
//  value into tmp var, then use tmp var as address to write.
void Nova::RecordDefineEvent(Module &M, Value *var,int setID) {
    Value *op, *val;
    for (User *UoV : var->users()) {// 遍历使用变量 var 的所有用户
        //errs()<<"RecordDefineEvent: UoV:" << *UoV <<"\n";
        if (Instruction *Inst = dyn_cast<Instruction>(UoV)) {// 如果用户是指令
            //errs()<<"RecordDefineEvent: Inst:" << *Inst <<"\n";
            // normal variable
            if (isa<StoreInst>(Inst)){
                op = cast<StoreInst>(Inst)->getPointerOperand();
                val = cast<StoreInst>(Inst)->getValueOperand();
                if (op == var) {//如果 Store 操作的地址操作数等于变量 var
                    errs() << "StoreInst: " << *Inst << "\n";
                    // define event :insert call to void def_check(uint32 addr, uint32 val)如果存储的地址是变量 var 本身，插入记录定义事件的代码
                    InstrumentStoreInst(Inst, op, val,setID);
                }
            } else if (isa<LoadInst>(Inst)) {// 处理指针变量的定义
                for (User *UoL : Inst->users()) {
                    if (Instruction *Inst1 = dyn_cast<Instruction>(UoL)) {
                        if (isa<StoreInst>(Inst1)){// 处理 Load 指令的用户中的 Store 指令
                          // int **pp;
                          // void foo() {
                          //     int *p = *pp;  // load
                          //     *p = 42;       // store, 地址操作数用的就是 load 的结果
                          // }
                          //对应IR
                          // %0 = load i32*, i32** @pp          ; loadInst (Inst)
                          // store i32 42, i32* %0              ; StoreInst (Inst1), PointerOperand = %0
                            val = cast<StoreInst>(Inst1)->getValueOperand();
                            op = cast<StoreInst>(Inst1)->getPointerOperand();
                            if (op == Inst) {// 如果 Store 操作的地址操作数等于 Load 操作的结果
                                errs() << "LoadInst Users: " << *Inst1 << "\n";
                                InstrumentStoreInst(Inst1, op, val,setID);
                            }
                        }
                    } else {
                        //errs() <<"    " << UoL->getName() << " is not an instruction\n";
                    }
                }
            }
        }
    }
}
//插入“记录定义”函数
void Nova::InstrumentStoreInst(Instruction *inst, Value *addr, Value *val,int setID) {

    
    IRBuilder<> B(inst);
    Module *M = B.GetInsertBlock()->getModule();
    Type *VoidTy = B.getVoidTy();
    Type *I32Ty = B.getInt32Ty();
    Type *I64Ty = B.getInt64Ty(); // 获取 64 位整数类型
    Value *castAddr, *castVal,* castSetID;

    //errs() << __func__ << " : "<< *inst << "\n";
    define_event_count++;
    errs() << "^^^^^^^^^^InstrumentStoreInst^^^^^^^^^^^\n";

    #if defined(SMALLTEST) 
    // 检查 addr 是否是 GEP 指令，如果是，则检查其访问的基址是否是预定义数组
    if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(addr)) {
        Value* basePtr = gepInst->getPointerOperand();
        if (isPreDefinedArray(basePtr)) {
            errs() << "GEP 访问预定义数组，跳过插桩: " << basePtr->getName() << "\n";
            return;
        }
    }
    if (auto *gep = dyn_cast<GetElementPtrInst>(addr)) {
        // 检查 GEP 的源类型是不是 struct
        if (!gep->getSourceElementType()->isStructTy()) {
            return ;
        }
    }
    #endif
    //jinwen comment
    // Constant *RecordDefEvt = M->getOrInsertFunction("__record_defevt", VoidTy,
    //                                                                   I32Ty,
    //                                                                   I32Ty, 
    //                                                                   nullptr);

    FunctionCallee RecordDefEvt = M->getOrInsertFunction("def_check", VoidTy, I32Ty, I32Ty,I32Ty);
  
    Function *RecordDefEvtFunc = cast<Function>(RecordDefEvt.getCallee());
    RecordDefEvtFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联

    FunctionCallee RecordDefEvtForBasicTypeInStruct= M->getOrInsertFunction("def_check_for_basic_type_in_struct", VoidTy, I32Ty, I32Ty,I32Ty);
    
    Function *RecordDefEvtForBasicTypeInStructFunc=cast<Function>(RecordDefEvtForBasicTypeInStruct.getCallee());
    RecordDefEvtForBasicTypeInStructFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联
    FunctionCallee RecordDefEvtForPtrInStruct= M->getOrInsertFunction("def_check_for_ptr_in_struct", VoidTy, I32Ty, I32Ty,I32Ty);
    Function *RecordDefEvtForPtrInStructFunc=cast<Function>(RecordDefEvtForPtrInStruct.getCallee());
    RecordDefEvtForPtrInStructFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联

    castAddr = CastInst::Create(Instruction::PtrToInt, addr, I32Ty, "recptrtoint", inst);
    bool is_basic_type_in_struct=false;
    bool is_pointer_in_struct=false;
    if (val->getType()->isPointerTy()) {//如果是指针类型，则转化为32位整数
            // 如果值是指针类型，将其转换为 32 位整数，并零扩展到 64 位
      if(whetherIsAFiledInStruct(addr))
      {
        //enable data-flow integrity for pointer in struct.
        is_pointer_in_struct=true;
      }
      castVal = CastInst::Create(Instruction::PtrToInt, val, I32Ty, "chkptrtoint", inst);
      //castVal = CastInst::Create(Instruction::ZExt, tmpVal, I64Ty, "chkzexttoi64", inst);
      errs() << "正在插桩指针值: " << *val << "\n"; // 调试输出：处理指针值
    } 
    else if (val->getType()->isIntegerTy(32)) {
        castVal = val;
        errs() << "正在插桩 32 位整数值: " << *val << "\n";
        if (whetherIsAFiledInStruct(addr)) is_basic_type_in_struct = true;
    } else if (val->getType()->isIntegerTy()) {
        unsigned bw = cast<IntegerType>(val->getType())->getBitWidth();
        if (bw < 32) {
            castVal = B.CreateZExt(val, I32Ty, "chkzexttoi32");
        } else { // bw > 32
            castVal = B.CreateTrunc(val, I32Ty, "chktrunctoi32");
        }
        errs() << "正在插桩其他整数值: " << *val << "\n";
        if (whetherIsAFiledInStruct(addr)) is_basic_type_in_struct = true;
    }
    else if (val->getType()->isFloatTy()) 
    {
      // 如果值是 float 类型，通过位转换重新解释为 32 位整数，再零扩展到 64 位
      castVal = CastInst::Create(Instruction::BitCast, val, I32Ty, "chkfloattoint", inst);
      //castVal = CastInst::Create(Instruction::ZExt, tmpVal, I64Ty, "chkzexttoi64", inst);
      if(whetherIsAFiledInStruct(addr))
      {
        is_basic_type_in_struct=true;
      }
      errs() << "正在插桩 float 值: " << *val << "\n"; // 调试输出：处理 float 值
    } 
    // else if (val->getType()->isDoubleTy()) 
    // {
    //     // 如果值是 double 类型，通过位转换重新解释为 64 位整数
    //     castVal = CastInst::Create(Instruction::BitCast, val, I64Ty, "chkdoubletoint", inst);
    //     errs() << "正在插桩 double 值: " << *val << "\n"; // 调试输出：处理 double 值
    // }
    else
    {
      errs() << "Unsupported value type for InstrumentStoreInst: " << *val->getType() << "\n";
      return;
    }
    castSetID = ConstantInt::get(I32Ty, setID); // 将 setID 转换为 32 位整数
    //enable data-flow integrity.
    if(is_basic_type_in_struct)
      B.CreateCall(RecordDefEvtForBasicTypeInStructFunc, {castAddr,castVal, castSetID});
    else if(is_pointer_in_struct)
      B.CreateCall(RecordDefEvtForPtrInStructFunc, {castAddr,castVal, castSetID});
    else
      B.CreateCall(RecordDefEvtFunc, {castAddr, castVal,castSetID});

    return;
}
bool Nova::whetherIsAFiledInStruct(Value*val)
{
  //判断val是否是一个结构体的某个变量
    if (auto *gep = dyn_cast<GetElementPtrInst>(val)) {
        // 检查 GEP 的源类型是不是 struct
        if (gep->getSourceElementType()->isStructTy()) {
            return true;
        }
    }
  return false;
}
//插入“检查使用”函数
void Nova::InstrumentLoadInst(Instruction *inst, Value *addr, Value *val,int setID) {

    
    IRBuilder<> B(inst);
    Module *M = B.GetInsertBlock()->getModule();
    Type *VoidTy = B.getVoidTy();
    Type *I32Ty = B.getInt32Ty();
    Type *I64Ty = B.getInt64Ty(); // 获取 64 位整数类型
    Value *castAddr, *castVal, *castSetID;

    
    errs() << "^^^^^^^^^^InstrumentLoadInst^^^^^^^^^^^\n";
    //errs() << __func__ << " inst: "<< *inst << "\n";
    //errs() << __func__ << " addr->name: "<< addr->getName() << "\n";
    //errs() << __func__ << " val->name: "<< val->getName() << "\n";

    // 检查 val 是否是 call 指令的返回值
    if (isa<CallInst>(addr)||isa<InvokeInst>(addr)||isa<BinaryOperator>(addr) || isa<BitCastInst>(addr) || isa<CastInst>(addr) || isa<PHINode>(addr) || isa<SelectInst>(addr) ||isa<LoadInst>(addr) ) {
        errs() << "val 是 call 指令的返回值，跳过插桩: " << *val << "\n";
        return;
    }
    #if defined(SMALLTEST) 
    // 检查 addr 是否是 GEP 指令，如果是，则检查其访问的基址是否是预定义数组
    if (GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(addr)) {
        Value* basePtr = gepInst->getPointerOperand();
        if (isPreDefinedArray(basePtr)) {
            errs() << "GEP 访问预定义数组，跳过插桩: " << basePtr->getName() << "\n";
            return;
        }
    }
    if (auto *gep = dyn_cast<GetElementPtrInst>(addr)) {
        // 检查 GEP 的源类型是不是 struct
        if (!gep->getSourceElementType()->isStructTy()) {
            return ;
        }
    }
    #endif
    //jinwen comment
    // Constant *CheckUseEvt = M->getOrInsertFunction("__check_useevt", VoidTy,
    //                                                                  I64Ty,
    //                                                                  I64Ty, 
    //                                                                  nullptr);
    

    FunctionCallee CheckUseEvt = M->getOrInsertFunction("use_check", VoidTy,
                                                                     I32Ty,
                                                                     I32Ty,
                                                                     I32Ty);// 添加 setID 参数 


    FunctionCallee CheckUseEvtForBasicTypeInStruct= M->getOrInsertFunction("use_check_for_basic_type_in_struct", VoidTy,
                                                                     I32Ty,
                                                                     I32Ty,
                                                                     I32Ty);// 添加 setID 参数
    Function *CheckUseEvtFunc = cast<Function>(CheckUseEvt.getCallee());
    Function *CheckUseEvtForBasicTypeInStructFunc=cast<Function>(CheckUseEvtForBasicTypeInStruct.getCallee());
    CheckUseEvtFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联

    CheckUseEvtForBasicTypeInStructFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联
    castAddr = CastInst::Create(Instruction::PtrToInt, addr, I32Ty, "chkptrtoint", inst);// 将地址值转换为 32 位整数

    FunctionCallee CheckUseEvtForPtrInStruct= M->getOrInsertFunction("use_check_for_ptr_in_struct", VoidTy,
                                                                     I32Ty,
                                                                     I32Ty,
                                                                     I32Ty);// 添加 setID 参数
    Function *CheckUseEvtForPtrInStructFunc=cast<Function>(CheckUseEvtForPtrInStruct.getCallee());
    CheckUseEvtForPtrInStructFunc->addFnAttr(Attribute::NoInline);// 设置函数为不内联
    bool is_pointer_in_struct=false;
    bool is_basic_type_in_struct=false;
    
    if (val->getType()->isPointerTy()) {
        // 如果值是指针类型，将其转换为 32 位整数
        castVal = CastInst::Create(Instruction::PtrToInt, val, I32Ty, "chkptrtoint", inst);
        if(whetherIsAFiledInStruct(addr))
        {
          //enable data-flow integrity for pointer in struct.
          is_pointer_in_struct=true;
        }
        errs() << "正在插桩指针值: " << *val << "\n"; // 调试输出：处理指针值
    } 
    else if (val->getType()->isIntegerTy()) {
      unsigned bw = cast<IntegerType>(val->getType())->getBitWidth();
      if (bw < 32) {
          castVal = B.CreateZExt(val, I32Ty, "chkzexttoi32");
      } else if (bw > 32) {
          castVal = B.CreateTrunc(val, I32Ty, "chktrunctoi32");
      } else {
          castVal = val;
      }
      errs() << "正在插桩整数值: " << *val << "\n";
      if (whetherIsAFiledInStruct(addr)) {
          is_basic_type_in_struct = true;
      }
    }
    else if (val->getType()->isFloatTy()) {
        // 如果值是 float 类型，通过位转换重新解释为 32 位整数
        castVal= CastInst::Create(Instruction::BitCast, val, I32Ty, "chkfloattoint", inst);
        if(whetherIsAFiledInStruct(addr))
        {
          is_basic_type_in_struct=true;
        }
        errs() << "正在插桩 float 值: " << *val << "\n"; // 调试输出：处理 float 值
    } 
    // else if (val->getType()->isDoubleTy()) {
    //     // 如果值是 double 类型，通过位转换重新解释为 64 位整数
    //     castVal = CastInst::Create(Instruction::BitCast, val, I64Ty, "chkdoubletoint", inst);
    //     errs() << "正在插桩 double 值: " << *val << "\n"; // 调试输出：处理 double 值
    // }
      else {
        // 如果值类型不受支持，输出错误信息并返回
        errs() << "InstrumentLoadInst 不支持的值类型: " << *val->getType() << "\n";
        return;
    }
        
    use_event_count++;
    
    castSetID = ConstantInt::get(I32Ty, setID); // 将 setID 转换为 32 位整数
    if(is_basic_type_in_struct==true)
    {
      B.CreateCall(CheckUseEvtForBasicTypeInStruct, {castAddr, castVal,castSetID}); // 创建调用指令，传入地址、值和 setID 参数
    }
    else if(is_pointer_in_struct==true) {
      B.CreateCall(CheckUseEvtForPtrInStruct, {castAddr, castVal,castSetID}); // 创建调用指令，传入地址、值和 setID 参数
    }
    else
    {
      
      B.CreateCall(CheckUseEvtFunc, {castAddr, castVal,castSetID}); // 创建调用指令，传入地址、值和 setID 参数
    }

    // remove fake inst
    inst->eraseFromParent();

    return;
}

// 检查变量是否是从常量数组通过 memcpy 初始化的预定义数组，或者是栈上的数组
bool Nova::isPreDefinedArray(Value* var) {
    // 如果不是 AllocaInst，不是局部数组
    AllocaInst* allocaInst = dyn_cast<AllocaInst>(var);
    if (!allocaInst) {
        errs() << "isPreDefinedArray: 不是 AllocaInst: " << *var << "\n";
        return false;
    }
    
    // 检查分配的类型是否是数组类型
    Type* allocatedType = allocaInst->getAllocatedType();
    if (allocatedType->isArrayTy()) {
        errs() << "✓ 检测到栈数组: " << var->getName() << " (数组类型)\n";
        return true;
    }
    
    errs() << "isPreDefinedArray: 检查 AllocaInst: " << var->getName() << "\n";
    
    // 遍历该变量的所有使用
    for (User* user : var->users()) {
        errs() << "  检查 user: " << *user << "\n";
        
        // 检查是否是 GEP，然后检查 GEP 的使用
        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(user)) {
            for (User* gepUser : gep->users()) {
                if (BitCastInst* bitcast = dyn_cast<BitCastInst>(gepUser)) {
                    for (User* bitcastUser : bitcast->users()) {
                        if (CallInst* call = dyn_cast<CallInst>(bitcastUser)) {
                            Function* calledFunc = call->getCalledFunction();
                            // 检测 memset 调用（用于初始化数组为0或其他常量）
                            if (calledFunc && calledFunc->getName().startswith("llvm.memset")) {
                                errs() << "✓ 检测到栈数组: " << var->getName() 
                                       << " (通过 memset 初始化)\n";
                                return true;
                            }
                        }
                    }
                }
            }
        }
        
        // 检查是否是 bitcast，如果是，继续检查 bitcast 的使用
        if (BitCastInst* bitcast = dyn_cast<BitCastInst>(user)) {
            for (User* bitcastUser : bitcast->users()) {
                errs() << "    检查 bitcast user: " << *bitcastUser << "\n";
                
                if (CallInst* call = dyn_cast<CallInst>(bitcastUser)) {
                    Function* calledFunc = call->getCalledFunction();
                    
                    // 检测 memset 调用
                    if (calledFunc && calledFunc->getName().startswith("llvm.memset")) {
                        errs() << "✓ 检测到栈数组: " << var->getName() 
                               << " (通过 memset 初始化)\n";
                        return true;
                    }
                    
                    if (calledFunc && calledFunc->getName().startswith("llvm.memcpy")) {
                        errs() << "      发现 memcpy 调用\n";
                        
                        // 检查 memcpy 的目标是否是当前 bitcast
                        Value* dest = call->getArgOperand(0);
                        if (dest == bitcast) {
                            // 检查源操作数是否来自全局常量数组
                            Value* src = call->getArgOperand(1);
                            errs() << "        memcpy 源: " << *src << "\n";
                            
                            // 处理 BitCastInst（指令）
                            if (BitCastInst* srcBitcast = dyn_cast<BitCastInst>(src)) {
                                src = srcBitcast->getOperand(0);
                                errs() << "        解除 BitCastInst 后的源: " << *src << "\n";
                            }
                            // 处理 ConstantExpr（常量表达式）
                            else if (ConstantExpr* srcConstExpr = dyn_cast<ConstantExpr>(src)) {
                                if (srcConstExpr->isCast()) {
                                    src = srcConstExpr->getOperand(0);
                                    errs() << "        解除 ConstantExpr 后的源: " << *src << "\n";
                                }
                            }
                            
                            // 检查源是否是全局常量（如 @__const.benchmark_body.in_b）
                            if (GlobalVariable* gv = dyn_cast<GlobalVariable>(src)) {
                                errs() << "        源是全局变量: " << gv->getName() << "\n";
                                if (gv->isConstant() || gv->getName().startswith("__const.")) {
                                    errs() << "✓ 检测到预定义数组: " << var->getName() 
                                           << " 从常量 " << gv->getName() << " 初始化\n";
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // 也检查直接的 memcpy/memset 调用（目标直接是 alloca）
        if (CallInst* call = dyn_cast<CallInst>(user)) {
            Function* calledFunc = call->getCalledFunction();
            
            // 检测 memset 调用
            if (calledFunc && calledFunc->getName().startswith("llvm.memset")) {
                errs() << "✓ 检测到栈数组: " << var->getName() 
                       << " (通过 memset 直接初始化)\n";
                return true;
            }
            
            if (calledFunc && calledFunc->getName().startswith("llvm.memcpy")) {
                // 检查 memcpy 的目标是否是当前变量
                Value* dest = call->getArgOperand(0);
                if (BitCastInst* bitcast = dyn_cast<BitCastInst>(dest)) {
                    dest = bitcast->getOperand(0);
                }
                
                if (dest == var) {
                    // 检查源操作数是否来自全局常量数组
                    Value* src = call->getArgOperand(1);
                    
                    // 处理 BitCastInst（指令）
                    if (BitCastInst* srcBitcast = dyn_cast<BitCastInst>(src)) {
                        src = srcBitcast->getOperand(0);
                    }
                    // 处理 ConstantExpr（常量表达式）
                    else if (ConstantExpr* srcConstExpr = dyn_cast<ConstantExpr>(src)) {
                        if (srcConstExpr->isCast()) {
                            src = srcConstExpr->getOperand(0);
                        }
                    }
                    
                    // 检查源是否是全局常量（如 @__const.benchmark_body.in_b）
                    if (GlobalVariable* gv = dyn_cast<GlobalVariable>(src)) {
                        if (gv->isConstant() || gv->getName().startswith("__const.")) {
                            errs() << "✓ 检测到预定义数组: " << var->getName() 
                                   << " 从常量 " << gv->getName() << " 初始化\n";
                            return true;
                        }
                    }
                }
            }
        }
    }
    
    errs() << "✗ 未检测到预定义数组: " << var->getName() << "\n";
    return false;
}

void Nova::CheckUseEvent(Module &M, Value *var,int setID) {
    LLVMContext& ctxt = M.getContext();
    Value *op;
    BasicBlock *pb;
    Instruction *fakeInst;
    Type *I32Ty = Type::getInt32Ty(ctxt);
    

    
    for (User *UoV : var->users()) {
        if (Instruction *Inst = dyn_cast<Instruction>(UoV)) {
            if (isa<LoadInst>(Inst)){
                op = cast<LoadInst>(Inst)->getPointerOperand();
                if (op == var) {
                    // define event :insert call to record_defevt(uint64 addr, uint64 val)
                    fakeInst = CastInst::Create(Instruction::PtrToInt, op, I32Ty, "fptrtoint"); // 创建一个伪指令，将地址操作数转换为 32 位整数
                    pb = Inst->getParent();
                    assert(pb != nullptr);
                    //pb->getInstList().insertAfter(Inst->getIterator(), fakeInst);// 在当前指令之后插入伪指令
                    fakeInst->insertAfter(Inst); // 在inst指令后面插入fakeInst
                    InstrumentLoadInst(fakeInst, op, Inst,setID); // 调用插入检查使用事件的函数
                    errs() << "checkuserevert: " << *Inst << "\n";
                }
            }
        }
    }
}
//foward analysis to  get more sensitive variables
void Nova::extendSenVarSet(Module &M,GlobalStateRef gs,ValueSet &senVarSet) {
  std::set<Value*> toAdd;                 // 待加入的新敏感变量
   std::queue<Value *> exSenVarQueue;
   Value *pSenObj, *nSenObj;
   Value *var, *exvar, *val;
  for(auto it = senVarSet.begin(), ie = senVarSet.end(); it != ie; ++it)
  {
    Value* v=*it;
    exSenVarQueue.push(v);
  }
   while(!exSenVarQueue.empty()) {
      var = exSenVarQueue.front();
       exSenVarQueue.pop();
       // Get all the users of var
       for (User *UoV : var->users()) {
           if (Instruction *Inst = dyn_cast<Instruction>(UoV)) {
               errs() << var->getName() <<" is used in instruction:\n";
               errs() << *Inst << "\n";
               // From propagation rule 1: find exvar follow load-a-store-b chain.
               // From propagation rule 2: find exvar follow store-a-ptr chain.
               if (isa<LoadInst>(Inst)){
                   for (User *UoL : Inst->users()) {
                       if (Instruction *Inst1 = dyn_cast<Instruction>(UoL)) {
                           errs() << "    " << "cur inst is used in instruction:\n";
                           errs() << "    " << *Inst1 << "\n";
                           if (isa<StoreInst>(Inst1)){
                               val = cast<StoreInst>(Inst1)->getValueOperand();
                               exvar = cast<StoreInst>(Inst1)->getPointerOperand();
                               if (val == Inst) {
                                  toAdd.insert(exvar);
                                   // TODOO Copy pointsToSet from nSenObj to pSenObj
                                   exSenVarQueue.push(exvar);
                               }

                               errs() << "    " << "     " << "store val :" << *val << "\n";
                               errs() << "    " << "     " << "store operand :" << exvar->getName() << "\n";
                           }
                       } else {
                           errs() <<"    " << UoL->getName() << " is not an instruction\n";
                       }
                   }
               } else if(isa<StoreInst>(Inst)) {
                   val = cast<StoreInst>(Inst)->getValueOperand();
                   exvar = cast<StoreInst>(Inst)->getPointerOperand();
                   if (val == var) {
                       exSenVarQueue.push(exvar);
                       toAdd.insert(exvar);

                       errs() << "    " << "val:" << *val << "\n";
                       errs() << "    " << "var:" << *var << "\n";
                       errs() << "    " << "exvar:" << *exvar << "\n";
                   } else {
                       errs() << "    " << "val:" << *val << "\n";
                       errs() << "    " << "var:" << *var << "\n";
                   }
               }
              //  else if (isa<BitCastInst>(Inst) || isa<CastInst>(Inst) || isa<GetElementPtrInst>(Inst))
              //  {
              //     val = Inst->getOperand(0);
              //     exvar = Inst;
              //     if (val == var) {          // Check if the input operand is the sensitive variable
              //           exSenVarQueue.push(exvar); // Propagate the result as a sensitive variable
              //         //toAdd.insert(exvar);       // Add to the set of sensitive variables
              //     }
              //  }
              //  else if (isa<PHINode>(Inst) || isa<SelectInst>(Inst) || Inst->isBinaryOp())
              //  {
              //     for (unsigned i = 0; i < Inst->getNumOperands(); ++i) {
              //         val = Inst->getOperand(i);
              //         exvar = Inst;
              //         if (val == var) 
              //         {          // Check if the input operand is the sensitive variable
              //             exSenVarQueue.push(exvar); // Propagate the result as a sensitive variable
              //             //toAdd.insert(exvar);       // Add to the set of sensitive variables
                          
              //         }
              //     }
              //  }
           } else {
               errs() << "UoV is not an instruction:\n";
               errs() << *UoV<<"\n";
           }
       }

       errs() << "\n";
    // 写回集合与别名映射
    for (Value* nv : toAdd) {
      senVarSet.insert(nv);
    }
   }

  //  errs() << "extended sensitive var set: \n";
  //  for (auto v :exSenVarSet) {
  //      errs() << (*v).getName()<<"\n";
  //  }
}

char Nova::ID = 0;
std::map<std::pair<std::string,int>,std::set<std::string>> Nova::indirect_call_graph={};//存储由指针分析svf+llvm内置虚函数分析得到的间接调用关系
std::map<std::pair<std::string,int>,std::set<std::string>> Nova::direct_call_graph{};//存储直接调用
static RegisterPass<Nova> X("nova", "Nova Module Pass", false, false);
