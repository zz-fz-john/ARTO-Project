//为了避免引入不必要的控制流，所以需要将该pass写在llvm后端，操纵arm汇编指令，来插入indirectjump的记录指令
//参考OAT的实现
//保留寄存器R8
//#define SHADOW_STACK
#include "ARM.h"
#include "ARMInstrInfo.h"
#include "ARMSubtarget.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include "collect_indirectJump.h"
#include <fstream>
#include<algorithm>
#include <iostream>
// =*= indirect jmp =*=
// before insert check
//      L1: bx xA
// after insert check
//          push R0 
//          mov R0, RA
//          bl __control_flow_check /* param0:target_addr, src_addr could be get from lr reg */
//          pop x0 
//      L1: br xA
//
// =*= indirect call =*=
// before insert check
//      L1: blx xA
// after insert check
//          push r0 
//          mov r0, rA
//          bl __control_flow_check /* param0:target_addr, src_addr could be get from lr reg */
//          pop r0 
//      L1: blr rA
//
//=*= ret lr=*=
//before insert check 
//      L1: pop pc
//          bx lr
//        
//after insert check
//          push  r0
//          mov r0,lr
//          bl __control_flow_check /* param0:target_addr, src_addr could be get from lr reg */
//          pop r0
//      L1: pop pc
//          ret lr
//
//===----------------------------------------------------------------------===//
using namespace llvm;
#define DEBUG_TYPE "ARM-collect-indirect-jump"
char ARMCollectIndirectJump::ID=0;
int ARMCollectIndirectJump::flag=0;
int ARMCollectIndirectJump::count=0;
int ARMCollectIndirectJump::inCallID=0;
int ARMCollectIndirectJump::dummy_flag=0;
int ARMCollectIndirectJump::dummy_branch_flag=0;
#define ARM_COLLECT_INDIRECT_CALL_NAME "ARM Collect Indirect Jump"
INITIALIZE_PASS(ARMCollectIndirectJump,DEBUG_TYPE,ARM_COLLECT_INDIRECT_CALL_NAME,false,false)
//记录并哈希计算indirect call 指令的源地址和目的地址
bool ARMCollectIndirectJump ::instrumentIndirectCall(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc & DL,const TargetInstrInfo * TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{
    unsigned targetReg;
    const char * sym="__collect__icall";
    
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器Rn，Rn可能是R0、R2...
    targetReg=MI.getOperand(0).getReg();
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // //push lr
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::LR,RegState::Define)
    //         .addReg(ARM::LR)//useless
    //         .addReg(ARM::SP)
    //         .addImm(-4)//pre offset
    //         .addImm(14);//conditional codes
    //push r0
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);
    //mov r0 ,rn
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(targetReg)
            .addImm(14)
            .addImm(0)
            .addImm(0);
    MachineBasicBlock::iterator nextInst=std::next(MI.getIterator());
    int num=ARMCollectIndirectJump::inCallID;
    //bl __collect__icall
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    //pop r0
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    //mov r8,0
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
    .addReg(ARM::R8)
    .addImm(0)
    .addImm(14)//conditional code
    .addImm(0)
    .addImm(0);
    //bfi r8,rn,24,0
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(targetReg)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //blx r8
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BLX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
    MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::MOVi16))
    .addReg(ARM::R8)
    .addImm(num&0xffff)
    .addImm(14)
    .addImm(0);
    return true;
}
bool ARMCollectIndirectJump::instrumentIndirectCallwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc & DL,const TargetInstrInfo * TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{
        unsigned targetReg;
    const char * sym="__collect__icall_shadow_stack";
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器Rn，Rn可能是R0、R2...
    targetReg=MI.getOperand(0).getReg();
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // //push lr
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::LR,RegState::Define)
    //         .addReg(ARM::LR)//useless
    //         .addReg(ARM::SP)
    //         .addImm(-4)//pre offset
    //         .addImm(14);//conditional codes
    //push r0
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);
    //mov r0 ,rn
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(targetReg)
            .addImm(14)
            .addImm(0)
            .addImm(0);
    MachineBasicBlock::iterator nextInst=std::next(MI.getIterator());
    int num=ARMCollectIndirectJump::inCallID;
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    //pop r0
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);

    //mov r8,0
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
    .addReg(ARM::R8)
    .addImm(0)
    .addImm(14)//conditional code
    .addImm(0)
    .addImm(0);
    //bfi r8,target_reg,24,0
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(targetReg)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //blx r8
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BLX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
    MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::MOVi16))
        .addReg(ARM::R8)
        .addImm(num&0xffff)
        .addImm(14)
        .addImm(0);
    return true;
}
void ARMCollectIndirectJump::initProcessFunction(){
    std::string filename="./ToInsertFunc.txt";
    //LibcName=ReadLibcFuncInfile(filename);
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";

    }
    std::string line;
    while (std::getline(file,line))
    {
        ToprocessFunction.push_back(line);
    }
    return ;
}
void ARMCollectIndirectJump::initOnlycalledOnceFunction(){
    std::string filename="./only_called_once_func_backup.txt";
    std::ifstream file(filename);
    if (!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    std::string line;
    while (std::getline(file,line))
    {
        std::string func_name=line.substr(0,line.find("-"));
        OnlyCalledOnceFunc.push_back(func_name);
    }
    return ;

}

//记录并哈希计算indirect jump的源地址和目标地址
bool ARMCollectIndirectJump::instrumentIndirectjump(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo*TII)
{
    unsigned targetReg;
    const char *sym="__collect__ijump";
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器rn，rn可能是r1、r2...
    targetReg=MI.getOperand(0).getReg();
    //push lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::LR,RegState::Define)
            .addReg(ARM::LR)//useless
            .addReg(ARM::SP)
            .addImm(-4)//pre offset
            .addImm(14);//conditional codes
    //push r0
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);
    //mov r0,rn
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(targetReg)
            .addImm(14)
            .addImm(0)
            .addImm(0);
    //bl __collect__ijump
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    //pop r0
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::LR,RegState::Define)
        .addReg(ARM::LR,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    return true;

}

