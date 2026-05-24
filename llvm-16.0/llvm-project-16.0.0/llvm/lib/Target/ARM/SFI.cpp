//保留寄存器R8
#include "ARM.h"
#include "llvm/Pass.h"
#include "ARMInstrInfo.h"
#include "ARMSubtarget.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include <fstream>
#include <iostream>
#include "llvm/IR/DebugLoc.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "json/json.h"
#include <map>
#include <vector>
#include "SFI.h"
#include <sstream>
#include<algorithm>
#include "llvm/IR/DebugInfoMetadata.h"
using namespace llvm;
#define DEBUG_TYPE "ARM-SFI"
#define ARM_SFI "ARM SFI"
INITIALIZE_PASS(ARMSFI,DEBUG_TYPE,ARM_SFI,false,false)
char ARMSFI::ID=0;
int  ARMSFI::init_flag=0;
std::map<std::string,std::string> ARMSFI::func2sec={};
std::map<int,std::vector<int>>  ARMSFI::sectaddrmask={};
void ARMSFI::build_func2sec(){

    Json::Value CPT_Root;
    std::ifstream CPT_File;
    CPT_File.open("./compartments_result.json");
    CPT_File >> CPT_Root;
    Json::Value PolicyRegions = CPT_Root.get("Regions","");
    for(auto RegionName: PolicyRegions.getMemberNames()){
        Json::Value Region = PolicyRegions[RegionName];
        Json::Value region_type = Region["Type"];
        if( region_type.compare("Code") == 0){
            for (auto funct: Region.get("Objects","")){
                func2sec[funct.asString()] = RegionName;
            }
        }
    }
}


//构建一个mask的映射，用于存储每个section的起始地址和mask
void ARMSFI ::build_sectaddrmask(){

    // Open the file storing the section start address and masks
   std::ifstream file("./sec_mask_result.txt");  // Replace with your filename
    if (!file) {
        std::cerr << "Unable to open file";
        return ;  // exit if file couldn't be opened
    }

    std::string line;
    int curr_cpt_idx = 1;
    while (std::getline(file, line)) {
        std::istringstream iss(line); 
        std::string hex_str, dec_str;
        if (!(iss >> hex_str >> dec_str)) {
            break;  // error in reading line or EOF
        }

        // Ignore '0x' at the start of the hex_str
        if (hex_str.size() >= 2 && hex_str.substr(0, 2) == "0x") {
            hex_str = hex_str.substr(2);
        }

        // Convert hex_str from hexadecimal to int
        int hex_val;
        std::stringstream ss;
        ss << std::hex << hex_str;  // set hex flag
        ss >> hex_val;             // convert hexadecimal string to int

        // Convert dec_str from decimal to int
        int dec_val = std::stoi(dec_str);  // convert decimal string to int

        std::vector<int> addrmask = {hex_val, dec_val};

        sectaddrmask[curr_cpt_idx] = addrmask;
        curr_cpt_idx += 1;

        // Print the converted values
        // std::cout << "Hexadecimal value: " << hex_val << ", Decimal value: " << dec_val << std::endl;
    }

    file.close();

}


std::map<std::string, int> ARMSFI :: readLinesIntoMap(const std::string& filename) {
    std::map<std::string, int> result;

    std::ifstream file(filename);
    if (!file.is_open()) {
        // throw std::runtime_error("Could not open file " + filename);
        errs() << "Could not open file" << filename << "\n";
    }

    std::string line;
    while (std::getline(file, line)) {
        result[line] = 0;
    }

    return result;
}

int ARMSFI::isSTRInstruction(const MachineInstr &MI) {
    switch (MI.getOpcode()) {
        case ARM::STRi12://87,第二个操作数是基址寄存器
            return 1;
        case ARM::STR_PRE_IMM://325，第三个操作数是基址寄存器
            return 2;
        case ARM::STR_PRE_REG://324，第三个操作数
            return 3;
        case ARM::STR_POST_IMM://324，第三个
            return 4;
        case ARM::STR_POST_REG: //324 ，第三个
            return 5;
        // case ARM::STRH_PRE:
        // case ARM::STRH_POST:
        // case ARM::STRH:    
        // case ARM::STRB_PRE_IMM:
        // case ARM::STRB_PRE_REG:
        // case ARM::STRB_POST_IMM:
        // case ARM::STRB_POST_REG:
        // case ARM::STRBi12:     
        default:
            return 0;
    }
}
//判断是否是间接寻址的store指令
bool ARMSFI::isIndirectAddressingSTR(const MachineInstr &MI) {
    if (isSTRInstruction(MI)==0)
        return false;

    // 确保操作数足够
    if (MI.getNumOperands() < 2)
        return false;

    // 第一个操作数是目标寄存器
    if (!MI.getOperand(0).isReg())
        return false;

    // 第二个操作数是基址寄存器
    if (!MI.getOperand(1).isReg())
        return false;

    // 可能存在第三个操作数（偏移量）
    if (MI.getNumOperands() > 2) {
        const MachineOperand &Op2 = MI.getOperand(2);
        if (Op2.isImm() || Op2.isReg()) {
            return true;  // 立即数或寄存器偏移
        }
    }

    return true;  // 仅基址寄存器 `[r1]` 也算间接寻址
}

