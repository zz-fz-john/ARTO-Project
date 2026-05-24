#用于在阿里云服务器上测试
#pip3 install capstone==5.1.0
#use blake2s (32-bit)
#最原始版本，使用fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()来记录,,并过滤掉不用对函数调用不起作用的bcc指令，以后使用该版本
import argparse
import binascii
import configparser
import logging
import math
import mmap
import os.path
import struct
import sys
sys.setrecursionlimit(30000)
from argparse import Namespace
from bitarray import bitarray
from capstone import *
from capstone.arm import *
from enum import Enum
from datetime import datetime
from xprint import to_hex,to_x
import subprocess
from elftools.elf.elffile import ELFFile
from pyblake2 import blake2s
import struct
import re
import time
import copy
lib_path=".."
sys.path.append(lib_path)
from tools.fileOperation import *
func_name_map={}##key=func_name ,value=func_addr
func_addr_map={}#key=addr,value=func_name
callsite_to_target_map={}##key=callsite_addr,value=target_addr
callsite_address_map={}#key= callsite in IR ,value = callsite_addr
shadow_stack=[]
instrument_func_addr=[]
branch_func_addr=[]
shadow_instrument_function_addr=[]
abort_func_addr=[]
only_called_once_func_addr=[]
ignore_func_addr=set()
to_insert_func=set()
to_insert_func_addr=[]
hash_map={}
hash_count={}
fake_hash_map={}
record_start_count_map={}
loop_count_map={}
compute_start=False
first_checkpoint=False
#visited_direct_call=set()
# checkpoint_start_addr=None
# checkpoint_end_addr=None
# compute_hash=blake2s()
CONFIG_SECTION_CODE_ADDRESSES="code-addresses"
CONFIG_DEFAULT_PATHNAME= './replay.cfg'
CONFIG_DEFAULTS={
    'load_address'          :'0x0000',
    'text_start'            :'None',
    'text_end'              :'None',
    'omit_addresses'        :'None',
    'collect_end'           :'None',
    'collect_start'         :'None',
    'loop_recordpoint'      :'None',
    'recursive_recordpoint' : 'None',
    'recursive_latch'       : 'None',
    'loop_latch'            :'None',
    'checkpoint'            : 'None',
}

def rotl64(x, r):
    return ((x << r) & 0xFFFFFFFFFFFFFFFF) | (x >> (64 - r))
def update_hash(current_hash, dest):
    # 强制为 64-bit 无符号
    current_hash &= 0xFFFFFFFFFFFFFFFF
    dest &= 0xFFFFFFFFFFFFFFFF

    current_hash ^= dest
    current_hash = rotl64(current_hash, 13)

    return current_hash & 0xFFFFFFFFFFFFFFFF
def read_config(pathname):
    parser=configparser.SafeConfigParser(CONFIG_DEFAULTS)
    parser.read(pathname)
    return Namespace(
            load_address            =   parser.get(CONFIG_SECTION_CODE_ADDRESSES, 'load_address'),
            text_start              =   parser.get(CONFIG_SECTION_CODE_ADDRESSES, 'text_start'),
            text_end                =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'text_end'),
            omit_address            =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'omit_addresses'),
            collect_start           =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'collect_start'),
            collect_end             =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'collect_end'),
            loop_recordpoint        =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'loop_recordpoint'),
            recursive_recordpoint   =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'recursive_recordpoint'),
            recursive_latch         =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'recursive_latch'),
            loop_latch              =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'loop_latch'),   
            checkpoint              =   parser.get(CONFIG_SECTION_CODE_ADDRESSES,'checkpoint'),
    )
def hexbytes(insn):
    width=int(pow(2,math.ceil(math.log(len(insn))/math.log(2))))
    return "0x"+binascii.hexlify(bytearray(insn)).zfill(width)