// //记录并哈希计算ret指令的源地址和目标地址，ret inst ,ret [XR]，xr默认为lr
// bool ARMCollectIndirectJump ::instrumentRet(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &Dl,const TargetInstrInfo *TII);
// {
//     unsigned targetReg;
//     const char *sym="__"
//     DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
//     DEBUG(MI.print(dbgs()));//输出当前处理的指令
//     //获取 目标寄存器xR，xR可能是x1、x2...
//     targetReg=MI.getOperand(0).getReg();
//     return handleControlTransfer(MBB,MI,DL,TII,sym,targetReg);
// }
//记录并哈希计算ret指令的源地址和目标地址，bx lr
bool ARMCollectIndirectJump ::instrumentRetLR(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{
    const char *sym="__collect_bx_ret";
    unsigned targetReg;
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器rn，rn可能是r1、r2...
    targetReg=ARM::LR;
    
    //push lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::LR,RegState::Define)
            .addReg(ARM::LR)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);   
    //push r0
   MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);
    //mov r0,lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(targetReg)
            .addImm(14)
            .addImm(0)
            .addImm(0);
    //bl __collect_bx_ret
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    //pop r0
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    //pop lr
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
    .addReg(ARM::LR,RegState::Define)
    .addReg(ARM::LR,RegState::Define)
    .addReg(ARM::SP,RegState::Define)
    .addImm(0)
    .addImm(4)
    .addImm(14);
    //bfi r8,lr,24,0
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::LR)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //bx r8
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
    return true;
}
bool ARMCollectIndirectJump ::instrumentRetLR_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{
    const char *sym="__collect_bx_ret_shadow_stack";
    unsigned targetReg;
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器rn，rn可能是r1、r2...
    targetReg=ARM::LR;
    //mov r8,lr
    // BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::LR,RegState::Define)
    //         .addReg(ARM::LR)//useless
    //         .addReg(ARM::SP)
    //         .addImm(-4)//pre offset
    //         .addImm(14);//conditional codes
    //push lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::LR,RegState::Define)
            .addReg(ARM::LR)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);   
    //push r0
   MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14);
    //mov r0,lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(targetReg)
            .addImm(14)
            .addImm(0)
            .addImm(0);
    //bl __collect_bx_ret_shadow_stack
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    //pop r0
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    //pop lr
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
    .addReg(ARM::LR,RegState::Define)
    .addReg(ARM::LR,RegState::Define)
    .addReg(ARM::SP,RegState::Define)
    .addImm(0)
    .addImm(4)
    .addImm(14);

    //mov r8,0
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
    .addReg(ARM::R8)
    .addImm(0)
    .addImm(14)//conditional code
    .addImm(0)
    .addImm(0);
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::LR)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //bx r8
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
    return true;
}

bool ARMCollectIndirectJump ::instrumentIndirectCallPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    unsigned targetReg;
    targetReg=MI.getOperand(0).getReg();
    MachineInstr *BMI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // //push lr
    // BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::LR,RegState::Define)
    //     .addReg(ARM::LR)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14); 
    //push r0
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);    
    //push r1
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R1,RegState::Define)
        .addReg(ARM::R1)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);   
    //push r2
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R2,RegState::Define)
        .addReg(ARM::R2)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14); 
    //push r3
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R3,RegState::Define)
        .addReg(ARM::R3)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);     
    //mov r0, targetReg
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
        .addReg(ARM::R0)
        .addReg(targetReg)
        .addImm(14)
        .addImm(0)
        .addImm(0);

    //move cpsr to r1
    BMI=BuildMI(MBB, MI,DL, TII->get(ARM::MRS), ARM::R1)
            .addImm(14);
    //move conditional type to r2
    unsigned condcode =MI.getOperand(1).getImm();
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R2)
        .addImm(condcode)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //jump trampoline
    const char * sym="__collect_indirect_call_pred";
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    // //restore r3
    // //相当于pop R3
    // //ldr R3,[sp]
    // //sp=sp+4
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
            .addReg(ARM::R3,RegState::Define)
            .addReg(ARM::R3,RegState::Define)
            .addReg(ARM::SP,RegState::Define)
            .addImm(0)//useless
            .addImm(4)
            .addImm(14);//conditional code
    // //restore r2
    // //相当于pop R2
    // //ldr R2,[sp]
    // //sp=sp+4
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
            .addReg(ARM::R2,RegState::Define)
            .addReg(ARM::R2,RegState::Define)
            .addReg(ARM::SP,RegState::Define)
            .addImm(0)//useless
            .addImm(4)
            .addImm(14);//conditional code
    // //restore r1
    // //相当于pop R1
    // //ldr R1,[sp]
    // //sp=sp+4
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
            .addReg(ARM::R1,RegState::Define)
            .addReg(ARM::R1,RegState::Define)
            .addReg(ARM::SP,RegState::Define)
            .addImm(0)//useless
            .addImm(4)
            .addImm(14);//conditional code
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)//useless
        .addImm(4)
        .addImm(14);//conditional code
    return true;
}
bool ARMCollectIndirectJump::instrumentIndirectCallPredwithShadowStack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    unsigned targetReg;
    targetReg=MI.getOperand(0).getReg();
    MachineInstr *BMI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // //push lr
    // BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::LR,RegState::Define)
    //     .addReg(ARM::LR)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14); 
    //push r0
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);    
    //push r1
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R1,RegState::Define)
        .addReg(ARM::R1)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);   
    //push r2
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R2,RegState::Define)
        .addReg(ARM::R2)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14); 
    //push r3
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R3,RegState::Define)
        .addReg(ARM::R3)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);     
    //mov r0, targetReg
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
        .addReg(ARM::R0)
        .addReg(targetReg)
        .addImm(14)
        .addImm(0)
        .addImm(0);
    //move cpsr to r1
    BMI=BuildMI(MBB, MI,DL, TII->get(ARM::MRS), ARM::R1)
            .addImm(14);
    //move conditional type to r2
    unsigned condcode =MI.getOperand(1).getImm();
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R2)
        .addImm(condcode)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //jump trampoline
    const char * sym="__collect_indirect_call_pred_shadow_stack";
    BMI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
}