int ARMSFI::isLDRInstruction(const MachineInstr &MI) {
    switch (MI.getOpcode()) {
        case ARM::LDRi12: 
            return 1;
        case ARM::LDR_PRE_IMM:
            return 2;
        case ARM::LDR_PRE_REG:
            return 3;
        case ARM::LDR_POST_IMM:
            return 4;
        case ARM::LDR_POST_REG: 
            return 5;
        // case ARM::LDRBi12:
        // case ARM::LDRH:    
        // case ARM::LDRSH:
        // case ARM::LDRSB:     
        // case ARM::LDRH_PRE:
        // case ARM::LDRH_POST:
        // case ARM::LDRB_PRE_IMM:
        // case ARM::LDRB_PRE_REG:
        // case ARM::LDRB_POST_IMM:
        // case ARM::LDRB_POST_REG:
        // case ARM::LDRSH_PRE:
        // case ARM::LDRSH_POST:
        // case ARM::LDRSB_PRE:
        // case ARM::LDRSB_POST:
        default:
            return 0;
    }
}
//判断是否是间接寻址的load指令
bool ARMSFI::isIndirectAddressLDR(const MachineInstr &MI){
    if(!isLDRInstruction(MI))
        return false;
    if(MI.getNumOperands()<2)
        return false;
    if(!MI.getOperand(0).isReg())
        return false;
    if(!MI.getOperand(1).isReg())
        return false;
    if(MI.getNumOperands()>2)
    {
        const MachineOperand &Op2=MI.getOperand(2);
        if(Op2.isImm()||Op2.isReg())
        {
            return true;
        }
    }
    return true;
}
//判断是否是控制流转移指令
bool ARMSFI::IsControlFlowInstruction(const MachineInstr &MI) {
    switch (MI.getOpcode()) {
        case ARM::B:         // Unconditional branch
        case ARM::BL:        // Unconditional branch with link
        case ARM::BX:        // Branch and exchange
        case ARM::BLX:       // Branch with link and exchange
        case ARM::BL_pred:   // Conditional branch with link
        case ARM::BLX_pred:  // Conditional branch with link and exchange
        case ARM::BLX_pred_noip:
        case ARM::BLX_noip:
        case ARM::BX_pred:
        case ARM::BX_RET:
        case ARM::LDMIA_RET:
            return true;
        default:
            return false;
    }
}


// 第一次遍历函数，找到需要插桩的指令
// 算法描述：
//     1.顺序遍历整个函数的每一条汇编指令
//     2.如果是store指令,且是寄存器间接寻址,例如:str r0,[r1,#4]则将该指令加入到tmp_vector中,
//     3.如果是ldr指令,且是寄存器间接寻址,例如:ldr r1,[r0,#3],则将str r0,[r1,#4]加入到real_vector中
//     4.如果是控制流分支指令,则将tmp_vector中的指令加入到real_vector中
//     5.返回real_vector

// 定义宏来选择使用哪个版本
//#define INSTRUMENT_ALL_INDIRECT_STORE  // 取消注释以启用插桩所有间接store指令

#ifdef INSTRUMENT_ALL_INDIRECT_STORE
// 简化版本：插桩所有间接寻址的store指令（除了使用sp寄存器的）
void ARMSFI::store_inst_analysis(MachineFunction &MF,SmallVector<MachineInstr*,500>& real_vector)
{
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    //遍历MachineFunction的每一个MachineBasicBlock
    for(MachineFunction::iterator FI=MF.begin();FI!=MF.end();++FI)
    {
        MachineBasicBlock &MBB=*FI;
        //遍历基本块中的所有指令
        for(MachineBasicBlock::iterator I=MBB.begin();I!=MBB.end();++I)
        {
            MachineInstr &MI=*I;
            //如果是store指令,且是寄存器间接寻址,忽略掉sp，pc,lr,r11(fp)，对这些寄存器的间接寻址另做处理
            if(isSTRInstruction(MI)!=0&&(MI.getOperand(0).getReg()!=ARM::SP&&MI.getOperand(1).getReg()!=ARM::SP&&MI.getOperand(2).getReg()!=ARM::SP&&MI.getOperand(0).getReg()!=ARM::PC&&MI.getOperand(1).getReg()!=ARM::PC&&MI.getOperand(2).getReg()!=ARM::PC&&MI.getOperand(0).getReg()!=ARM::LR&&MI.getOperand(1).getReg()!=ARM::LR&&MI.getOperand(2).getReg()!=ARM::LR&&MI.getOperand(0).getReg()!=ARM::R11&&MI.getOperand(1).getReg()!=ARM::R11&&MI.getOperand(2).getReg()!=ARM::R11))
            {
                // 直接将所有符合条件的store指令加入到real_vector中
                real_vector.push_back(&MI);
            }
        }
    }
}

