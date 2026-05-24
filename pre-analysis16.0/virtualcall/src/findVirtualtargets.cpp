#include "vCallAnalysis.h"
#include "inCallUtil.h"
#include "macro.h"
using namespace llvm;
using namespace vcall;

VirtualCallTarget::VirtualCallTarget(Function *Fn, const TypeMemberInfo *TM)
    : Fn(Fn), TM(TM),
      IsBigEndian(Fn->getParent()->getDataLayout().isBigEndian()), WasDevirt(false) {}

CallSiteInfo &VTableSlotInfo::findCallSiteInfo(CallBase &CB) {
  std::vector<uint64_t> Args;
  auto *CBType = dyn_cast<IntegerType>(CB.getType());
  if (!CBType || CBType->getBitWidth() > 64 || CB.arg_empty())
    return CSInfo;
  for (auto &&Arg : drop_begin(CB.args())) {
    auto *CI = dyn_cast<ConstantInt>(Arg);
    if (!CI || CI->getBitWidth() > 64)
      return CSInfo;
    Args.push_back(CI->getZExtValue());
  }
  return ConstCSInfo[Args];
}

void VTableSlotInfo::addCallSite(Value *VTable, CallBase &CB) {
  auto &CSI = findCallSiteInfo(CB);
  CSI.CallSites.push_back({VTable, CB});
}