bool ARMCollectIndirectJump::instrumentDirectBranchPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // Push LR (关键！防止 BL 覆盖 LR 导致无法返回)
    // BuildMI(MBB, MI, DL, TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::LR, RegState::Define)
    //     .addReg(ARM::LR)
    //     .addReg(ARM::SP)
    //     .addImm(-4).addImm(14).addReg(0);   
    // //push r0
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::R0,RegState::Define)
    //     .addReg(ARM::R0)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14);    
    // //push r1
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::R1,RegState::Define)
    //     .addReg(ARM::R1)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14);   
    BuildMI(MBB, MI, DL, TII->get(ARM::STMDB_UPD))
        .addReg(ARM::SP, RegState::Define) // 1. SP Write-back (更新后的 SP)
        .addReg(ARM::SP)                   // 2. Base Register (当前的 SP)
        .addImm(14).addReg(0)              // 3. Predicate (Always)
        // --- 下面是寄存器列表 (Variable Operands) ---
        .addReg(ARM::R0)                   // Push R0
        .addReg(ARM::R1)                   // Push R1
        .addReg(ARM::LR);                  // Push LR
    // //push r2
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::R2,RegState::Define)
    //     .addReg(ARM::R2)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14); 
    
    //move cpsr to r0
    MBI=BuildMI(MBB, MI,DL, TII->get(ARM::MRS), ARM::R0)
            .addImm(14);
    int PIdx = MI.findFirstPredOperandIdx();

    // 2. 确保找到了条件码 (有些指令可能没有)
    unsigned condcode;
    if (PIdx != -1) {
        condcode = MI.getOperand(PIdx).getImm();
    } else {
        // 如果指令是无条件的 (AL)，或者不是标准 predicated 指令
        condcode = ARMCC::AL; 
    }
    //move conditional type to r1
    //unsigned condcode = MI.getOperand(MI.getNumOperands()-1).getImm();
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R1)
        .addImm(condcode)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    
    //jump trampoline
    const char * sym="__collect__conditional_branch_pred";
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    
    // //restore r2
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
    //         .addReg(ARM::R2,RegState::Define)
    //         .addReg(ARM::R2,RegState::Define)
    //         .addReg(ARM::SP,RegState::Define)
    //         .addImm(0)
    //         .addImm(4)
    //         .addImm(14);
    BuildMI(MBB, MI, DL, TII->get(ARM::LDMIA_UPD))
        .addReg(ARM::SP, RegState::Define) // 1. SP Write-back
        .addReg(ARM::SP)                   // 2. Base Register
        .addImm(14).addReg(0)              // 3. Predicate
        // --- 下面是寄存器列表 (Def) ---
        .addReg(ARM::R0, RegState::Define) // Pop 到 R0
        .addReg(ARM::R1, RegState::Define) // Pop 到 R1
        .addReg(ARM::LR, RegState::Define);// Pop 到 LR
    //restore r1
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
    //         .addReg(ARM::R1,RegState::Define)
    //         .addReg(ARM::R1,RegState::Define)
    //         .addReg(ARM::SP,RegState::Define)
    //         .addImm(0)
    //         .addImm(4)
    //         .addImm(14);
    // //restore r0
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
    //     .addReg(ARM::R0,RegState::Define)
    //     .addReg(ARM::R0,RegState::Define)
    //     .addReg(ARM::SP,RegState::Define)
    //     .addImm(0)
    //     .addImm(4)
    //     .addImm(14);
    // //restore lr
    // BuildMI(MBB, MI, DL, TII->get(ARM::LDR_POST_IMM), ARM::LR)
    //     .addReg(ARM::SP, RegState::Define).addReg(ARM::SP)
    //     .addImm(0).addImm(4).addImm(14).addReg(0);
    return true;
}