#else
// 原始优化版本：通过分析ldr指令和控制流指令来减少插桩数量
void ARMSFI::store_inst_analysis(MachineFunction &MF,SmallVector<MachineInstr*,500>& real_vector)
{
    SmallVector <MachineInstr*,500> tmp_vector;
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    //遍历MachineFunction的每一个MachineBasicBlock
    for(MachineFunction::iterator FI=MF.begin();FI!=MF.end();++FI)
    {
        MachineBasicBlock &MBB=*FI;
        //遍历基本块中的所有指令
        for(MachineBasicBlock::iterator I=MBB.begin();I!=MBB.end();++I)
        {
            MachineInstr &MI=*I;
            //如果是store指令,且是寄存器间接寻址,忽略掉sp，pc,lr,r11(fp)，对这些寄存器的间接寻址另做处理
            if(isSTRInstruction(MI)!=0&&(MI.getOperand(0).getReg()!=ARM::SP&&MI.getOperand(1).getReg()!=ARM::SP&&MI.getOperand(2).getReg()!=ARM::SP&&MI.getOperand(0).getReg()!=ARM::PC&&MI.getOperand(1).getReg()!=ARM::PC&&MI.getOperand(2).getReg()!=ARM::PC&&MI.getOperand(0).getReg()!=ARM::LR&&MI.getOperand(1).getReg()!=ARM::LR&&MI.getOperand(2).getReg()!=ARM::LR&&MI.getOperand(0).getReg()!=ARM::R11&&MI.getOperand(1).getReg()!=ARM::R11&&MI.getOperand(2).getReg()!=ARM::R11))
            {
                //去除掉第二个或者第三个操作数一样的store指令,减少需要插桩的指令
                SmallVector <MachineInstr*,500> tmp2_vector;
                for(auto &tmpMI:tmp_vector)
                {
                    if(isSTRInstruction(MI)==1&&isSTRInstruction(*tmpMI)==1&&tmpMI->getOperand(1).getReg()==MI.getOperand(1).getReg())//说明基址寄存器，且偏移是立即数
                    {
                        tmp2_vector.push_back(tmpMI);
                    }
                    if((MI.getOpcode()==ARM::STR_PRE_IMM||MI.getOpcode()==ARM::STR_POST_IMM)&&(tmpMI->getOpcode()==ARM::STR_PRE_IMM||tmpMI->getOpcode()==ARM::STR_POST_IMM)&&tmpMI->getOperand(2).getReg()==MI.getOperand(2).getReg())//说明基址寄存器，且偏移是寄存器
                    {
                        tmp2_vector.push_back(tmpMI);
                    }
                    if((MI.getOpcode()==ARM::STR_PRE_REG||MI.getOpcode()==ARM::STR_POST_REG)&&(tmpMI->getOpcode()==ARM::STR_PRE_REG||tmpMI->getOpcode()==ARM::STR_POST_REG)&&((tmpMI->getOperand(2).getReg()==MI.getOperand(2).getReg()&&tmpMI->getOperand(3).getReg()==MI.getOperand(3).getReg())||(tmpMI->getOperand(2).getReg()==MI.getOperand(3).getReg()&&tmpMI->getOperand(3).getReg()==MI.getOperand(2).getReg())))//说明基址寄存器，且偏移是寄存器
                    {
                        tmp2_vector.push_back(tmpMI);
                    }
                }
                for (auto &tmpMI:tmp2_vector)//去除连续且基址寄存器相同的
                {
                    //删除单个元素
                    auto it = llvm::find(tmp_vector, tmpMI); // 查找元素
                    if (it != tmp_vector.end()) {
                        tmp_vector.erase(it); // 删除元素
                    }
                    
                    // //批量删除多个元素
                    // tmp_vector.erase(
                    //     llvm::remove_if(tmp_vector, [&](MachineInstr *MI) { return MI == tmpMI; }),
                    //     tmp_vector.end()
                    // );
                }
                tmp_vector.push_back(&MI);
            }
            //如果是ldr指令，且是寄存器间接寻址
            else if(isIndirectAddressLDR(MI))
            {
                SmallVector <MachineInstr*,500> tmp2_vector;
                //在tmp_vector中找到操作数相同的store指令
                //例如：ldr r1,[r0,#3],则找到str r0,[r1,#4]
                //将找到的store指令加入到real_vector中,且在tmp_vector中删除
                for(auto &tmpMI:tmp_vector)
                {
                    //如果是store指令
                    if(isSTRInstruction(*tmpMI)==1)//stri12
                    {
                        //如果store指令的第二个操作数和ldr指令的第一个操作数相同
                        if(tmpMI->getOperand(1).getReg()==MI.getOperand(0).getReg())
                        {
                            real_vector.push_back(tmpMI);
                            tmp2_vector.push_back(tmpMI);
                        }
                    }
                    else if (isSTRInstruction(*tmpMI)==2)//ARM::STR_PRE_IMM
                    {
                        //如果store指令的第3个操作数和ldr指令的第一个操作数相同
                        if(tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg())
                        {
                            real_vector.push_back(tmpMI);
                            tmp2_vector.push_back(tmpMI);
                        }
                    }
                    else if (isSTRInstruction(*tmpMI)==3)//ARM::STR_PRE_REG
                    {
                        //如果store指令的第2个操作数和ldr指令的第一个操作数相同
                        if(tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg()||tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg())
                        {
                            real_vector.push_back(tmpMI);
                            tmp2_vector.push_back(tmpMI);
                        }
                    }
                    else if(isSTRInstruction(*tmpMI)==4)//ARM::STR_POST_IMM
                    {
                        //如果store指令的第3个操作数和ldr指令的第一个操作数相同
                        if(tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg())
                        {
                            real_vector.push_back(tmpMI);
                            tmp2_vector.push_back(tmpMI);
                        }
                    }
                    else if (isSTRInstruction(*tmpMI)==5)//ARM::STR_POST_REG
                    {
                        //如果store指令的第2个操作数和ldr指令的第一个操作数相同
                        if(tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg()||tmpMI->getOperand(2).getReg()==MI.getOperand(0).getReg())
                        {
                            real_vector.push_back(tmpMI);
                            tmp2_vector.push_back(tmpMI);
                        }
                    }
                }
                for(auto &tmpMI:tmp2_vector)
                {
                    //删除单个元素
                    auto it = llvm::find(tmp_vector, tmpMI); // 查找元素
                    if (it != tmp_vector.end()) {
                        tmp_vector.erase(it); // 删除元素
                    }
                    
                    // //批量删除多个元素
                    // tmp_vector.erase(
                    //     llvm::remove_if(tmp_vector, [&](MachineInstr *MI) { return MI == tmpMI; }),
                    //     tmp_vector.end()
                    // );
                }

            }
            //如果是控制流分支指令
            if(IsControlFlowInstruction(MI))
            {
                for(auto &tmpMI:tmp_vector)
                {
                    real_vector.push_back(tmpMI);
                }
                tmp_vector.clear();
            }
        }
    }
}
#endif

