# Hardcode the return values of functions that are only called once.
from capstone import *
from capstone.arm import *
import keystone
import struct
import re
from elftools.elf.elffile import ELFFile
import lief
import mmap
import sys
import os
lib_path=".."
sys.path.append(lib_path)
from tools.fileOperation import *
import argparse

func_name_map={}# Mapping from function names to addresses
toprocessFuncSet=set()# Functions processed in the binary
to_process_func_addr=set()# Addresses of functions processed in the binary
called_once_func_addr=set()# Start addresses of functions called only once
to_process_ret_inst_addr={}# ret inst addr to callsite addr, key is ret inst addr, value is callsite addr
only_called_once_func_map={}# Mapping of functions called once and their corresponding call instruction IDs
has_visited_func=set()# Functions called once that are processed during runtime

def get_load_address(binfile):
    # Get the load address of the ELF file
    load_addresses=[]
    with open(binfile, "rb") as f:
        elf = ELFFile(f)
        for segment in elf.iter_segments():
            if segment.header.p_type == "PT_LOAD":  # Find loadable code segment
                load_addresses.append(segment.header.p_vaddr)  # This is the load address
    return load_addresses
# Assemble into machine code
def assemble_arm(assembly_code,current_addr):
    ks = keystone.Ks(keystone.KS_ARCH_ARM, keystone.KS_MODE_ARM)
    encoding, count = ks.asm(assembly_code,addr=current_addr)
    return bytes(encoding)



# Modify binary file
def modify_binary(file_name,outpufile):# file_name is the binary file name
    global to_process_ret_inst_addr
    binary=lief.parse(file_name)
    # Save original entry point
    original_entry_point = binary.header.entrypoint
    # # Save original virtual address, file offset, and alignment of all sections
    # original_section_data = {
    #     section.name: (section.virtual_address, section.file_offset)
    #     for section in binary.sections
    # }

    # Get .note.ABI-tag section
    abi_tag_section = binary.get_section(".note.ABI-tag")

    original_abi_tag_content = abi_tag_section.content if abi_tag_section else None
    # Get .text section
    text_section=binary.get_section(".CODE_REGION_1_")# .CODE_REGION_1_ is critical part, but now we don't need it, we put all code in .text
    print(hex(text_section.virtual_address))

    content=bytearray(text_section.content)
    # Modify binary file
    for ret_inst_addr,callsite_addr in to_process_ret_inst_addr.items():
        new_assemble_code="b "+ str(callsite_addr)
        new_instruction=assemble_arm(new_assemble_code,ret_inst_addr)
        instruction_offset=ret_inst_addr-text_section.virtual_address
        content[instruction_offset:instruction_offset+len(new_instruction)]=new_instruction
    text_section.content=list(content)
    abi_tag_section.content = original_abi_tag_content

    # # Restore virtual address, file offset, and alignment of all sections
    # for section in binary.sections:
    #     if section.name in original_section_data:
    #         # Restore virtual address, file offset, alignment, and size
    #         # print(section.name)
    #         original_virtual_address, original_file_offset= original_section_data[section.name]
    #         section.virtual_address = original_virtual_address
    #         section.file_offset = original_file_offset
    #     else:
    #         print(f"Section {section.name} not in original_section_data")

    # Restore entry point
    binary.header.entrypoint = original_entry_point
    print(hex(text_section.virtual_address))
    binary.write(outpufile)


# Get function addresses, including those that need processing and those called only once
def get_func_addr():
    global func_name_map
    global toprocessFuncSet
    global only_called_once_func_map
    global to_process_func_addr
    global called_once_func_addr
    for func_name in toprocessFuncSet:
        if func_name in func_name_map:
            to_process_func_addr.add(int(func_name_map[func_name],16))
    for func_name in only_called_once_func_map.keys():
        if func_name in func_name_map:
            called_once_func_addr.add(int(func_name_map[func_name],16))