bool ARMCollectIndirectJump::instrumentIndirectJumpPred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    unsigned targetReg;
    targetReg=MI.getOperand(0).getReg();
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    //push lr
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::LR,RegState::Define)
        .addReg(ARM::LR)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14); 
    //push r0
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);    
    //push r1
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R1,RegState::Define)
        .addReg(ARM::R1)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);   
    //push r2
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R2,RegState::Define)
        .addReg(ARM::R2)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14); 
    //push r3
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R3,RegState::Define)
        .addReg(ARM::R3)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);
    //mov r0,targetReg
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVr))
        .addReg(ARM::R0)
        .addReg(targetReg)
        .addImm(14)
        .addImm(0)
        .addImm(0); 
    //move cpsr to r1
    MBI=BuildMI(MBB, MI,DL, TII->get(ARM::MRS), ARM::R1)
            .addImm(14);
    //move conditional type to r2
    unsigned condcode =MI.getOperand(1).getImm();
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R2)
        .addReg(condcode)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //jump trampoline
    const char * sym="__collect_indirect_jump_pred";
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    return true;

}
bool ARMCollectIndirectJump:: instrumentLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{   
    ARMCollectIndirectJump::flag=1;
    unsigned num_operands=MI.getNumOperands();
    for(int i=0;i<num_operands;i++)
    {
        if(MI.getOperand(i).isReg()&&MI.getOperand(i).getReg()==ARM::PC)
        {
            MI.removeOperand(i);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)
                .addImm(4)
                .addImm(14);
            const char * sym="__collect_ldmia_ret";
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BL)).addExternalSymbol(sym);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14); 
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(ARM::R8)
            .addImm(14)
            .addImm(0)
            .addImm(0);
            //mov r8,0
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::MOVi))
                .addReg(ARM::R8)
                .addImm(0)
                .addImm(14)//conditional code
                .addImm(0)
                .addImm(0);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BFI))
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R0)
                .addImm(~((1<<24)-1))
                .addImm(14);
           // to_delete_instr.push_back(&MI);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::R0,RegState::Define)
                .addReg(ARM::R0,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)//useless
                .addImm(4)
                .addImm(14);//conditional code
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BX)).addReg(ARM::R8);
            break;
        }
    }
    return true;
}
bool ARMCollectIndirectJump:: instrumentLDMIA_RET_shadow_stack(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII,SmallVector<MachineInstr*,500>& to_delete_instr)
{
    ARMCollectIndirectJump::flag=1;
    unsigned num_operands=MI.getNumOperands();
    for(int i=0;i<num_operands;i++)
    {
        if(MI.getOperand(i).isReg()&&MI.getOperand(i).getReg()==ARM::PC)
        {
            MI.removeOperand(i);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)
                .addImm(4)
                .addImm(14);
            const char * sym="__collect_ldmia_ret_shadow_stack";
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BL)).addExternalSymbol(sym);
            //push r0
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::STR_PRE_IMM))
            .addReg(ARM::R0,RegState::Define)
            .addReg(ARM::R0)
            .addReg(ARM::SP)
            .addImm(-4)
            .addImm(14); 
            //mov r0,r8
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::MOVr))
            .addReg(ARM::R0)
            .addReg(ARM::R8)
            .addImm(14)
            .addImm(0)
            .addImm(0);
            //mov r8,0
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::MOVi))
                .addReg(ARM::R8)
                .addImm(0)
                .addImm(14)//conditional code
                .addImm(0)
                .addImm(0);
            //bfi r8,r0,24,0
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BFI))
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R0)
                .addImm(~((1<<24)-1))
                .addImm(14);
            //to_delete_instr.push_back(&MI);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::R0,RegState::Define)
                .addReg(ARM::R0,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)//useless
                .addImm(4)
                .addImm(14);//conditional code
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BX)).addReg(ARM::R8);
            break;
        }
    }
    return true;
}
bool ARMCollectIndirectJump::instrumentOnceCalledFuncLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc& DL,const TargetInstrInfo* TII)
{
    ARMCollectIndirectJump::flag=1;
    unsigned num_operands=MI.getNumOperands();
    for(int i=0;i<num_operands;i++)
    {
        if(MI.getOperand(i).isReg()&&MI.getOperand(i).getReg()==ARM::PC)
        {
            MI.removeOperand(i);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::R8,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)
                .addImm(4)
                .addImm(14);

            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::BX)).addReg(ARM::R8);
            break;
        }
    }
    return true;
}