// 对非关键路径中的间接控制流转移指令进行限制,限制其跳转范围
//     origin code:
//         blx/bx  r1
//     new code:   
//         bfi     r8,r1,#0,#0x1
//         blx/bx  r8

bool ARMSFI::InstrumentIndirectFwdTransferInst(MachineBasicBlock &MBB,MachineInstr & MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr,int base_addr ,int mask)
{   
    std::cout<<"InstrumentIndirectFwdTransferInst"<<std::endl;
    std::cout<<base_addr<<std::endl;
    std::cout<<mask<<std::endl;
    //MI.print(errs());
    //将基地址转移到r8
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
    // .addReg(ARM::R8)
    // .addImm(base_addr&0xFFFF)
    // .addImm(14)
    // .addImm(0);
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVTi16),ARM::R8)
    // .addReg(ARM::R8)
    // .addImm((base_addr>>16)&0xFFFF)
    // .addImm(14)
    // .addReg(0);
    //mov r8,0
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R8)
        .addImm(0)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //bfi r8,rx,#0,#24
    BuildMI(MBB, MI, DL, TII->get(ARM::BFI))
    .addReg(ARM::R8, RegState::Define)  // 目标寄存器R8
    .addReg(ARM::R8,RegState::Define)
    .addReg(MI.getOperand(0).getReg())   // 源寄存器，提供插入的位
    .addImm(~((1<<24)-1))                        // 插入位数width
    .addImm(14);                          // 条件码ARMCC::AL
    std::cout<<"insert bfi"<<std::endl;
    //blx r8
    if(MI.getOpcode()==ARM::BLX&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert blx"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BLX))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    else if(MI.getOpcode()==ARM::BX&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert bx"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BX))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    else if(MI.getOpcode()==ARM::BX_pred&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert bx_pred"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BX_pred))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    else if (MI.getOpcode()==ARM::BLX_pred&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert blx_pred"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BLX_pred))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    else if(MI.getOpcode()==ARM::BLX_pred_noip&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert blx_pred_noip"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BLX_pred_noip))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    else if(MI.getOpcode()==ARM::BLX_noip&&MI.getOperand(0).getReg()!=ARM::R8)
    {
        std::cout<<"insert blx_noip"<<std::endl;
        BuildMI(MBB,MI,DL,TII->get(ARM::BLX_noip))
        .addReg(ARM::R8);
        to_delete_instr.push_back(&MI);
        return true;
    }
    return true;
}