def main():
    start_time=time.time()
    parser=argparse.ArgumentParser(description="To generate offline database")
    parser.add_argument('file',nargs='?',metavar='FILE',
                        help='binary file to analysis')
    parser.add_argument('-L','--load-address',dest='load_address',default=None,
                        help='start address of section to analysis')
    parser.add_argument('--text-start', dest='text_start', default=None,
            help='start address of section to instrument')
    parser.add_argument('--text-end', dest='text_end', default=None,
            help='end address of section to instrument')
    parser.add_argument('--collect_start',dest='collect_start',default=None,
            help='collect end function address')  
    parser.add_argument('--collect_end',dest='collect_end',default=None,
            help='collect end function address')
    parser.add_argument('--loop_recordpoint', dest='loop_recordpoint', default=None,
            help='loop recordpoint function address')
    parser.add_argument('--recursive_recordpoint', dest='recursive_recordpoint', default=None,
            help='recursive recordpoint function address')
    parser.add_argument("-dis","--disassemble_file",dest='disassemble_file',default=None,
            help='disassemble file path')

    parser.add_argument("-IR","--IR_file",dest='IR_file',default=None,
            help='IR file path')
    parser.add_argument("-svf","--svf_result_file",dest='svf_result_file',default=None,
            help='svf result file path')
    parser.add_argument('-tiff',"--to_insert_func_file",dest='to_insert_func_file',default=None,
            help='to_insert_func_file path')
    parser.add_argument('-ocof',"--only_called_once_func_file",dest='only_called_once_func_file',default=None,
            help='only_called_once_func_file path')
    parser.add_argument('--ignore-functions', dest='ignore_functions', default=None,
            help='comma separated list of function names to ignore during DFS')
    parser.add_argument('--ignore-functions-file', dest='ignore_functions_file', default=None,
            help='file path with one ignored function name per line')


    parser.add_argument('--checkpoint',dest='checkpoint',default=None,
            help='checkpoint function address')
    parser.add_argument('--loop_latch',dest='loop_latch',default=None,
            help='loop latch function address')
    parser.add_argument('--recursive_latch',dest='recursive_latch',default=None,
            help='recursive latch function  address')    
    parser.add_argument('--omit-addresses', dest='omit_addresses', default=None,
            help='comma separated list of addresses of instructions to omit from instrumentation')
    parser.add_argument('-l', '--little-endian', dest='flags', default=[],
            action='append_const', const=CS_MODE_LITTLE_ENDIAN,
            help='disassemble in little endian mode')
    parser.add_argument('-big', '--big-endian', dest='flags', default=[],
            action='append_const', const=CS_MODE_BIG_ENDIAN,
            help='disassemble in big endian mode')
    parser.add_argument('-o', '--outfile', dest='outfile', default=None,
            help='outfile for hash data')
    parser.add_argument('-bf','--binary_file', dest = 'binary_file', default=None,
            help='final binary file to analysis')
    parser.add_argument('-c', '--config', dest='config', default=None,
            help='pathname of configuration file')
    parser.add_argument('--verbose', '-v', action='count',
            help='verbose output (repeat up to three times for additional information)')
    
    args=parser.parse_args()
    if args.verbose == None:
        logging.basicConfig(format='%(message)s',level=logging.ERROR)
    if args.verbose == 1:
        logging.basicConfig(format='%(message)s',level=logging.WARNING)
    if args.verbose == 2:
        logging.basicConfig(format='%(message)s',level=logging.INFO)
    if args.verbose >= 3:
        logging.basicConfig(format='%(message)s',level=logging.DEBUG)
    try:
        if args.config is not None:
            config = read_config(str(args.config) )
        else :
            config = read_config(CONFIG_DEFAULT_PATHNAME)
    except configparser.MissingSectionHeaderError as error:
            logging.error(error)
            sys.exit(1)
    def get_req_opt(opt):
        args_value=getattr(args,opt) if hasattr(args,opt) else None
        config_value=getattr(config,opt) if hasattr(config,opt) else None
        if args_value is not None:
            return args_value
        elif config_value is not None:
            return config_value
        else:
            exit("%s: required option '%s' not defined" % (sys.argv[0], opt))
    
    def get_csv_opt(opt):
        args_value=getattr(args,opt) if hasattr(args,opt) else None#获取args对象中名为opt的属性值
        config_value=getattr(config,opt) if hasattr(config,opt) else None##获取config对象中名为opt的属性值
        if args_value is not None:
            return args_value.split(',')
        elif config_value is not None:
            return config_value.split(',')
        else: return []
    opts=Namespace(
            binfile     =args.file,
            outfile     =args.outfile,
            cs_mode_flags  = args.flags,
            load_address            =   int(get_req_opt('load_address'),   16),
            text_start              =   int(get_req_opt('text_start'),     16),
            text_end                =   int(get_req_opt('text_end'),       16),
            collect_start           =   int(get_req_opt('collect_start'),  16),
            collect_end             =   int(get_req_opt('collect_end'),    16),
            loop_recordpoint        =   int(get_req_opt('loop_recordpoint'),    16),
            recursive_recordpoint   =   int(get_req_opt('recursive_recordpoint'),16),
            checkpoint              =   int(get_req_opt('checkpoint'), 16),
            recursive_latch         =   int(get_req_opt('recursive_latch'),16),
            loop_latch              =   int(get_req_opt('loop_latch'),16),
            omit_addresses          =   [int(i,16) for i in get_csv_opt('omit_addresses')],
            binary_file             =   args.binary_file,
            disassemble_file        =   args.disassemble_file,
            to_insert_func_file     =   args.to_insert_func_file,
            only_called_once_file   =   args.only_called_once_func_file,
            ignore_functions        =   args.ignore_functions,
            ignore_functions_file   =   args.ignore_functions_file,
            svf_result_file         =   args.svf_result_file,
            IR_file                 =   args.IR_file
    )
    logging.debug("load_address                 = 0x%08x" % opts.load_address)
    logging.debug("text_start                   = 0x%08x" % opts.text_start)
    logging.debug("text_end                     = 0x%08x" % opts.text_end)
    logging.debug("omit_addresses               = %s" % ['0x%08x' % i for i in opts.omit_addresses])
    logging.debug("collect_start                = 0x%08x" % opts.collect_start)
    logging.debug("collect_end                  = 0x%08x" % opts.collect_end)
    logging.debug("loop_recordpoint             = 0x%08x" % opts.loop_recordpoint)
    logging.debug("recursive_recordpoint        = 0x%08x" % opts.recursive_recordpoint)
    logging.debug("loop_latch                   = 0x%08x" %opts.loop_latch )
    logging.debug("recursive_latch              = 0x%08x" %opts.recursive_latch)
    logging.debug("checkpoint                   =0x%08x" % opts.checkpoint)
    logging.debug("binary_file                  = %s" % opts.binary_file)
    logging.debug("disassemble_file             = %s" % opts.disassemble_file)
    if not os.path.isfile(args.file):
        exit("%s: file '%s' not found" % (sys.argv[0], args.file))

    if not os.path.isfile(args.binary_file):
        exit("%s: file '%s' not found" % (sys.argv[0], args.binary_file))

    if not os.path.isfile(args.disassemble_file):
        exit("%s: file '%s' not found" % (sys.argv[0], args.disassemble_file))

    analysis(opts)
    end_time=time.time()
    print(f"total time is {(end_time-start_time):.2f} s")