# Get return instruction address of functions called only once
def get_ret_inst_addr(load_address,mm,md,current_address,binfile):
    cur_address=current_address
    while(True):
        mm.seek(cur_address-load_address)
        code=mm.read(4)
        if code==b'\x18\xff\x2f\xe1':# e12fff18 bx r8
            return cur_address
        if code==b'\x1e\xff\x2f\xe1': # e12fff1e bx lr
            return cur_address
        cur_address=cur_address+4
        # if judge_out_of_func_range(cur_address):
        #     return None
    # mm.seek(current_address-load_address)
    # code=mm.read(mm.size()-mm.tell())   
    # if current_address==0x2f79b8:
    #     print(mm.size())
    #     # print()
    # address=None
    # for i in md.disasm(code,current_address):
    #     if current_address==0x2f79b8:
    #         print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")
    #     if i.id==ARM_INS_BX:
    #         if(i.operands[0].reg == ARM_REG_LR):
    #             return i.address
    #         if(i.operands[0].reg ==ARM_REG_R8):
    #             return  i.address
# Judge whether it exceeds the range of the function
def judge_out_of_func_range(cur_addr):
    global func_name_map
    for key,value in func_name_map.items():
        if cur_addr==int(value,16):
            return True
    return False


# Map return instruction address to callsite
def map_ret_inst_to_callsite(binfile,load_address):
    global to_process_func_addr
    global called_once_func_addr
    global to_process_ret_inst_addr
    global only_called_once_func_map
    global has_visited_func
    md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
    md.detail=True
    with open(binfile,"rb") as f:
        mm=mmap.mmap(f.fileno(),0,prot=mmap.PROT_READ)
        for func_addr in to_process_func_addr:
            # print(func_addr)
            # func_addr_hex=int(func_addr,16)
            # print(hex(func_addr_hex))
            cur_address=func_addr
            finish=False
            if len(load_address) >= 2:
                if func_addr > load_address[1]:
                    continue
            while(True):
                if finish==True:
                    break
                mm.seek(cur_address-load_address[0])
                code=mm.read(4)
                for i in md.disasm(code,cur_address):
                    # print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")
                    # if func_addr==0xb3f18:
                        # print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")
                    if i.id==ARM_INS_BL:
                        if i.operands[0].value.imm not in called_once_func_addr:
                            # print(hex(i.operands[0].value.imm))
                                pass
                        else:
                            # print(hex(i.operands[0].value.imm))
                            callsite_addr=i.address+4
                            # print(hex(callsite_addr))
                            target_address=i.operands[0].value.imm
                            has_visited_func.add(target_address)
                            ret_inst_addr=get_ret_inst_addr(load_address[0],mm,md,target_address,binfile)
                            # print(ret_inst_addr)
                            to_process_ret_inst_addr[ret_inst_addr]=callsite_addr

                    if i.id==ARM_INS_BLX:
                        next_inst_addr=i.address+4
                        mm.seek(next_inst_addr-load_address[0])
                        next_inst_code=mm.read(4)
                        for  next_inst in  md.disasm(next_inst_code,next_inst_addr):
                            if next_inst.operands[0].reg==ARM_REG_R8:
                                # print(next_inst.id)
                                cur_address=next_inst.address
                                print(hex(next_inst.address))
                                print(hex(i.address))
                                print(hex(func_addr))
                                callinst_number=next_inst.operands[1].value.imm
                                print("call number is"+str(callinst_number))
                                for key,value in only_called_once_func_map.items():# key is funcName, value is (callinst_number, ret_inst_addr)
                                    if value[0]=="di":
                                        continue
                                    # (value[1])
                                    if int(value[1])==callinst_number:# Find the indirect callsite number corresponding to the callsite of the function called only once                     
                                        for key1,value1 in func_name_map.items():#key1 is calleeFuncName,value1 is calleeFuncAddress 
                                            if key1==key:
                                                addr=int(value1,16)
                                                break
                                        has_visited_func.add(addr)
                                        ret_inst_addr=get_ret_inst_addr(load_address[0],mm,md,addr,binfile)
                                            # print(ret_inst_addr)
                                        to_process_ret_inst_addr[ret_inst_addr]=i.address+4
                    if i.id==ARM_INS_BX:
                        if(i.operands[0].reg == ARM_REG_LR):
                            finish=True
                        if(i.operands[0].reg ==ARM_REG_R8):
                            finish=True
                cur_address=cur_address+4
    f.close()
                