PreservedAnalyses VCallFinderPass::run(Module &M,
                                              ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto AARGetter = [&](Function &F) -> AAResults & {
    return FAM.getResult<AAManager>(F);
  };
  auto OREGetter = [&](Function *F) -> OptimizationRemarkEmitter & { 
    return FAM.getResult<OptimizationRemarkEmitterAnalysis>(*F);
  };
  auto LookupDomTree = [&FAM](Function &F) -> DominatorTree & {
    return FAM.getResult<DominatorTreeAnalysis>(F);
  };
  if (!TargetModule(M, AARGetter, OREGetter, LookupDomTree).run())
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

void TargetModule::buildTypeIdentifierMap(
    std::vector<VTableBits> &Bits,
    DenseMap<Metadata *, std::set<TypeMemberInfo>> &TypeIdMap) {
  DenseMap<GlobalVariable *, VTableBits *> GVToBits;
  Bits.reserve(M.getGlobalList().size());
  SmallVector<MDNode *, 2> Types;
  for (GlobalVariable &GV : M.globals()) {
    Types.clear();
    GV.getMetadata(LLVMContext::MD_type, Types);
    if (GV.isDeclaration() || Types.empty())
      continue;

    VTableBits *&BitsPtr = GVToBits[&GV]; 
    if (!BitsPtr) { 
      Bits.emplace_back(); 
      Bits.back().GV = &GV; 
      Bits.back().ObjectSize =
          M.getDataLayout().getTypeAllocSize(GV.getInitializer()->getType());
      BitsPtr = &Bits.back();
    }

    for (MDNode *Type : Types) {
      auto TypeID = Type->getOperand(1).get();

      uint64_t Offset =
          cast<ConstantInt>(
              cast<ConstantAsMetadata>(Type->getOperand(0))->getValue())
              ->getZExtValue();

      TypeIdMap[TypeID].insert({BitsPtr, Offset});
    }
  }
}

bool TargetModule::tryFindVirtualCallTargets(
    std::vector<VirtualCallTarget> &TargetsForSlot,
    const std::set<TypeMemberInfo> &TypeMemberInfos, uint64_t ByteOffset) {
  for (const TypeMemberInfo &TM : TypeMemberInfos) {
    if (!TM.Bits->GV->isConstant())
      return false;

    Constant *Ptr = getPointerAtOffset(TM.Bits->GV->getInitializer(),
                                       TM.Offset + ByteOffset, M);
    if (!Ptr)
      return false;

    auto Fn = dyn_cast<Function>(Ptr->stripPointerCasts());
    if (!Fn)
      return false;

    // We can disregard __cxa_pure_virtual as a possible call target, as
    // calls to pure virtuals are UB.
    if (Fn->getName() == "__cxa_pure_virtual")
      continue;

    TargetsForSlot.push_back({Fn, &TM});
  }

  // Give up if we couldn't find any targets.
  return !TargetsForSlot.empty();
}

void TargetModule::scanTypeTestUsers(
    Function *TypeTestFunc,
    DenseMap<Metadata *, std::set<TypeMemberInfo>> &TypeIdMap) {
  // Find all virtual calls via a virtual table pointer %p under an assumption
  // of the form llvm.assume(llvm.type.test(%p, %md)). This indicates that %p
  // points to a member of the type identifier %md. Group calls by (type ID,
  // offset) pair (effectively the identity of the virtual function) and store
  // to CallSlots.
  for (Use &U : llvm::make_early_inc_range(TypeTestFunc->uses())) { 
    auto *CI = dyn_cast<CallInst>(U.getUser());
    if (!CI)
      continue;

    // Search for virtual calls based on %p and add them to DevirtCalls.
    SmallVector<DevirtCallSite, 1> DevirtCalls;
    SmallVector<CallInst *, 1> Assumes;
    auto &DT = LookupDomTree(*CI->getFunction());
    findDevirtualizableCallsForTypeTest(DevirtCalls, Assumes, CI, DT);
    Metadata *TypeId =
        cast<MetadataAsValue>(CI->getArgOperand(1))->getMetadata();
    // If we found any, add them to CallSlots.
    if (!Assumes.empty()) {
      Value *Ptr = CI->getArgOperand(0)->stripPointerCasts(); 
      for (DevirtCallSite Call : DevirtCalls){
        CallSlots[{TypeId, Call.Offset}].addCallSite(Ptr, Call.CB); 
      }
    }
  }
}

bool TargetModule::run() {

  Function *TypeTestFunc =
      M.getFunction(Intrinsic::getName(Intrinsic::type_test));
  Function *AssumeFunc = M.getFunction(Intrinsic::getName(Intrinsic::assume));

  // Normally if there are no users of the devirtualization intrinsics in the
  // module, this pass has nothing to do. But if we are exporting, we also need
  // to handle any users that appear only in the function summaries.
  if ((!TypeTestFunc || TypeTestFunc->use_empty() || !AssumeFunc ||
       AssumeFunc->use_empty()) )
    return false;

  // Rebuild type metadata into a map for easy lookup.
  std::vector<VTableBits> Bits;
  DenseMap<Metadata *, std::set<TypeMemberInfo>> TypeIdMap;
  buildTypeIdentifierMap(Bits, TypeIdMap);

  if (TypeTestFunc && AssumeFunc)
    scanTypeTestUsers(TypeTestFunc, TypeIdMap);
  
  if (TypeIdMap.empty())
    return true;

  // For each (type, offset) pair:
  std::map<std::string, Function*> DevirtTargets;
  for (auto &S : CallSlots) {
    // Search each of the members of the type identifier for the virtual
    // function implementation at offset S.first.ByteOffset, and add to
    // TargetsForSlot.
    std::vector<VirtualCallTarget> TargetsForSlot;
    const std::set<TypeMemberInfo> &TypeMemberInfos = TypeIdMap[S.first.TypeID];

    if (isa<MDString>(S.first.TypeID) && TypeMemberInfos.size())
        tryFindVirtualCallTargets(TargetsForSlot, TypeMemberInfos, S.first.ByteOffset);

    std::set<llvm::Function*> vcallCandidates;
    for (auto & cs : S.second.CSInfo.CallSites){
        vcallCandidates.clear();
        llvm::Metadata* incallID = cs.CB.getMetadata("inCallID");
        if (!incallID) {
          LLVM_ERROR("No inCallID metadata found for callsite: " << cs.CB);
          exit(1);
        }
        auto *md = llvm::dyn_cast<llvm::MDNode>(incallID);
        int ID = cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
        for (auto &slot : TargetsForSlot){
            vcallCandidates.insert(slot.Fn);
        }
        incallEdges.emplace(ID, incall::IndirectCallEdge(cs.CB, true, vcallCandidates));
    }

    for (auto & pair : S.second.ConstCSInfo){

      for (auto & cs : pair.second.CallSites){
        vcallCandidates.clear();
        llvm::Metadata* incallID = cs.CB.getMetadata("inCallID");
        auto *md = llvm::dyn_cast<llvm::MDNode>(incallID);
        int ID = cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
        for (auto &slot : TargetsForSlot){
            vcallCandidates.insert(slot.Fn);
        }
        incallEdges.emplace(ID, incall::IndirectCallEdge(cs.CB, true, vcallCandidates));
      }
    }
  }

  return true;
}



void vcall::runVirtualCallAnalysis(llvm::Module &M){
    llvm::PassBuilder PB;

    // 创建分析管理器
    llvm::ModulePassManager MPM;
    llvm::ModuleAnalysisManager MAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::LoopAnalysisManager LAM;

    // 向 PassBuilder 注册所有分析
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerLoopAnalyses(LAM);

    // 将所有分析关联到模块级别的管理器中
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    VCallFinderPass vcf;
    MPM.addPass(std::move(vcf));
    MPM.run(M, MAM);
}