// 主要是保护measurement和verifier中的数据不会被目标程序中的store指令修改,假如measurement和verfier的代码和数据存储在0x40000000中
//     origin code:
//         str r0,[r1,#4] 或者 std r0,[r1,r2]
//     new code:
//         str r0,[r1,#4]
//         movw r8,#0x0000
//         movt r8,#0x0400
//         cmp r8,r1,#4或者cmp r8,r1,r2
//         blt abort

void ARMSFI::InstrumentStrInst(MachineBasicBlock &MBB, MachineInstr &MI, const DebugLoc &DL, const TargetInstrInfo *TII) {
    LLVM_DEBUG(dbgs() << __func__ << '\n');
    bool result = isSTRInstruction(MI);
    MachineFunction *MF = MBB.getParent();
    if (!result) {
        return;
    }
    const char * sym="__str_sfi";
    MachineBasicBlock::iterator nextInst=std::next(MI.getIterator());
    unsigned baseReg;   // 基址寄存器
    unsigned targetReg ; // 目标寄存器
    unsigned baseReg2;
    int type=0;
    switch (MI.getOpcode()) {
        case ARM::STR_PRE_IMM:
        case ARM::STR_POST_IMM: // 后变址指令
          targetReg = MI.getOperand(0).getReg(); // 源寄存器
          baseReg = MI.getOperand(2).getReg();   // 基址寄存器
          type=0;
          break;
        case ARM::STRi12:       // 基址+偏移指令
          targetReg = MI.getOperand(0).getReg(); // 源寄存器
          baseReg = MI.getOperand(1).getReg();   // 基址寄存器
          type=0;
          break;
        case ARM::STR_PRE_REG:
        case ARM::STR_POST_REG: // 后变址指令
          targetReg = MI.getOperand(0).getReg(); // 源寄存器
          baseReg = MI.getOperand(2).getReg();   // 基址寄存器
          baseReg2 = MI.getOperand(3).getReg();  // 偏移寄存器
          type=2;
          break;
        default:
          llvm_unreachable("Unsupported STR variant");
    }
    MachineInstr *MBI;
    // push lr
    // MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::STR_PRE_IMM))
    // .addReg(ARM::LR,RegState::Define)
    // .addReg(ARM::LR)//useless
    // .addReg(ARM::SP)
    // .addImm(-4)//pre offset
    // .addImm(14);//conditional codes
    // //psuh r0
    // MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::STR_PRE_IMM))
    //         .addReg(ARM::R0,RegState::Define)
    //         .addReg(ARM::R0)
    //         .addReg(ARM::SP)
    //         .addImm(-4)
    //         .addImm(14);
    MBI = BuildMI(MBB, nextInst, DL, TII->get(ARM::STMDB_UPD))
        .addReg(ARM::SP, RegState::Define) // 1. 写回更新后的 SP (SP = SP - 8)
        .addReg(ARM::SP)                   // 2. 基址寄存器 (当前的 SP)
        .addImm(14).addReg(0)              // 3. Predicate (Always)
        // --- 下面是要入栈的寄存器列表 ---
        .addReg(ARM::R0)                   // Push R0
        .addReg(ARM::LR);                  // Push LR
    if(type==2)
    {
        //add r0,r1,r2
        MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::ADDrr))
            .addReg(ARM::R0)
            .addReg(baseReg)
            .addReg(baseReg2)
            .addImm(14);
    }
    else{
    //mov r0 ,rn
    MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::MOVr))
        .addReg(ARM::R0)
        .addReg(baseReg)
        .addImm(14)
        .addImm(0)
        .addImm(0);
    }

    MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::BL)).addExternalSymbol(sym);
    // //pop r0
    // MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::LDR_POST_IMM))
    // .addReg(ARM::R0,RegState::Define)
    // .addReg(ARM::R0)
    // .addReg(ARM::SP)
    // .addImm(0)
    // .addImm(4)
    // .addImm(14);
    // //pop lr
    // MBI=BuildMI(MBB,nextInst,DL,TII->get(ARM::LDR_POST_IMM))
    // .addReg(ARM::LR,RegState::Define)
    // .addReg(ARM::LR)
    // .addReg(ARM::SP)
    // .addImm(0)
    // .addImm(4)
    // .addImm(14);
    MBI = BuildMI(MBB, nextInst, DL, TII->get(ARM::LDMIA_UPD))
    .addReg(ARM::SP, RegState::Define) // 1. 写回更新后的 SP
    .addReg(ARM::SP)                   // 2. 基址寄存器 (当前的 SP)
    .addImm(14).addReg(0)              // 3. Predicate (Always)
    // --- 下面是要出栈的寄存器 (注意必须加 RegState::Define) ---
    .addReg(ARM::R0, RegState::Define) // Pop 到 R0
    .addReg(ARM::LR, RegState::Define);// Pop 到 LR

}
//在函数结束处检查fp是否在合法的区域内
MachineBasicBlock * ARMSFI::InstrumentFuncExit(MachineFunction&MF)
{
    return nullptr;
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    //获取函数的最后一个基本块
    MachineBasicBlock &MBB = MF.back();
    //获取最后一条指令
    MachineBasicBlock::iterator LastInst = MBB.getLastNonDebugInstr();
    //获取最后一条指令的DebugLoc
    DebugLoc DL = LastInst->getDebugLoc();
    //在最后一条指令的前面插入，最后一条指令应该是bx r8或者是bx lr或者是pop pc
    if((LastInst->getOpcode()==ARM::BX&&LastInst->getOperand(0).getReg()==ARM::R8)||LastInst->getOpcode()==ARM::BX_RET||(LastInst->getOpcode()==ARM::LDR_POST_IMM&&LastInst->getOperand(0).getReg()==ARM::PC))
    {
        //movw r8,#0x0000
        BuildMI(MBB, LastInst, DL, TII->get(ARM::MOVi16))
        .addReg(ARM::R8)
        .addImm(0)
        .addImm(14)
        .addImm(0);
        //movt r8,#0x0400
        BuildMI(MBB, LastInst, DL, TII->get(ARM::MOVTi16), ARM::R8)
        .addReg(ARM::R8)
        .addImm(0x100)
        .addImm(14)
        .addImm(0);
        //cmp r8,fp,#0
        BuildMI(MBB, LastInst, DL, TII->get(ARM::CMPrr))
        .addReg(ARM::R11)
        .addReg(ARM::R8)
        .add(predOps(ARMCC::AL));
        //blt abort
        // 创建 TargetMBB 并插入到 MBB 之后
        MachineBasicBlock *TargetMBB = MF.CreateMachineBasicBlock();
        //MF.insert(std::next(MBB.getIterator()), TargetMBB);  // 关键修改：插入到 MBB 之后
        MF.insert(std::next(MF.back().getIterator()), TargetMBB);
        // 创建 SplitMBB 用于存放原 MBB 的残留指令
        MachineBasicBlock *SplitMBB = MF.CreateMachineBasicBlock();
        MF.insert(std::next(TargetMBB->getIterator()), SplitMBB);

        // 转移 MBB 中 Bcc 之后的指令到 SplitMBB
        SplitMBB->splice(SplitMBB->begin(), &MBB, LastInst, MBB.end());
        SplitMBB->transferSuccessors(&MBB);  // 继承原 MBB 的后继关系

        // 设置 MBB 的后继为 TargetMBB 和 SplitMBB
        MBB.addSuccessor(TargetMBB);
        MBB.addSuccessor(SplitMBB);

        // 插入 Bcc 指令（LT 条件分支到 TargetMBB）
        BuildMI(&MBB, DL, TII->get(ARM::Bcc))
            .addMBB(SplitMBB)
            .addImm(ARMCC::LT)
            .addReg(ARM::CPSR, RegState::Implicit);

        // 填充 TargetMBB：调用 abort 并终止
        BuildMI(TargetMBB, DL, TII->get(ARM::BL))
            .addExternalSymbol("abort")
            .add(predOps(ARMCC::AL));

        // 填充 SplitMBB：继承原控制流
        if (!SplitMBB->empty()) {
            // 如果 SplitMBB 有指令，确保以正确的分支或返回结尾
            // 例如：添加跳转到原流程的下一个基本块
        } else {
            BuildMI(SplitMBB, DL, TII->get(ARM::B))
                .addMBB(&*std::next(MBB.getIterator()));  // 跳转到原后继
        }
        return TargetMBB;
    }
    else
    {
        return nullptr;
    }
    

}