bool ARMCollectIndirectJump::instrumentBL_pred(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII){
    //unsigned targetReg;
    //targetReg=MI.getOperand(0).getReg();
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');
    // //push lr
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //     .addReg(ARM::LR,RegState::Define)
    //     .addReg(ARM::LR)
    //     .addReg(ARM::SP)
    //     .addImm(-4)
    //     .addImm(14); 
    //push r0
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R0,RegState::Define)
        .addReg(ARM::R0)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);    
    //push r1
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R1,RegState::Define)
        .addReg(ARM::R1)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);   
    //push r2
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R2,RegState::Define)
        .addReg(ARM::R2)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14); 
    //push r3
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
        .addReg(ARM::R3,RegState::Define)
        .addReg(ARM::R3)
        .addReg(ARM::SP)
        .addImm(-4)
        .addImm(14);
    // //mov r0,targetaddress
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
    //     .addReg(ARM::R0)
    //     .addGlobalAddress(MI.getOperand(0).getGlobal(),0,1)
    //     .addImm(14)
    //     .addImm(0); 
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVTi16),ARM::R0)
    //     .addReg(ARM::R0)
    //     .addGlobalAddress(MI.getOperand(0).getGlobal(),0,2)
    //     .addImm(14)
    //     .addReg(0);
    
    //move cpsr to r0
    MBI=BuildMI(MBB, MI,DL, TII->get(ARM::MRS), ARM::R0)
            .addImm(14);
    //move conditional type to r1
    unsigned condcode =MI.getOperand(1).getImm();
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R1)
        .addReg(condcode)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //jump trampoline
    const char * sym="__collect_direct_call_pred";
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    return true;
}
bool ARMCollectIndirectJump ::delete_dummy_code(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,std::string calledFuncName,std::vector<MachineInstr *> &DelInstVect){
    int num;
    //calledFuncName=“llvm_Indirectdummy_0”,识别出后面的数字来
    size_t pos=calledFuncName.find_last_of("_");
    if(pos!=std::string::npos)
    {
        std::string numStr=calledFuncName.substr(pos+1);
        num=std::stoi(numStr);
        ARMCollectIndirectJump ::inCallID=num;
        std::cout<<num<<std::endl;
        // MachineInstr *MBI;
        // // MachineInstr *MIB=&MI;
        // // DelInstVect.push_back(MIB);
        // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
        // .addReg(ARM::R8)
        // .addImm(num&0xffff)
        // .addImm(14)
        // .addImm(0);
    }
    return true;



}
bool ARMCollectIndirectJump::instrumentBL(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    //unsigned targetReg;
    const char *sym="__collect_direct_call";
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    //获取 目标寄存器rn，rn可能是r1、r2...
    //targetReg=MI.getOperand(0).getReg();
    // //push lr
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::LR,RegState::Define)
    //         .addReg(ARM::LR)//useless
    //         .addReg(ARM::SP)
    //         .addImm(-4)//pre offset
    //         .addImm(14);//conditional codes
    // //push r0
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::R0,RegState::Define)
    //         .addReg(ARM::R0)
    //         .addReg(ARM::SP)
    //         .addImm(-4)
    //         .addImm(14);
    // //mov r0,targetaddress
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
    //     .addReg(ARM::R0)
    //     .addGlobalAddress(MI.getOperand(0).getGlobal(),0,1)
    //     .addImm(14)
    //     .addImm(0); 
    // MBI=BuildMI(MBB,MI,DL,TII->get(ARM::MOVTi16),ARM::R0)
    //     .addReg(ARM::R0)
    //     .addGlobalAddress(MI.getOperand(0).getGlobal(),0,2)
    //     .addImm(14)
    //     .addReg(0);
    MBI=BuildMI(MBB,MI,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    return true;
}
void ARMCollectIndirectJump ::initCheckpointFunc()
{
    std::string filename="./checkpoint_function_backup.txt";
    std::ifstream file(filename);
    if(!file.is_open())
    {
        errs()<<"could not open file"<<filename<<"\n";
    }
    std::string line;
    while(std::getline(file,line))
    {
        CheckpointFunc.push_back(line);
    }
}
bool ARMCollectIndirectJump::UnrollLDMIA_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    unsigned num_operands=MI.getNumOperands();
    //errs()<<MI<<'\n';
    for(int i=0;i<num_operands;i++)
    {
        //errs()<<MI.getOperand(i).getReg()<<'\n';
        if(MI.getOperand(i).isReg()&&MI.getOperand(i).getReg()==ARM::PC)
        {
            //errs()<<"run delete operand "<<'\n';
            MI.removeOperand(i);
            BuildMI(MBB,MBB.end(),DL,TII->get(ARM::LDR_POST_IMM))
                .addReg(ARM::PC,RegState::Define)
                .addReg(ARM::PC,RegState::Define)
                .addReg(ARM::SP,RegState::Define)
                .addImm(0)
                .addImm(4)
                .addImm(14);
            break;
        }
    }
    return true;
}
// bool ARMCollectIndirectJump::UnrollLDMIA_RET(MachineBasicBlock &MBB, MachineInstr &MI, const DebugLoc &DL, const TargetInstrInfo *TII) {
//     std::vector<int> toRemove;
//     unsigned num_operands = MI.getNumOperands();

//     // 1. 先记录要删除的操作数索引
//     for (int i = num_operands - 1; i >= 0; i--) {  // 从后向前遍历
//         if (MI.getOperand(i).isReg() && MI.getOperand(i).getReg() == ARM::PC) {
//             toRemove.push_back(i);
//         }
//     }

//     // 2. 删除操作数
//     for (int i : toRemove) {
//         MI.removeOperand(i);
//     }

//     // 3. 在 MI 之后插入新的 LDR_POST_IMM 指令
//     MachineInstr *NewInstr = BuildMI(MBB, std::next(MI.getIterator()), DL, TII->get(ARM::LDR_POST_IMM))
//                                  .addReg(ARM::PC, RegState::Define)   // 定义 PC
//                                  .addReg(ARM::PC,RegState::Define)
//                                  .addReg(ARM::SP, RegState::Define)     // SP 作为 source
//                                  .addImm(0)                            // Offset (通常为0)
//                                  .addImm(4)                            // Post-increment
//                                  .addImm(14);                          // Mode (通常为 14)

//     // 4. 打印调试信息，检查是否执行
//     errs() << "Inserted LDR_POST_IMM after: " << MI << "\n";
//     errs() << "New instruction: " << *NewInstr << "\n";

//     return true;
// }