##判断该指令在哪个函数中
def get_inst_func(inst_addr):##获取指令所在的函数
    global func_name_map
    keys=list(func_name_map.keys())
    for i,key in enumerate(keys):
        current_funcaddr=int('0x'+func_name_map[key],16)
        next_key=keys[i+1] if i+1 <len(keys) else None
        next_funcaddr=int('0x'+func_name_map[next_key],16) if next_key else None
        if next_funcaddr != None:
            if int(inst_addr,16) >=current_funcaddr and int(inst_addr,16) <= next_funcaddr:
                func_name=key
                return func_name
            




def construct_indirectcallsite_to_target_map(svf_result_file,disassembly_file_path,IR_file_path):
    """
    建立callsite_addr到target_addr的映射
    """
    global callsite_to_target_map
    global func_name_map
    global callsite_address_map
    global to_insert_func
    IRCallsite_pattern=re.compile(r"In function\s*:\s*([^\s]+)\s*indirect callsite\s*:\s*(.*)")
    target_pattern=re.compile(r'--target\s*(\S+)')
    function_name=None
    IR_inst_number=None
    Callsite_addr=None
    find_callsite=False
    map_address_to_callsite(disassembly_file_path,IR_file_path,callsite_address_map)
    # for key,value in  callsite_address_map.items():
    #     print(key+"----"+value)
    with open(svf_result_file,'r') as file:
        for line in file:
            line=line.strip()
            IRcallsite_match=IRCallsite_pattern.match(line)
            if IRcallsite_match:
                function_name=IRcallsite_match.group(1)
                if function_name not in to_insert_func:
                    find_callsite=False
                    continue
                find_callsite=True
                IR_inst_number=IRcallsite_match.group(2)
                target_line=function_name+"----"+IR_inst_number
                # print(target_line)
                #print(target_line)
                Callsite_addr=callsite_address_map[target_line]#callsite_addr是callsite在汇编文件中的地址
                callsite_to_target_map[Callsite_addr]=set()
                continue
            #if Callsite_addr!=None:
            else:
                target_match=target_pattern.match(line)
                if target_match:
                    if find_callsite==False:
                        continue
                    target_function=target_match.group(1)
                    #print(target_function+"\n")
                    # print(target_function)
                    if target_function not in func_name_map:
                        #print("target function "+target_function+" not in func_name_map")
                        target_function=target_function+"@plt"
                    target_addr=func_name_map[target_function]#target_addr 是目标函数的地址
                    ##print(target_addr)
                    callsite_to_target_map[Callsite_addr].add(target_addr)

def reverse_func_name_map(func_name_map):
    """
    将func_name_map中的key和value进行反转
    """
    reverse_map={}
    for key in func_name_map.keys():
        value=func_name_map[key]
        reverse_map[int(value,16)]=key
    return reverse_map

