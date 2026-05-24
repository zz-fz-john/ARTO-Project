#ifndef LLVM_LIB_TARGET_ARM_ARMCollectIndirectJump_H
#define LLVM_LIB_TARGET_ARM_ARMCollectIndirectJump_H
#include "llvm/CodeGen/MachineFunction.h"


namespace llvm{
    class ARMCollectIndirectJump :public MachineFunctionPass{
        //std::vector<std::string> libcfuncname;
        std::vector<std::string> AvoidHandleFuncName;
        std::vector<std::string> ToprocessFunction;
        std::vector<std::string> OnlyCalledOnceFunc;
        std::vector<std::string> CheckpointFunc;
        //记录并哈希计算indirect call 指令的源地址和目的地址
        bool instrumentIndirectCall(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc & DL,const TargetInstrInfo * TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        bool instrumentIndirectCallwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc & DL,const TargetInstrInfo * TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        //记录并哈希计算indirect jump的源地址和目标地址
        bool instrumentIndirectjump(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc & DL,const TargetInstrInfo * TII);
        bool instrumentRetLR_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        //记录并哈希计算ret指令的源地址和目标地址，ret inst ,ret [XR]，xr默认为lr
        //bool instrumentRet(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &Dl,const TargetInstrInfo *TII);
        
        //记录并哈希计算ret指令的源地址和目标地址，ret lr
        bool instrumentRetLR(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        //负责处理instrumentxxx函数，instrumentXXX会调用这个函数
        //bool handleControlTransfer(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        bool instrumentLDMIA_RET_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII,SmallVector<MachineInstr*,500>& to_delete_instr);
        bool instrumentIndirectJumpPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectCallPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentIndirectCallPredwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentDirectBranchPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentBL_pred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentBL(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);
        bool instrumentOnceCalledFuncLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII);
        bool UnrollLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII);

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
        static int dummy_branch_flag;

        ARMCollectIndirectJump(): MachineFunctionPass(ID){initializeARMCollectIndirectJumpPass(*PassRegistry::getPassRegistry());}
        bool runOnMachineFunction(MachineFunction &MF) override;
        StringRef getPassName()const override{
            return "arm collect indirect jump";
        }

    };

}//end llvm namespace
#endif