bool ARMCollectIndirectJump ::runOnMachineFunction(MachineFunction &MF) 
{
    bool Madechange =false;
    errs()<<"brefor run ARM collect indirect jump on func :"<<MF.getName()<<'\n';
    if(ARMCollectIndirectJump::count==0)
    {
        const std::string AvoidHandleFuncNamefilename="./avoid_handle_function.txt";
        std::ifstream file("./avoid_handle_function.txt");
        if(!file.is_open())
        {
            errs()<<"could not open file"<<AvoidHandleFuncNamefilename<<"\n";
        }
        std::string line;
        while(std::getline(file,line))
        {
            AvoidHandleFuncName.push_back(line);
        }
        AvoidHandleFuncName.push_back("_Z16start_collectingv");
        AvoidHandleFuncName.push_back("_Z14end_collectingv");
        AvoidHandleFuncName.push_back("_Z11recordpointv");
        AvoidHandleFuncName.push_back("_Z16loop_recordpointv");
        AvoidHandleFuncName.push_back("_Z21recursive_recordpointv");
        AvoidHandleFuncName.push_back("end_collecting");
        AvoidHandleFuncName.push_back("start_collecting");
        AvoidHandleFuncName.push_back("recordpoint");
        AvoidHandleFuncName.push_back("_Z15recursive_latchv");

        AvoidHandleFuncName.push_back("start_collecting");
        AvoidHandleFuncName.push_back("end_collecting");
        AvoidHandleFuncName.push_back("recordpoint");
        AvoidHandleFuncName.push_back("loop_recordpoint");
        AvoidHandleFuncName.push_back("recursive_recordpoint");
        AvoidHandleFuncName.push_back("recursive_latch");
        AvoidHandleFuncName.push_back("use_check");
        AvoidHandleFuncName.push_back("def_check");
        //AvoidHandleFuncName.push_back("main");
        AvoidHandleFuncName.push_back("def_collect");
        AvoidHandleFuncName.push_back("Critical_def_check");
        AvoidHandleFuncName.push_back("non_sen_def_check");
        AvoidHandleFuncName.push_back("use_check_for_basic_type_in_struct");
        AvoidHandleFuncName.push_back("def_check_for_basic_type_in_struct");
        AvoidHandleFuncName.push_back("def_collect_for_float");
        AvoidHandleFuncName.push_back("Critical_def_check_for_float");
        AvoidHandleFuncName.push_back("checkpoint");
        AvoidHandleFuncName.push_back("use_check_for_ptr_in_struct");

        initProcessFunction();
        initOnlycalledOnceFunction();
        initCheckpointFunc();
        ARMCollectIndirectJump::count+=1;
    }
    const TargetInstrInfo*TII=MF.getSubtarget().getInstrInfo();
    std::string funcName=std::string(MF.getName());
    dummy_flag=0;
    dummy_branch_flag=0;
    const Function*F=&MF.getFunction();
    if (llvm::DISubprogram *SP = F->getSubprogram()) {
        if (llvm::DIFile *File = SP->getFile()) {
            llvm::StringRef Filename = File->getFilename();
            llvm::StringRef Directory = File->getDirectory();
            if (Filename.contains("arm-linux-gnueabihf")) {
                std::cout<<"run arm-linux-gnueabihf"<<F->getName().str()<<std::endl;
                return false;
                
            }
        }
    }
    if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),funcName)!=AvoidHandleFuncName.end())
    {
        return false;
    }
            
    // if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),funcName)==AvoidHandleFuncName.end())//不对libc函数的内部逻辑进行处理
    // {
        if(std::find(ToprocessFunction.begin(),ToprocessFunction.end(),funcName)!=ToprocessFunction.end()){
            //errs()<<funcName<<'\n';
            for(MachineFunction::iterator FI=MF.begin();FI!=MF.end();++FI)
            {    
                std::vector<MachineInstr *> DelInstVect;
                SmallVector<MachineInstr*,500> to_delete_instr;
                MachineBasicBlock &MBB=*FI;
                //遍历基本块中的所有指令
                for(MachineBasicBlock::iterator I=MBB.begin();I!=MBB.end();++I)
                {
                    MachineInstr &MI=*I;
                    if(MI.getDesc().isCall()||
                    MI.getDesc().isIndirectBranch()||
                    MI.getDesc().isReturn()||MI.getOpcode()==ARM::Bcc)
                    {   
                        
                        switch(MI.getOpcode()){
                            // 在 switch 之前
                            //LLVM_DEBUG(dbgs() << "Checking Instr: " << TII->getName(MI.getOpcode()) << "\n");   
                            case ARM::Bcc:
                            {
                                if(dummy_branch_flag==1)
                                {
                                    dummy_branch_flag=0;
                                    break;
                                }
                                #ifdef SHADOW_STACK
                                    break;//不处理有条件分支指令
                                #endif
                                llvm::errs()<<"ARM::Bcc \n";
                                llvm::Register PredReg = 0;
                                 llvm::ARMCC::CondCodes precode=getInstrPredicate(MI,PredReg);
                                if(precode==llvm::ARMCC::AL)
                                {
                                    break; 
                                }
                                else{
                                    Madechange|=instrumentDirectBranchPred(MBB,MI,MI.getDebugLoc(),TII);
                                    break;
                                }
                            }
                            case ARM::BLX://indirect call
                                errs()<<"ARM::BLX \n";
                                #ifndef SHADOW_STACK
                                if(dummy_flag==0)
                                {
                                #endif
                                    Madechange|=instrumentIndirectCall(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                    break;
                                #ifndef SHADOW_STACK
                                }
                                else if(dummy_flag==1)
                                {
                                
                                    Madechange|=instrumentIndirectCallwithShadowStack(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                
                                    dummy_flag=0;
                                    break;
                                }
                                #endif
                                break;

                            case ARM::BX://indirect jump,相当于bx Rn，只有swich语句 会有这个指令，但switch语句已经被展开了，switch语句有另外一种形式，就是ldr pc,[r0,#0x0]，这种形式已经被处理了
                            //useless ，因为没有indirect jump了
                                errs()<<"ARm::BX\n";

                                if(ARMCollectIndirectJump::flag==0)
                                {
                                    Madechange|=instrumentIndirectjump(MBB,MI,MI.getDebugLoc(),TII);
                                    break;
                                }
                                else 
                                {
                                    ARMCollectIndirectJump::flag=0;
                                    break;
                                }

                            case ARM::BLX_pred://useless
                                errs()<<"ARM::BLX_pred\n";
                                if(dummy_flag==0)
                                {
                                    Madechange|=instrumentIndirectCallPred(MBB,MI,MI.getDebugLoc(),TII);

                                }
                                else if (dummy_flag==1)
                                {
                                    Madechange|=instrumentIndirectCallPredwithShadowStack(MBB,MI,MI.getDebugLoc(),TII);
                                    dummy_flag=0;
                                    break;
                                }
                                
                                break;
                            case ARM::BX_pred://useless，因为没有indirect jump了
                                errs()<<"ARM::BX_pred\n";
                                if(!MI.getOperand(0).isReg())
                                {
                                    break;
                                }
                                Madechange|=instrumentIndirectJumpPred(MBB,MI,MI.getDebugLoc(),TII);
                                break;

                            case ARM::BX_RET://return bx lr
                                errs()<<"ARM::BX_RET\n";     
                                //该叶函数在checkpointfunc 集合内部，但实际上该函数内部没有调用checkpoint ，只不过某个间接调用指令的目标集合内有个函数调用了checkpoint，该间接调用指令需要插入shadow stack，其实是在insert_dummy的扩展过程中，将该函数扩展进来，所以需要处理。                           
                                // if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),funcName)!=CheckpointFunc.end())
                                // {
                                //     break;
                                //     Madechange|=instrumentRetLR_shadow_stack(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                    
                                // }
                                // //实际上不可能，因为先前只被调用一次的函数且是叶子的函数已经被插入了checkpoint，所以不再是叶子函数，也就意味着不会有bx lr指令，所以该分支不会走。
                                // if(std::find(OnlyCalledOnceFunc.begin(),OnlyCalledOnceFunc.end(),funcName)!=OnlyCalledOnceFunc.end())
                                // {
                                //     // MadeChange|=instrumentOnceCalledFuncRetLR(MBB,MI,MI.getDebugLoc(),TII);
                                //     break;
                                // }
                                #ifndef SHADOW_STACK
                                    break;//不需要对该指令再做处理了，因为根据调用约定，这种类型的返回地址不会被劫持
                                #endif
                                Madechange|=instrumentRetLR(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                break;
                            case ARM::LDMIA_RET:
                                errs()<<"ARM::LDMIA_RET\n";
                                #ifdef SHADOW_STACK
                                    if(funcName=="main")
                                        break;
                                    Madechange|=instrumentLDMIA_RET(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                    break;
                                #endif
                                //只被调用一次的函数的返回地址会被写死。onlyCalledOnceFunc集合与Checkpointfunc集合是互斥的，不会有交集。
                                if(std::find(OnlyCalledOnceFunc.begin(),OnlyCalledOnceFunc.end(),funcName)!=OnlyCalledOnceFunc.end())
                                {   
                                    
                                    Madechange|=instrumentOnceCalledFuncLDMIA_RET(MBB,MI,MI.getDebugLoc(),TII);
                                    break;
                                }
                                // if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),funcName)!=CheckpointFunc.end())//该函数内部调用了checkpoint，或者是checkpointfunc集合内函数，则需要使用shadow stack 保护。
                                // {
                                //     Madechange|=instrumentLDMIA_RET_shadow_stack(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                //     break;
                                // }
                                if(funcName=="main")
                                    break;
                                Madechange|=instrumentLDMIA_RET(MBB,MI,MI.getDebugLoc(),TII,to_delete_instr);
                                break;
                    
                        
                            case ARM::BL:
                            {
                                const MachineOperand &operand=MI.getOperand(0);


                                if(operand.isGlobal()){
                                    const GlobalValue * GV=operand.getGlobal();
                                    const Function *F =dyn_cast<Function> (GV);
                                    //errs()<<F->getName()<<'\n';
                                    if(F)
                                    {
                                        StringRef functionName=F->getName();
                                        std::string callfuncname=std::string(functionName);
                                        //errs()<<callfuncname<<'\n';
                                        std::string prefix="llvm_Indirectdummy";
                                        if(callfuncname.find(prefix)!=std::string::npos)//包含该前缀
                                        {   
                                            MachineInstr *MIB=&*I;
                                            DelInstVect.emplace_back(&*I);
                                            errs()<<"ARM::BL llvm_dummy  "<< callfuncname<<'\n';
                                            Madechange|=delete_dummy_code(MBB,MI,MI.getDebugLoc(),TII,callfuncname,DelInstVect);
                                            //MBB.remove_instr(MIB);
                                            break;                                    
                                        }
                                        std::string dummy_branch="llvm_branch_dummy";
                                        if(callfuncname.find(dummy_branch)!=std::string::npos)
                                        {
                                            dummy_branch_flag=1;
                                            MachineInstr *MIB=&*I;
                                            DelInstVect.emplace_back(&*I);
                                            errs()<<"ARM::BL llvm_branch_dummy  "<< callfuncname<<'\n';
                                            break;                                    
                                        }
                                        #ifndef SHADOW_STACK
                                            break;//delete shadow stack
                                        
                                        std::string dummycode="llvm_CallCkDummy";
                                        if(callfuncname==dummycode)
                                        {
                                            dummy_flag=1;
                                            MachineInstr *MIB=&*I;
                                            DelInstVect.emplace_back(&*I);
                                            break;
                                        }
                                        #endif
                                        #ifndef SHADOW_STACK
                                        if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),callfuncname)!=CheckpointFunc.end())
                                        {
                                        #endif
                                            if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),callfuncname)!=AvoidHandleFuncName.end())
                                                break;
                                            errs()<<"ARM::BL "<<callfuncname<<'\n';
                                            Madechange|=instrumentBL(MBB,MI,MI.getDebugLoc(),TII);

                                            break;
                                        #ifndef SHADOW_STACK
                                        }
                                        else {
                                            break;
                                        }
                                        #endif
                                    }
                                    else{
                                        break;
                                    }
                                }
                                else if (operand.isSymbol()){
                                    // #ifndef SHADOW_STACK  
                                    break;//delete shadow stack
                                    // #endif
                                    StringRef functionName=operand.getSymbolName();
                                    std::string callfuncname=std::string(functionName); 
                                    #ifndef SHADOW_STACK                               
                                    if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),callfuncname)!=CheckpointFunc.end())
                                    {
                                    #endif
                                        if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),callfuncname)!=AvoidHandleFuncName.end())
                                            break;
                                        errs()<<"ARM::BL symbol "<<callfuncname<<'\n';
                                        Madechange|=instrumentBL(MBB,MI,MI.getDebugLoc(),TII);
                                        break;
                                    #ifndef SHADOW_STACK
                                    }
                                    else{
                                        break;
                                    }
                                    #endif
                                }
                                else{
                                    break;
                                }
                            }
                            case ARM::BL_pred://该命令的意思为bl func，func 为一个label 或者一个地址，不是间接跳转。
                            {
                                #ifndef SHADOW_STACK
                                    break;//delete shadow stack
                                #endif
                                const MachineOperand &operand=MI.getOperand(0);
                                if(operand.isGlobal()){
                                    const GlobalValue * GV=operand.getGlobal();
                                    const Function *F =dyn_cast<Function> (GV);
                                    if(F)
                                    {   
                                        llvm::Register PredReg = 0;
                                        llvm::ARMCC::CondCodes precode=getInstrPredicate(MI,PredReg);
                                        StringRef functionName=F->getName();
                                        std::string callfuncname=std::string(functionName);
                                        #ifndef SHADOW_STACK  
                                        if(std::find(CheckpointFunc.begin(),CheckpointFunc.end(),callfuncname)!=CheckpointFunc.end())
                                        {
                                        #endif
                                            if(precode==ARMCC::AL)
                                            {
                                                if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),callfuncname)!=AvoidHandleFuncName.end())
                                                    break;
                                                errs()<<"ARM::BL "<<callfuncname<<'\n';
                                                Madechange|=instrumentBL(MBB,MI,MI.getDebugLoc(),TII);
                                                break;
                                            }
                                            errs()<<"ARM::BL pred  "<<callfuncname<<'\n';
                                            Madechange|=instrumentBL_pred(MBB,MI,MI.getDebugLoc(),TII);
                                            break;
                                        #ifndef SHADOW_STACK
                                        }
                                        else{
                                            break;
                                        }
                                        #endif

                                    }
                                    else{
                                        break;
                                    }
                                }
                                else{
                                    break;
                                }
                            }

                            // case ARM::Bcc://该命令的意思为条件分支跳转，只有满足条件时，才进行跳转，相当于x86中的jne等指令，但该指令后只能跟一个func,func应该是一个label或者是一个地址，而不能是寄存器
                            //     //errs()<<"ARM::Bcc";
                            //     //unsigned PreReg=0;
                            //     //ARMCC::Concodes precode=getInstrPredicate(MI,PreCode,MBB,BBI,TII);
                            //     //Madechange|=instrumentIndirectCall_Bcc(MBB,MI,MI.getDebugLoc(),TII);
                            default:
                                break;

                        }

                    }
                }
                    //delete instructions to be deleted
                for (auto &DI:to_delete_instr){
                    MachineBasicBlock &MBB=*DI->getParent();
                    //errs()<<(*DI)->getOpcode()<<(*DI)->getOperand(0)<<"\n";
                    MBB.remove_instr(DI);                    
                    //errs()<<"run delete instruction"<<"\n";
                }
                    //delete instructions to be deleted
                for (auto DI=DelInstVect.begin();DI!=DelInstVect.end();++DI){
                    //errs()<<(*DI)->getOpcode()<<(*DI)->getOperand(0)<<"\n";
                    MBB.remove_instr(*DI);                    
                    //errs()<<"run delete instruction"<<"\n";
                }
            }
        }
        else{
            if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),funcName)!=AvoidHandleFuncName.end())
            {
                return false;
            }
            
            const Function*F=&MF.getFunction();
            std::string cur_func_name=F->getName().str();
            std::string str1="__cxx_global_var_init";
            std::string str2="_GLOBAL__sub_I_";
            std::string str3="_cxx";
            std::string str4="llvm";
        
            std::string ::size_type idx=cur_func_name.find(str1);
            if(F->isDeclaration())
            {
                return false;
            }   
            if(idx!=std::string::npos)
            {
                return false;
            }
            idx=cur_func_name.find(str2);
            if(idx!=std::string::npos)
            {
                return false;
            }
            idx=cur_func_name.find(str3);
            if(idx!=std::string::npos)
            {
                return false;
            }
            idx=cur_func_name.find(str4);
            if(idx!=std::string::npos)
            {
                return false;
            }
            if(F->getName().startswith("llvm."))
            {
                return false;
            }
            //将其他函数中的LDMIA_RET展开
            //errs()<<MF.getName()<<'\n';
            errs()<<" run ARM collect indirect jump on func :"<<MF.getName()<<'\n';
            for(MachineFunction::iterator FI=MF.begin();FI!=MF.end();++FI)
            {    
                MachineBasicBlock &MBB=*FI;
                for(MachineBasicBlock::iterator I=MBB.begin();I!=MBB.end();++I)
                {
                    MachineInstr &MI=*I;
                    //errs()<<"run 内部 loop"<<'\n';
                    if(MI.getOpcode()==ARM::LDMIA_RET)
                    {
                        //errs()<<"before run unrollLDMIA_RET"<<'\n';
                        Madechange|=UnrollLDMIA_RET(MBB,MI,MI.getDebugLoc(),TII);
                    }
                }
            }
           
        }

    return Madechange;
}

FunctionPass *llvm ::createARMCollectIndirectJumpPass(){

    return new ARMCollectIndirectJump();
}
// char MachineCountPass::ID=0;
// char ARMCollectIndirectJump::ID=0;
// static RegisterPass<ARMCollectIndirectJump> X("collectIndirectBranch","collect indirect branch");