loop_count=0
should_bcc=0
def recurse_to_hash(opts,mm,md,current_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list):
    global instrument_func_addr
    global should_bcc
    global branch_func_addr
    global shadow_instrument_function_addr
    global abort_func_addr
    global shadow_stack
    global only_called_once_func_addr
    global ignore_func_addr
    global to_insert_func_addr
    global func_addr_map
    global fake_hash_map
    global loop_count
    global hash_map
    global record_start_count_map
    global hash_count
    global loop_count_map
    #global first_checkpoint
    #global compute_start
    #global edge_list=[]
    # while(True):
    print(hex(current_address))
    mm.seek(current_address-opts.load_address)
    code=mm.read(mm.size()-mm.tell())
    for i in md.disasm(code,current_address):
        #print(hex(i.address)+"  "+i.mnemonic+i.op_str)
        if i.id==ARM_INS_B:#direct branch b 
            print(hex(i.address)+"  "+i.mnemonic+i.op_str)
            target_address=i.operands[0].imm
            if i.cc==ARM_CC_AL:
                branch_list.append(i.address)
                branch_list.append(target_address)
                compute_hash_path=blake2s(digest_size=8)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                branch_list.pop()
                branch_list.pop()
                return
            else:#beq，blt
                # shadow_stack_copy=shadow_stack.copy()
                if loop_depth>3:
                    return
                compute_hash_path=blake2s(digest_size=8)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if i.address not in loop_count_map:
                    loop_count_map[i.address]=1
                loop_count_map[i.address]=loop_count_map[i.address]+1
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                if should_bcc==0:
                    recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                    return
                old_should_bcc=should_bcc
                should_bcc=0
                branch_list.append(i.address)
                branch_list.append(i.address+4)
                recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list )
                branch_list.pop()
                branch_list.pop()
                branch_list.append(i.address)
                branch_list.append(target_address)
                recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                branch_list.pop()
                branch_list.pop()
                should_bcc=old_should_bcc
                return
                # continue
        if i.id==ARM_INS_BL:#direct call bl
            print(hex(i.address)+"  "+i.mnemonic+i.op_str)
            target_address=i.operands[0].imm
            if target_address in ignore_func_addr:
                recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                return
            if target_address in instrument_func_addr:
                compute_hash_path=blake2s(digest_size=4)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                return
            elif target_address in branch_func_addr:
                compute_hash_path=blake2s(digest_size=4)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                old_should_bcc=should_bcc
                should_bcc=1
                recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                should_bcc=old_should_bcc
                return
            elif target_address in abort_func_addr:
                return
            elif hex(target_address)==hex(opts.collect_start):##处理start_collecting
                fake_stack=[]
                recurse_to_hash(opts,mm,md,i.address+4,i.address+4,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                del fake_stack
                return
            elif target_address==opts.collect_end:##处理end_collecting
                recordpoint_end_addr=i.address+4
                ##compute_hash=blake2s(digest_size=16)##blake2s哈希值
                compute_hash = blake2s(digest_size=8)
                
                ##计算列表中的哈希               
                for edge in edge_list:
                    #print('collect end edge list is '+hex(edge))
                    #compute_hash.update(struct.pack('i',int(str(edge),16)))
                    compute_hash.update(struct.pack('i',edge))
                    #current_hash = update_hash(current_hash, edge)
                #compute_hash.update(struct.pack('i',int(str(recordpoint_start_addr),16)))
                #compute_hash.update(struct.pack('i',int(str(recordpoint_end_addr),16)))
                #hash_map_copy=copy.deepcopy(hash_map)
                print("encounter end_collecting")
                if (recordpoint_start_addr, recordpoint_end_addr) not in hash_map:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                if compute_hash.hexdigest() not in hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    # print("find i "+str(loop_count))
                    loop_count=loop_count+1
                    for edge in edge_list:
                        print('collect end edge list is '+hex(edge))
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash.hexdigest())
                        # for key,values in hash_map.items():
                        #     for value in values:
                        # f.write("1,"+hex(checkpoint_start_addr)+','+hex(checkpoint_end_addr)+'\n')
                    with open(opts.outfile,"a") as file:
                        file.write("1,"+hex(recordpoint_start_addr)+','+hex(recordpoint_end_addr)+'\n')
                        file.write("2,"+str(int.from_bytes(compute_hash.digest(), "little"))+'\n')
                        #file.write("2,"+compute_hash.hexdigest()+'\n')
                        for edge in edge_list:
                            file.write(hex(edge)+'\n')
                return
            elif target_address==opts.loop_recordpoint:# 处理loop处的recordpoint
                if loop_depth>3:
                    return
                recordpoint_end_addr=i.address+4
                #compute_hash=blake2s(digest_size=16)##哈希值
                compute_hash = blake2s(digest_size=8)
                #compute_hash=0
                ##计算列表中的哈希
                for edge in edge_list:
                    #print('loop_recordpoint edge list is '+hex(edge))
                    #compute_hash.update(struct.pack('i',int(str(edge),16)))
                    compute_hash.update(struct.pack('i',edge))
                    #compute_hash= update_hash(compute_hash, edge)
                #compute_hash.update(struct.pack('i',int(str(recordpoint_start_addr),16)))
                #compute_hash.update(struct.pack('i',int(str(recordpoint_end_addr),16)))
                if (recordpoint_start_addr, recordpoint_end_addr) not in hash_map:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                if compute_hash.hexdigest() not in hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    for edge in edge_list:
                        print('loop_recordpoint edge list is '+hex(edge))
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash.hexdigest())
                    with open(opts.outfile,"a") as file:#记录在txt文件中 
                        file.write("1,"+hex(recordpoint_start_addr)+','+hex(recordpoint_end_addr)+'\n')
                        file.write("2,"+str(int.from_bytes(compute_hash.digest(), "little"))+'\n')
                        #file.write("2,"+compute_hash.hexdigest()+'\n')
                        for edge in edge_list:
                            file.write(hex(edge)+'\n') 

                compute_hash_fake=blake2s(digest_size=4)
                compute_hash_shadow=blake2s(digest_size=4)
                for edge in fake_stack:#计算相关路径，fake_stack 中保存了call指令的下一条指令的地址
                    compute_hash_fake.update(struct.pack('i',edge))
                for edge in shadow_stack:
                    compute_hash_shadow.update(struct.pack('i',edge))
                if (recordpoint_start_addr, recordpoint_end_addr) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                # if compute_hash_fake.hexdigest() not in hash_count:
                #     hash_count[compute_hash_fake.hexdigest()]=0
                # hash_count[compute_hash_fake.hexdigest()]=hash_count[compute_hash_fake.hexdigest()]+1
                if i.address not in loop_count_map:
                    loop_count_map[i.address]=1
                if compute_hash_fake.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] \
                    or compute_hash_shadow.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    # if loop_count_map[i.address]>4:
                    #     return 
                    loop_count_map[i.address]=loop_count_map[i.address]+1
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_fake.hexdigest())
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_shadow.hexdigest())
                    edge_list_copy=[]
                    fake_stack_copy=[]
                    branch_list_copy=[]
                    recurse_to_hash(opts,mm,md,i.address+8,i.address+4,edge_list_copy,fake_stack_copy,loop_depth,recursive_depth,branch_list_copy)
                    del edge_list_copy
                    del fake_stack_copy
                    del branch_list_copy
                    edge_list_copy=[]
                    fake_stack_copy=[]
                    branch_list_copy=[]
                    recurse_to_hash(opts,mm,md,i.address+4,i.address+4,edge_list_copy,fake_stack_copy,loop_depth,recursive_depth,branch_list_copy)
                    del edge_list_copy
                    del fake_stack_copy
                    del branch_list_copy
                    return
                else:#说明之前已经走过该路径了
                    return
            elif target_address==opts.recursive_recordpoint:#处理在递归函数中entry的recordpoint
                if recursive_depth>3:
                    return 
                recordpoint_end_addr=i.address+4
                #compute_hash=blake2s(digest_size=16)
                compute_hash = blake2s(digest_size=8)
                #compute_hash=0
                for edge in edge_list:
                    # print('recursive_recordpoint edge list is '+hex(edge))
                    #compute_hash.update(struct.pack('i',int(str(edge),16)))
                    compute_hash.update(struct.pack('i',edge))
                    #compute_hash= update_hash(compute_hash, edge)
                #compute_hash.update(struct.pack('i',int(str(recordpoint_start_addr),16)))
                #compute_hash.update(struct.pack('i',int(str(recordpoint_end_addr),16)))
                if (recordpoint_start_addr, recordpoint_end_addr) not in hash_map:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                if compute_hash.hexdigest() not in hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash.hexdigest())
                    for edge in edge_list:
                        print('recursive_recordpoint edge list is '+hex(edge))
                    with open(opts.outfile,"a") as file:
                        file.write("1,"+hex(recordpoint_start_addr)+','+hex(recordpoint_end_addr)+'\n')
                        file.write("2,"+str(int.from_bytes(compute_hash.digest(), "little"))+'\n')
                        #file.write("2,"+compute_hash.hexdigest()+'\n')
                        for edge in edge_list:
                            file.write(hex(edge)+'\n')

                compute_hash_fake=blake2s(digest_size=4)
                compute_hash_shadow=blake2s(digest_size=4)
                for edge in fake_stack:#计算相关路径，fake_stack 中保存了call指令的下一条指令的地址
                    compute_hash_fake.update(struct.pack('i',edge))
                for edge in shadow_stack:
                    compute_hash_shadow.update(struct.pack('i',edge))
                if (recordpoint_start_addr, recordpoint_end_addr) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                # if compute_hash_fake.hexdigest() not in hash_count:
                #     hash_count[compute_hash_fake.hexdigest()]=0
                # hash_count[compute_hash_fake.hexdigest()]=hash_count[compute_hash_fake.hexdigest()]+1
                if compute_hash_fake.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] \
                    or compute_hash_shadow.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:#判断之前是否走过该路径，因为是深度优先搜索，所以只要判断fake_stack中的路径是否走过即可
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_fake.hexdigest())
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_shadow.hexdigest())
                    edge_list_copy=[]
                    fake_stack_copy=[]
                    branch_list_copy=[]
                    recurse_to_hash(opts,mm,md,i.address+4,i.address+4,edge_list_copy,fake_stack_copy,loop_depth,recursive_depth+1,branch_list_copy )
                    del edge_list_copy
                    del fake_stack_copy
                    del branch_list_copy
                    return
                else:#说明之前已经走过该路径了
                    return
            elif target_address==opts.recursive_latch:#处理在递归尾函数中的retern前插入的recordpoint
                # if recursive_depth>4:
                #     return 
                recordpoint_end_addr=i.address+4
                #compute_hash=blake2s(digest_size=16)
                compute_hash = blake2s(digest_size=8)
                #compute_hash=0
                for edge in edge_list:
                    # print('recursive_recordpoint edge list is '+hex(edge))
                    #compute_hash.update(struct.pack('i',int(str(edge),16)))
                    compute_hash.update(struct.pack('i',edge))
                    #compute_hash= update_hash(compute_hash, edge)
                # compute_hash.update(struct.pack('i',int(str(recordpoint_start_addr),16)))
                # compute_hash.update(struct.pack('i',int(str(recordpoint_end_addr),16)))
                if (recordpoint_start_addr, recordpoint_end_addr) not in hash_map:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                if compute_hash.hexdigest() not in hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash.hexdigest())
                    for edge in edge_list:
                        print('recursive_recordpoint edge list is '+hex(edge))
                    with open(opts.outfile,"a") as file:
                        file.write("1,"+hex(recordpoint_start_addr)+','+hex(recordpoint_end_addr)+'\n')
                        file.write("2,"+str(int.from_bytes(compute_hash.digest(), "little"))+'\n')
                        #file.write("2,"+compute_hash.hexdigest()+'\n')
                        for edge in edge_list:
                            file.write(hex(edge)+'\n')

                compute_hash_fake=blake2s(digest_size=4)
                compute_hash_shadow=blake2s(digest_size=4)
                for edge in fake_stack:#计算相关路径，fake_stack 中保存了call指令的下一条指令的地址
                    compute_hash_fake.update(struct.pack('i',edge))
                for edge in shadow_stack:    
                    compute_hash_shadow.update(struct.pack('i',edge))    
                if (recordpoint_start_addr, recordpoint_end_addr) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                # if compute_hash_fake.hexdigest() not in hash_count:
                #     hash_count[compute_hash_fake.hexdigest()]=0
                # hash_count[compute_hash_fake.hexdigest()]=hash_count[compute_hash_fake.hexdigest()]+1
                if compute_hash_fake.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] \
                    or compute_hash_shadow.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:#判断之前是否走过该路径，因为是深度优先搜索，所以只要判断fake_stack中的路径是否走过即可
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_fake.hexdigest())
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_shadow.hexdigest())
                    edge_list_copy=[]
                    fake_stack_copy=[]
                    branch_list_copy=[]
                    recurse_to_hash(opts,mm,md,i.address+4,i.address+4,edge_list_copy,fake_stack_copy,loop_depth,recursive_depth+1,branch_list_copy)
                    del edge_list_copy
                    del fake_stack_copy
                    del branch_list_copy
                    return
                else:#说明之前已经走过该路径了
                    return

            elif target_address==opts.loop_latch:##处理循环体中没有函数调用的循环
                if loop_depth>5:
                    return
                recurse_to_hash(opts,mm,md,i.address+8,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                # compute_hash_fake=blake2s(digest_size=32)
                # compute_hash_shadow=blake2s(digest_size=32)
                # recordpoint_end_addr=i.address+4
                # for edge in fake_stack:#计算相关路径，fake_stack 中保存了call指令的下一条指令的地址
                #     compute_hash_fake.update(struct.pack('i',edge))
                # for edge in shadow_stack:
                #     compute_hash_shadow.update(struct.pack('i',edge))
                # if (recordpoint_start_addr, recordpoint_end_addr) not in fake_hash_map:
                #     fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                # if compute_hash_fake.hexdigest() not in hash_count:
                #     hash_count[compute_hash_fake.hexdigest()]=0
                # hash_count[compute_hash_fake.hexdigest()]=hash_count[compute_hash_fake.hexdigest()]+1
                # if compute_hash_fake.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] \
                #     or compute_hash_shadow.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:#判断之前是否走过该路径，因为是深度优先搜索，所以只要判断fake_stack中的路径是否走过即可
                #     fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_fake.hexdigest())
                #     fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_shadow.hexdigest())
                #     recurse_to_hash(opts,mm,md,i.address+8,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth)
                #     return
                # else:#说明之前已经走过该路径了
                #     return
            elif target_address==opts.checkpoint:##处理叶节点处的checkpoint
                recordpoint_end_addr=i.address+4
                #compute_hash=blake2s(digest_size=16)##哈希值
                compute_hash = blake2s(digest_size=8)
                ##计算列表中的哈希
                for edge in edge_list:
                    # print(edge)
                    #compute_hash.update(struct.pack('i',int(str(edge),16)))
                    compute_hash.update(struct.pack('i',edge))
                # compute_hash.update(struct.pack('i',int(str(recordpoint_start_addr),16)))
                # compute_hash.update(struct.pack('i',int(str(recordpoint_end_addr),16)))
                if (recordpoint_start_addr, recordpoint_end_addr) not in hash_map:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                if compute_hash.hexdigest() not in hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:
                    hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash.hexdigest())
                    for edge in edge_list:
                        print('checkpoint edge list is '+hex(edge))
                    with open(opts.outfile,"a") as file:
                        file.write("1,"+hex(recordpoint_start_addr)+','+hex(recordpoint_end_addr)+'\n')
                        file.write("2,"+str(int.from_bytes(compute_hash.digest(), "little"))+'\n')
                        #file.write("2,"+compute_hash.hexdigest()+'\n')
                        for edge in edge_list:
                            file.write(hex(edge)+'\n')

                compute_hash_fake=blake2s(digest_size=4)
                compute_hash_shadow=blake2s(digest_size=4)
                for edge in fake_stack:#计算相关路径，fake_stack 中保存了call指令的下一条指令的地址
                    compute_hash_fake.update(struct.pack('i',edge))
                for edge in shadow_stack:
                    compute_hash_shadow.update(struct.pack('i',edge))
                if (recordpoint_start_addr, recordpoint_end_addr) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] = set()
                # if compute_hash_fake.hexdigest() not in hash_count:
                #     hash_count[compute_hash_fake.hexdigest()]=0
                # hash_count[compute_hash_fake.hexdigest()]=hash_count[compute_hash_fake.hexdigest()]+1
                if compute_hash_fake.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] \
                    or compute_hash_shadow.hexdigest() not in fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)]:#判断之前是否走过该路径，因为是深度优先搜索，所以只要判断fake_stack中的路径是否走过即可
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_fake.hexdigest())
                    fake_hash_map[(recordpoint_start_addr, recordpoint_end_addr)] .add(compute_hash_shadow.hexdigest())
                    edge_list_copy=[]
                    fake_stack_copy=[]
                    branch_list_copy=[]
                    recurse_to_hash(opts,mm,md,i.address+4,i.address+4,edge_list_copy,fake_stack_copy,loop_depth,recursive_depth,branch_list_copy)
                    del fake_stack_copy
                    del edge_list_copy
                    del branch_list_copy
                    return
                else:#说明之前已经走过该路径了
                    return
            else:
                if i.cc==ARM_CC_AL:
                    compute_hash_path=blake2s(digest_size=4)
                    for edge in  fake_stack + shadow_stack:
                        compute_hash_path.update(struct.pack('i', edge))
                    path_hash = compute_hash_path.hexdigest()
                    if (recordpoint_start_addr, i.address) not in fake_hash_map:
                        fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                    if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                        return
                    fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)

                    if target_address  not in  to_insert_func_addr:         
                        recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                        return
                    #edge_list.append(i.address)
                    # edge_list.append(target_address)
                    print(hex(i.address)+"---"+func_addr_map[target_address])
                    if target_address not in only_called_once_func_addr:
                        shadow_stack.append(i.address+4)
                        # print("push  "+hex(i.address+4))
                    fake_stack.append(i.address)
                    fake_stack.append(target_address)
                    recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                    fake_stack.pop()
                    fake_stack.pop()
                    if target_address not in only_called_once_func_addr:
                        shadow_stack.pop()
                    #edge_list.pop()
                    # edge_list.pop()
                    return           
                else:
                    # exit("run into error")
                    compute_hash_path=blake2s(digest_size=4)
                    for edge in fake_stack + shadow_stack:
                        compute_hash_path.update(struct.pack('i', edge))
                    path_hash = compute_hash_path.hexdigest()
                    if (recordpoint_start_addr, i.address) not in fake_hash_map:
                        fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                    if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                        return
                    fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)

                    if target_address not in to_insert_func_addr:
                        recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                        return
                    print(hex(i.address)+"---"+func_addr_map[target_address])
                    fake_stack.append(i.address)
                    fake_stack.append(i.address+4)
                    recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                    fake_stack.pop()
                    fake_stack.pop()
                    if target_address not in only_called_once_func_addr:
                        shadow_stack.append(i.address+4)
                    fake_stack.append(i.address)
                    fake_stack.append(target_address)
                    recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                    fake_stack.pop()
                    fake_stack.pop()
                    if target_address not in only_called_once_func_addr:
                        shadow_stack.pop()
                    return
        if i.id==ARM_INS_BX:#indirect branch bx
            print(hex(i.address)+"  "+i.mnemonic+i.op_str)
            if(i.operands[0].reg == ARM_REG_LR):
                if len(shadow_stack)==0:
                    exit(hex(i.address))
                compute_hash_path=blake2s(digest_size=4)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                target_address=shadow_stack.pop()
                # print("pop  "+hex(i.address)+"  "+hex(target_address))
                #print(hex(target_address))
                #edge_list.append(i.address)
                #edge_list.append(target_address)
                fake_stack.append(i.address)
                fake_stack.append(target_address)
                recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                fake_stack.pop()
                fake_stack.pop()
                shadow_stack.append(target_address)
                #edge_list.pop()
                #edge_list.pop()
                return
            if(i.operands[0].reg ==ARM_REG_R8):
                if len(shadow_stack)==0:
                    exit(hex(i.address))
                compute_hash_path=blake2s(digest_size=4)
                for edge in  fake_stack + shadow_stack:
                    compute_hash_path.update(struct.pack('i', edge))
                path_hash = compute_hash_path.hexdigest()
                if (recordpoint_start_addr, i.address) not in fake_hash_map:
                    fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                    return
                fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)
                target_address=shadow_stack.pop()
                # print("pop  "+hex(i.address)+"  "+hex(target_address))
                # print(hex(target_address))
                #edge_list.append(i.address)
                edge_list.append(target_address)
                branch_list.append(i.address)
                branch_list.append(target_address)
                fake_stack.append(i.address)
                fake_stack.append(target_address)
                recurse_to_hash(opts,mm,md,target_address,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                branch_list.pop()
                branch_list.pop()
                fake_stack.pop()
                fake_stack.pop()
                shadow_stack.append(target_address)
                edge_list.pop()
                #edge_list.pop()
                return
            else:
                exit("meet wrong bx ")
        if i.id==ARM_INS_BLX:
            is_reg=False
            for op in  i.operands:
                if op.type==ARM_OP_REG:
                    is_reg=True
                    break
            if is_reg:
                if i.cc==ARM_CC_AL:
                    compute_hash_path=blake2s(digest_size=4)
                    for edge in fake_stack + shadow_stack:
                        compute_hash_path.update(struct.pack('i', edge))
                    path_hash = compute_hash_path.hexdigest()
                    if (recordpoint_start_addr, i.address) not in fake_hash_map:
                        fake_hash_map[(recordpoint_start_addr, i.address)] = set()
                    if path_hash in fake_hash_map[(recordpoint_start_addr, i.address)]:
                        return
                    fake_hash_map[(recordpoint_start_addr, i.address)].add(path_hash)

                    if hex(i.address) not in callsite_to_target_map:
                        recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                        return
                    for value in sorted(callsite_to_target_map[hex(i.address)]):
                        #print("target is "+value)
                        if int(value,16) in instrument_func_addr:
                            print(hex(i.address)+"---"+func_addr_map[int(value,16)])
                            # if func_addr_map[int(value,16)]=="malloc@plt":
                            #     print(value+"  malloc found")
                            #     for edge in edge_list:
                            #         print('collect end edge list is '+hex(edge))
                            #     exit("meet indirect blx")
                            int_value=int(value,16)
                            edge_list.append(int_value)
                            branch_list.append(i.address)
                            branch_list.append(int_value)
                            fake_stack.append(i.address)
                            fake_stack.append(int_value)
                            recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                            fake_stack.pop()
                            fake_stack.pop()
                            branch_list.pop()
                            branch_list.pop()
                            edge_list.pop()
                            continue
                            # if func_addr_map[int(value,16)]=="malloc@plt":
                            #     print(value+"  malloc found")
                            #     for edge in edge_list:
                            #         print('collect end edge list is '+hex(edge))
                            #     exit("meet indirect blx")
                            # continue
                        if int(value,16) not in only_called_once_func_addr:
                            shadow_stack.append(i.address+4)
                            # print("push  "+hex(i.address+4))
                        #edge_list.append(i.address)
                        int_value=int(value,16)
                        print(hex(i.address)+"---"+func_addr_map[int_value])
                        edge_list.append(int_value)
                        branch_list.append(i.address)
                        branch_list.append(int_value)
                        fake_stack.append(i.address)
                        fake_stack.append(int_value)
                        recurse_to_hash(opts,mm,md,int_value,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth,branch_list)
                        branch_list.pop()
                        branch_list.pop()
                        fake_stack.pop()
                        fake_stack.pop()
                        if int(value,16) not in only_called_once_func_addr:
                            shadow_stack.pop()
                        #edge_list.pop()
                        edge_list.pop()
                    return
                else:
                    exit("meet conditional blx")
                    for value in sorted(callsite_to_target_map[hex(i.address)]):
                        if int(value,16) not in only_called_once_func_addr:
                            shadow_stack.append(i.address+4)
                            # print("push  "+hex(i.address+4))
                        #edge_list.append(i.address)
                        #print("value is"+value)
                        int_value=int(value,16)
                        print(hex(i.address)+"---"+func_addr_map[int_value])
                        edge_list.append(int_value)
                        fake_stack.append(i.address+4)
                        recurse_to_hash(opts,mm,md,int_value,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth)
                        fake_stack.pop()
                        if int(value,16) not in only_called_once_func_addr:
                            shadow_stack.pop()
                        #edge_list.pop()
                        edge_list.pop()               
                    recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth)
                    return 
            else:
                fake_stack.append(i.address)
                fake_stack.append(i.address+4)
                recurse_to_hash(opts,mm,md,i.address+4,recordpoint_start_addr,edge_list,fake_stack,loop_depth,recursive_depth)
                fake_stack.pop()
                fake_stack.pop()
                return

        # current_address=current_address+4






