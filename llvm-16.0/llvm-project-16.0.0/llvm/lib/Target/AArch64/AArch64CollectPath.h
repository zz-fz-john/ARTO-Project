#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64COLLECTPATH_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64COLLECTPATH_H
#include "llvm/CodeGen/MachineFunction.h"
namespace llvm{
    class AArch64CollectPath :public MachineFunctionPass{
        std::vector<std::string> AvoidHandleFuncName;
        std::vector<std::string> ToprocessFunction;
        std::vector<std::string> OnlyCalledOnceFunc;
        std::vector<int> directCallsiteID;
        std::vector<int> indirectCallsiteID;
        std::vector<std::string> CheckpointFunc;
        bool instrumentIndirectCall(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectCallwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectjump(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentRetLR_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentRetLR(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII);
        bool instrumentLDMIA_RET_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII);
        bool instrumentIndirectJumpPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectCallPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectCallPredwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentBL_pred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentBL(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentOnceCalledFuncLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII);
        void initCheckpointFunc();
        void initProcessFunction();
        void initOnlycalledOnceFunction();
        bool delete_dummy_code(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,std::string calledFuncName,std::vector<MachineInstr *> &DelInstVect);
        public:
        static char ID;
        static int flag;
        static int count;
        static  int inCallID;
        static int dummy_flag;
        AArch64CollectPath(): MachineFunctionPass(ID){initializeAArch64CollectPathPass(*PassRegistry::getPassRegistry());}
        bool runOnMachineFunction(MachineFunction &MF) override;
        StringRef getPassName()const override{
            return "AArch64 Collect Path";
        }
    };
}//end llvm namespace
#endif