def main():
    global to_process_ret_inst_addr
    global to_process_func_addr
    global toprocessFuncSet # Functions in the call graph
    global func_name_map
    global called_once_func_addr
    global has_visited_func
    global only_called_once_func_map
    global func_name_map
    binary_path = "~/ARTO/ardupilot/build/SITL_arm_linux_gnueabihf/arducopter"
    binary_path= os.path.abspath(os.path.expanduser(binary_path))
    disassembly_file_name="~/ARTO/ardupilot/build/SITL_arm_linux_gnueabihf/arducopter.S"
    disassembly_file_name = os.path.abspath(os.path.expanduser(disassembly_file_name))
    only_called_once_func_file="../output/only_called_once_func_backup_copter.txt"
    ToInsertFuncFile="../output/ToInsertFunc_copter.txt"
    output_binary_path="../output/arducopter_output"
    parser = argparse.ArgumentParser(description="Binary rewrite tool")
    parser.add_argument("--binary_path", default=binary_path, help="Path to the binary file")
    parser.add_argument("--disassembly_file_name", default=disassembly_file_name, help="Path to the disassembly file")
    parser.add_argument("--only_called_once_func_file", default=only_called_once_func_file, help="Path to only_called_once_func file")
    parser.add_argument("--ToInsertFuncFile", default=ToInsertFuncFile, help="Path to ToInsertFunc file")
    parser.add_argument("--output_binary_path", default=output_binary_path, help="Path to output binary file")
    args = parser.parse_args()

    binary_path = args.binary_path
    disassembly_file_name = args.disassembly_file_name
    only_called_once_func_file = args.only_called_once_func_file
    ToInsertFuncFile = args.ToInsertFuncFile
    output_binary_path = args.output_binary_path
    init_set(ToInsertFuncFile,toprocessFuncSet)
    map_address_to_funcname(disassembly_file_name,func_name_map)
    get_only_called_once_func(only_called_once_func_file,only_called_once_func_map)
    get_func_addr()
    
    r=0
    for key in toprocessFuncSet:
        r=r+1
        print(key)
    
    i=0
    for key in to_process_func_addr:
        i=i+1
        print(key)
    
    # j=0
    # for key ,value in func_name_map.items():
    #     j=j+1
    #     print(key+"--"+value)
    # print(j)
    # print(i)
    # print(r)
    # for key in toprocessFuncSet:
    #     if key not in func_name_map:
    #         print(key)

    k=0
    for key in called_once_func_addr:
        k=k+1
        print(hex(key))
    load_addresses=get_load_address(binary_path)
    for load_address in load_addresses:
        print("load address is "+ hex(load_address))
    map_ret_inst_to_callsite(binary_path,load_addresses)

    
    z=0
    for key,value in to_process_ret_inst_addr.items():
        if key!=None:
            print(hex(key)+"--"+hex(value)) 
        else: 
            print(str(key)+"--"+hex(value))
        z=z+1
    
    m=0
    for key in has_visited_func:
        m=m+1
        # print(hex(key))
    zz=0
    has_pross_func=set()
    for key in has_visited_func:
        for key1,value1 in func_name_map.items():
            if int(value1,16)==key:
                has_pross_func.add(key1)

    # Output functions that are called only once but have not been processed
    for key ,value in only_called_once_func_map.items():
        if key not in has_pross_func:
            print(key)
            zz=zz+1
            
    print("Found ret-callsite count: "+str(z))
    print("Number of functions called only once: "+str(k))
    print("Number of correctly processed functions: "+str(m))
    print("Number of functions to be processed: "+str(r))
    print("Number of addresses of functions found to be processed: "+str(i))
    print("Number of functions that cannot be processed: "+str(zz))
    modify_binary(binary_path,output_binary_path)


if __name__ == "__main__":
    main()