//将bx lr指令删除替换为：bx r8
bool ARMSFI::InstrumentBX_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr ,int base_addr,int mask )
{
    std::cout<<"InstrumentBX_RET"<<std::endl;
    std::cout<<base_addr<<std::endl;
    std::cout<<mask<<std::endl;
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
    .addReg(ARM::R8)
    .addImm(base_addr&0xFFFF)
    .addImm(14)
    .addImm(0);
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVTi16),ARM::R8)
    .addReg(ARM::R8)
    .addImm((base_addr>>16)&0xFFFF)
    .addImm(14)
    .addReg(0);
    // //mov r8,0
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
    //     .addReg(ARM::R8)
    //     .addImm(0);
    //bfi r8,lr,26,0
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::LR)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //bx r8
    BuildMI(MBB,MI,DL,TII->get(ARM::BX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
}
//将pop pc指令删除，替换为bx R8
bool ARMSFI::InstrumentPOPPC(MachineBasicBlock &MBB, MachineInstr &MI, const DebugLoc &DL, const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr,int base_addr ,int mask)
{
    std::cout<<"InstrumentPOPPC"<<std::endl;
    std::cout<<base_addr<<std::endl;
    std::cout<<mask<<std::endl;
     //pop lr 
    BuildMI(MBB,MI,DL,TII->get(ARM::LDR_POST_IMM))
        .addReg(ARM::LR,RegState::Define)
        .addReg(ARM::LR,RegState::Define)
        .addReg(ARM::SP,RegState::Define)
        .addImm(0)
        .addImm(4)
        .addImm(14);
    // //mvo R8,0
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
    //     .addReg(ARM::R8)
    //     .addImm(0)
    //     .addImm(14);
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVi16))
    // .addReg(ARM::R8)
    // .addImm(base_addr&0xFFFF)
    // .addImm(14)
    // .addImm(0);
    // BuildMI(MBB,MI,DL,TII->get(ARM::MOVTi16),ARM::R8)
    // .addReg(ARM::R8)
    // .addImm((base_addr>>16)&0xFFFF)
    // .addImm(14)
    // .addReg(0);
    BuildMI(MBB,MI,DL,TII->get(ARM::MOVi))
        .addReg(ARM::R8)
        .addImm(0)
        .addImm(14)//conditional code
        .addImm(0)
        .addImm(0);
    //bfi r8,lr ,0,26
    BuildMI(MBB,MI,DL,TII->get(ARM::BFI))
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::R8,RegState::Define)
        .addReg(ARM::LR)
        .addImm(~((1<<24)-1))
        .addImm(14);
    //bx r8
    BuildMI(MBB,MI,DL,TII->get(ARM::BX))
        .addReg(ARM::R8);
    to_delete_instr.push_back(&MI);
    return true;
}
void ARMSFI::initProcessFunction(){
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
void ARMSFI::initOnlycalledOnceFunction(){
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

bool ARMSFI::runOnMachineFunction(MachineFunction &MF) 
{   
    errs()<<"befor run SFI pass on func :"<<MF.getName()<<'\n';
    if(ARMSFI::init_flag==0)
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
        // AvoidHandleFuncName.push_back("main");
        //AvoidHandleFuncName.push_back("work_hrtthread");
        AvoidHandleFuncName.push_back("_sighandler");
        AvoidHandleFuncName.push_back("_ZL15sig_int_handleri");
        AvoidHandleFuncName.push_back("def_collect");
        AvoidHandleFuncName.push_back("Critical_def_check");
        AvoidHandleFuncName.push_back("non_sen_def_check");
        AvoidHandleFuncName.push_back("use_check_for_basic_type_in_struct");
        AvoidHandleFuncName.push_back("def_check_for_basic_type_in_struct");
        AvoidHandleFuncName.push_back("def_collect_for_float");
        AvoidHandleFuncName.push_back("Critical_def_check_for_float");

        //AvoidHandleFuncName.push_back("hrt_work_process");
        //errs()<<"run init on SFI"<<'\n';
        //初始化函数和section的映射
        //build_func2sec();
        //初始化section的起始地址和mask
        //build_sectaddrmask();
        initProcessFunction();
        initOnlycalledOnceFunction();
        ARMSFI::init_flag=1;
    }
    //errs()<<"after run init "<<'\n';
    const Function*F=&MF.getFunction();
    std::string cur_func_name=F->getName().str();
    if (cur_func_name.find("D2Ev") != std::string::npos || cur_func_name.find("D1Ev") != std::string::npos) {
        // 这是一个析构函数
        return false;
    }
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
    if(std::find(AvoidHandleFuncName.begin(),AvoidHandleFuncName.end(),cur_func_name)!=AvoidHandleFuncName.end())//不对libc函数的内部逻辑进行处理
    {
        std::cout<<"run avoid handle function "<<F->getName().str()<<std::endl;
        return false;
    }
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
    errs()<<"run SFI pass on func :"<<MF.getName()<<'\n';
    int curr_cpt_idx = 0;//当前函数对应的section的index
    int curr_cpt_mask=24;//当前函数对应的section的mask
    int curr_cpt_base_addr=0;//当前函数对应的section的基址
    // //找到当前函数对应的section
    // std::map<std::string,std::string>::iterator it_func2sec=func2sec.find(cur_func_name);
    // if(it_func2sec!=func2sec.end())
    // {
    //     //errs()<<"have found the func2sec"<<'\n';
    //     std::string str_sect=func2sec[cur_func_name];
    //     if(str_sect.length()>=0)
    //     {
    //         char second_last_char=str_sect[str_sect.length()-2];
    //         //errs()<<"run length >= 0"<<'\n';
    //         if(isdigit(second_last_char)){
    //             //errs()<<"run isdigit"<<'\n';
    //             int second_last_digit=second_last_char-'0';
    //             //errs()<<"second_last_digit is : "<<second_last_digit<<'\n';
    //             curr_cpt_idx=second_last_digit; 
    //             //errs()<<"curr_cpt_idx is : "<<curr_cpt_idx<<'\n';
    //         }
    //         else{
    //             std::cout<<"The second last char is not a digit"<<std::endl;
    //         }
    //     }
    //     else{
    //         std::cout<<"The string is too short "<<std::endl;
    //         return false;
    //     }

    // }
    // else{
    //     std::cout<<"The function is not in the map"<<std::endl;
    //     return false;
    // }
    // // errs()<<"run before std::vector<int>&curr_addmask=sectaddrmask[curr_cpt_idx]; "<<'\n';
    // // errs()<<curr_cpt_idx<<'\n';
    // std::vector<int>&curr_addmask=sectaddrmask[curr_cpt_idx];
    // // errs()<<"run after std::vector<int>&curr_addmask=sectaddrmask[curr_cpt_idx]; "<<'\n';
    // //获取当前函数对应的section的base address和mask
    // curr_cpt_base_addr=curr_addmask[0];
    // curr_cpt_mask=curr_addmask[1];
    //存储需要插桩的store指令
    SmallVector<MachineInstr*,500> to_instrument_inst;
    SmallVector<MachineInstr*,500> to_delete_instr;
    //分析出需要插桩的store指令
    //errs()<<"run analysis"<<'\n';
    
    store_inst_analysis(MF,to_instrument_inst);
    
    //插桩store指令
    for(auto &MI:to_instrument_inst)
    {
           //errs()<<"run first loop"<<'\n';
            MachineBasicBlock &MBB=*MI->getParent();
            InstrumentStrInst(MBB,*MI,MI->getDebugLoc(),MF.getSubtarget().getInstrInfo());
    }
    errs()<<"run SFI pass after handle str instr :"<<MF.getName()<<'\n';
    //MachineBasicBlock* targetMBB=InstrumentFuncExit(MF);//对fp进行处理
    if(std::find(ToprocessFunction.begin(),ToprocessFunction.end(),cur_func_name)!=ToprocessFunction.end())//这些函数的间接控制流指令已经被记录了
    {
        return true;
    }

    //遍历每一个函数,对间接控制流转移指令进行限制，实际上没有必要，因为关键路径上的间接控制流转移指令已经被限制了，但需要限制其访问我们的measurement和verifier
    for(MachineFunction::iterator FI=MF.begin();FI!=MF.end();++FI)
    {
       for(MachineBasicBlock::iterator I=FI->begin();I!=FI->end();++I)
       {
           MachineInstr &MI=*I;
           if((MI.getOpcode()==ARM::BLX||MI.getOpcode()==ARM::BX||MI.getOpcode()==ARM::BX_pred||MI.getOpcode()==ARM::BLX_pred_noip||MI.getOpcode()==ARM::BLX_pred||MI.getOpcode()==ARM::BLX_noip)&&MI.getOperand(0).getReg()!=ARM::R8)
           {
                
               InstrumentIndirectFwdTransferInst(*FI,MI,MI.getDebugLoc(),MF.getSubtarget().getInstrInfo(),to_delete_instr,curr_cpt_base_addr,curr_cpt_mask);
           }
           if(MI.getOpcode()==ARM::LDR_POST_IMM&&MI.getOperand(0).getReg()==ARM::PC)
           {
                if(cur_func_name=="main")
                    continue;
                InstrumentPOPPC(*FI,MI,MI.getDebugLoc(),MF.getSubtarget().getInstrInfo(),to_delete_instr,curr_cpt_base_addr,curr_cpt_mask);
           }
           if(MI.getOpcode()==ARM::BX_RET)
           {
                continue;    
            //InstrumentBX_RET(*FI,MI,MI.getDebugLoc(),MF.getSubtarget().getInstrInfo(),to_delete_instr,curr_cpt_base_addr,curr_cpt_mask); 
           }
       
       }
    }
     errs()<<"run SFI pass after handle indirect transfer  :"<<MF.getName()<<'\n';
    //delete instructions to be deleted
    for (auto &DI:to_delete_instr){
        MachineBasicBlock &MBB=*DI->getParent();
        //errs()<<(*DI)->getOpcode()<<(*DI)->getOperand(0)<<"\n";
        MBB.remove_instr(DI);                    
        //errs()<<"run delete instruction"<<"\n";
    }
    return true;
}
FunctionPass *llvm::createARMSFIPass() 
    { 
        return new ARMSFI(); 
    }