def analysis(opts):##使用capstone工具
    ##这是测试多线程测试文件的
    # disassembly_file_name='/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/output/threadtest_new_all.S'
    # IR_file_path='/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/output/threadtest_dummy.ll'
    # svf_result_file="/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/pre-analysis/threadtest_target.txt"
    
    #这是测试单线程测试文件的的
    # disassembly_file_name='/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/output/threadtestSingleThread_new_all.S'
    # IR_file_path='/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/output/threadtestSingleThread_addMetadata_dummy.ll'
    # svf_result_file="../output/threadtestSingleThread_target.txt"
    # only_called_once_func_file="/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/only_called_once_func.txt"
    # to_insert_func_file="/home/zrz0517/study/chain_attestation/rt-ateest/zrz-attest/test/to_process_function.txt"
    # abort_func=[]
    # instrument_functions=["__collect_bx_ret","__collect_indirect_call_pred","__collect_indirect_jump_pred","__collect_ldmia_ret",
    #                       "__collect__icall","__collect_direct_call","__collect__icall_shadow_stack",
    #                       "__collect_indirect_call_pred_shadow_stack","__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack"]
    ##测试飞控
    global func_addr_map
    global func_name_map
    global callsite_to_target_map
    #disassembly_file_name="../output/arducopter_output.S"
    disassembly_file_name=opts.disassemble_file
    #IR_file_path='../output/after_insert_dummy.ll'
    IR_file_path=opts.IR_file
    #svf_result_file="../output/arducopter_indirectcall.txt"
    svf_result_file=opts.svf_result_file
    # only_called_once_func_file="../output/only_called_once_func_backup.txt"
    only_called_once_func_file=opts.only_called_once_file
    # to_insert_func_file="../output/ToInsertFunc.txt"
    to_insert_func_file=opts.to_insert_func_file
    map_address_to_funcname(disassembly_file_name,func_name_map)
    func_addr_map=reverse_func_name_map(func_name_map)
    global to_insert_func
    init_set(to_insert_func_file,to_insert_func)

    ignore_functions=set()
    if opts.ignore_functions:
        for func_name in opts.ignore_functions.split(','):
            func_name=func_name.strip()
            if func_name:
                ignore_functions.add(func_name)
    if opts.ignore_functions_file and os.path.isfile(opts.ignore_functions_file):
        init_set(opts.ignore_functions_file,ignore_functions)

    global to_insert_func_addr
    to_insert_func_addr=get_instrument_func_addr(to_insert_func,func_name_map)
    global ignore_func_addr
    ignore_func_addr=set(get_instrument_func_addr(ignore_functions,func_name_map))
    construct_indirectcallsite_to_target_map(svf_result_file,disassembly_file_name,IR_file_path)
    # for key,values in callsite_to_target_map.items():
    #     for value in values:
    #         print(key+"---"+value)
    instrument_functions=["__collect_bx_ret","__collect_indirect_call_pred","__collect_indirect_jump_pred","__collect_ldmia_ret",
                          "__collect__icall","__collect_direct_call","__collect__icall_shadow_stack",
                          "__collect_indirect_call_pred_shadow_stack","__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack","def_check","use_check","Critical_def_check","non_sen_def_check","use_check_for_basic_type_in_struct","def_check_for_basic_type_in_struct","Critical_def_check_for_float","__str_sfi"]
    shadow_instrument_function=["__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack"]#需要对插入影子堆栈的函数做特殊处理，因为他们不想要哈希
    abort_func=["abort@plt","exit@plt","__assert_fail@plt","tcpError"]#当跳转到abort函数的时候直接返回，不然有些包含abort的函数没有ret，所以会导致错误
    if disassembly_file_name == "../output/arducopter_output.S":
        abort_func.append("_ZN6AP_HAL5panicEPKcz")
    avoid_functions=get_avoid_handle_func(disassembly_file_name)
    instrument_functions.extend(avoid_functions)
    branch_func=["__collect__conditional_branch_pred"]
    only_called_once_func=set()
    init_only_called_once_func(only_called_once_func_file,only_called_once_func)
    global only_called_once_func_addr
    only_called_once_func_addr=get_instrument_func_addr(only_called_once_func,func_name_map)
    global instrument_func_addr
    instrument_func_addr=get_instrument_func_addr(instrument_functions,func_name_map)
    # for addr in instrument_func_addr:
    #     print(addr)
    global shadow_instrument_function_addr
    shadow_instrument_function_addr=get_instrument_func_addr(shadow_instrument_function,func_name_map)
    global abort_func_addr
    abort_func_addr=get_instrument_func_addr(abort_func,func_name_map)
    global branch_func_addr
    branch_func_addr=get_instrument_func_addr(branch_func,func_name_map)
    md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
    md.detail=True
    replay_start = False
    replay_stop = False
    analysis_start=False
    taken=True
    target_address=0
    global compute_hash
    with open(opts.binfile,"rb") as f:
        mm=mmap.mmap(f.fileno(),0,prot=mmap.PROT_READ)
        current_address=opts.text_start
        offset=current_address-opts.load_address
        #print(hex(offset))
        print("hooking %s from 0x%08x to 0x%08x" % (opts.binfile, offset, opts.text_end - opts.load_address))
        first_checkpoint=False
        edge_list=[]
        shadow_stack=[]
        branch_list=[]
        global hash_map
        recurse_to_hash(opts,mm,md,current_address,None,edge_list,None,0,0,branch_list)


if __name__ == '__main__':
    main()

        
