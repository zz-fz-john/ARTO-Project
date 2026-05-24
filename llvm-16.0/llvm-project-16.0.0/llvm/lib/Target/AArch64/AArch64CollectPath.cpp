#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "AArch64TargetMachine.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include<algorithm>
#include <iostream>

#include "AArch64CollectPath.h"
using namespace llvm;
#define DEBUG_TYPE "AArch64-collect-path"
char AArch64CollectPath::ID=0;
int AArch64CollectPath::flag=0;
int AArch64CollectPath::count=0;
int AArch64CollectPath::inCallID=0;
int AArch64CollectPath::dummy_flag=0;
#define AARCH64_COLLECT_PATH_NAME "AArch64 Collect Path"
INITIALIZE_PASS(AArch64CollectPath,DEBUG_TYPE,AARCH64_COLLECT_PATH_NAME,false,false)

//记录并哈希计算indirect call 指令的源地址和目的地址
bool AArch64CollectPath::instrumentIndirectCall(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII)
{
    //unsigned targetReg;
    const char *sym="__collect_icall";
    MachineInstr *MBI;
    LLVM_DEBUG(dbgs()<<__func__<<'\n');//输出函数名称
    LLVM_DEBUG(MI.print(dbgs()));//输出当前处理的指令
    MBI=BuildMI(MBB,MI,DL,TII->get(AArch64::STPpre))
        .addReg(AArch64::X30, RegState::Kill)//链接寄存器
        .addReg(AArch64::XSP)//栈指针
        .addImm(-16);// 栈偏移量，AArch64 的 STP 保存两个寄存器时通常是 16 字节对齐
    //push X0(保存X0到栈中)
    MBI = BuildMI(MBB, MI, DL, TII->get(AArch64::STPpre))
          .addReg(AArch64::X0, RegState::Kill)
          .addReg(AArch64::XSP)
          .addImm(-16);
    // MOV X0, targetReg (将目标寄存器值移动到 X0)
    MBI = BuildMI(MBB, MI, DL, TII->get(AArch64::ORRrr))
          .addReg(AArch64::X0, RegState::Define) // 目标寄存器
          .addReg(targetReg) // 源寄存器
          .addReg(AArch64::XZR); // ORR X0, targetReg, XZR 实现 MOV 功能
    MachineBasicBlock::iterator nextInst = std::next(MI.getIterator());
    int num = ARMCollectIndirectJump::inCallID;
    // BL __collect__icall (调用外部函数)
    MBI = BuildMI(MBB, MI, DL, TII->get(AArch64::BL))
          .addExternalSymbol(sym);
    MBI=BuildMI(MBB,MI,DL,TII->get(AArch64::BL)).addExternalSymbol(sym);
    return true;

}