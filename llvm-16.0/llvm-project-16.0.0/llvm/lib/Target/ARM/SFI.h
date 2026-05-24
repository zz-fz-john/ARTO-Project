#ifndef LLVM_LIB_TARGET_ARM_ARMSFI_H
#define LLVM_LIB_TARGET_ARM_ARMSFI_H
#include "llvm/CodeGen/MachineFunction.h"
namespace llvm{
    class ARMSFI :public MachineFunctionPass{
        public:
        static char ID;
        int one_time_flag;
        int main_cnt;
        int istmt_start_id;
        int istmt_end_id;  
        int istmt_curr_id;
        int debug_cnt;
        static int init_flag;
        //@brief 用于存储每个function对应的section
        static std::map<std::string,std::string>func2sec;
        std::vector<std::string> ToprocessFunction;
        std::vector<std::string> OnlyCalledOnceFunc;
        std::vector<std::string> AvoidHandleFuncName;
        //@brief 用于存储每个section的起始地址和mask
        //key 是section index，value是一个vector，包含了起始地址，和对应的mask
        static std::map<int,std::vector<int>> sectaddrmask;
        ARMSFI(): MachineFunctionPass(ID){initializeARMSFIPass(*PassRegistry::getPassRegistry());}
        bool runOnMachineFunction(MachineFunction &MF) override;
        //建立函数和section的映射
        void build_func2sec();
        //构建一个mask的映射，用于存储每个section的起始地址和mask
        void build_sectaddrmask();
        std::map<std::string, int>  readLinesIntoMap(const std::string& filename);
        //判断是否是store指令
        int isSTRInstruction(const MachineInstr &MI);
        //判断是否是间接寻址的store指令
        bool isIndirectAddressingSTR(const MachineInstr &MI);
        //判断是否是load指令
        int isLDRInstruction(const MachineInstr &MI);
        //判断是否是间接寻址的load指令
        bool isIndirectAddressLDR(const MachineInstr &MI);
        //判断是否是控制流转移指令
        bool IsControlFlowInstruction(const MachineInstr &MI);
        // 第一次遍历函数，找到需要插桩的指令
        // 算法描述：
        // 1.顺序遍历整个函数的每一条汇编指令
        // 2.如果是store指令,且是寄存器间接寻址,例如:str r0,[r1,#4]则将该指令加入到tmp_vector中,
        // 3.如果是ldr指令,且是寄存器间接寻址,例如:ldr r1,[r0,#3],则将str r0,[r1,#4]加入到real_vector中
        // 4.如果是控制流分支指令,则将tmp_vector中的指令加入到real_vector中
        // 5.返回real_vector

        void store_inst_analysis(MachineFunction &MF,SmallVector<MachineInstr*,500>& real_vector);

        // 对函数中的间接控制流转移指令进行限制,限制其跳转范围
        //     origin code:
        //         blx/bx  r1
        //     new code:   
        //         bfi     r8,r1,#0,#0x1
        //         blx/bx  r8
        MachineBasicBlock* InstrumentFuncExit(MachineFunction&MF);
        bool InstrumentIndirectFwdTransferInst(MachineBasicBlock &MBB,MachineInstr & MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr,int base_addr,int mask);
        // 主要是保护measurement和verifier中的数据不会被目标程序中的store指令修改,假如measurement和verfier的代码和数据存储在0x4000000中
        //     origin code:
        //         str r0,[r1,#4]
        //     new code:
        //         str r0,[r1,#4]
        //         movw r8,#0x0000
        //         movt r8,#0x0400
        //         cmp r8,r1,#4
        //         blt abort
        void InstrumentStrInst(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo*TII);
        bool InstrumentBX_RET(MachineBasicBlock &MBB,MachineInstr &MI,const DebugLoc &DL,const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr,int base_addr, int mask);
        bool InstrumentPOPPC(MachineBasicBlock &MBB, MachineInstr &MI, const DebugLoc &DL, const TargetInstrInfo *TII,SmallVector<MachineInstr*,500>& to_delete_instr,int base_addr ,int mask);
        void initProcessFunction();
        void initOnlycalledOnceFunction();
        StringRef getPassName()const override{
            return "arm sfi";
        }
    };
}
#endif