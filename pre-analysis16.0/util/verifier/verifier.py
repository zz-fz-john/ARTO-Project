import argparse
import binascii
import configparser
import logging
import math
import mmap
import os.path
import struct
import sys
import xxhash
sys.setrecursionlimit(30000)
from argparse import Namespace
from bitarray import bitarray
from capstone import *
from capstone.arm import *
from enum import Enum
from datetime import datetime
# from xprint import to_hex,to_x
import subprocess
from elftools.elf.elffile import ELFFile
from pyblake2 import blake2s
import struct
import re
import time
import copy
from concurrent.futures import ThreadPoolExecutor, ProcessPoolExecutor, as_completed
import threading
import multiprocessing
lib_path=".."
sys.path.append(lib_path)
from tools.fileOperation import *
func_name_map={}##key=func_name ,value=func_addr
func_addr_map={}#key=addr,value=func_name
callsite_to_target_map={}##key=callsite_addr,value=target_addr
callsite_address_map={}#key= callsite in IR ,value = callsite_addr
shadow_stack=[]
instrument_func_addr=[]
shadow_instrument_function_addr=[]
abort_func_addr=[]
only_called_once_func_addr=[]
to_insert_func=set()
to_insert_func_addr=[]
hash_map={}
hash_count={}
fake_hash_map={}
record_start_count_map={}
loop_count_map={}
compute_start=False
first_checkpoint=False
is_bcc_flag=0
is_bcc_func_addr=0
branch_index=0
indirect_index=0

branch_segment=[]
indirect_segment=[]


segment_hash_cache = {}

cache_lock = threading.Lock()

def load_segment_cache(cache_file):

    global segment_hash_cache
    import json
    if cache_file and os.path.exists(cache_file):
        try:
            with open(cache_file, 'r') as f:
                segment_hash_cache = json.load(f)
            print(f"Loaded {len(segment_hash_cache)} cached entries from {cache_file}")
        except Exception as e:
            print(f"Failed to load cache from {cache_file}: {e}")
            segment_hash_cache = {}
    else:
        segment_hash_cache = {}

def save_segment_cache(cache_file):

    global segment_hash_cache
    import json
    if cache_file:
        try:
            with open(cache_file, 'w') as f:
                json.dump(segment_hash_cache, f)
            print(f"Saved {len(segment_hash_cache)} cached entries to {cache_file}")
        except Exception as e:
            print(f"Failed to save cache to {cache_file}: {e}")

def compute_segment_hash(branch_trace, indirect_trace, branch_start, branch_end, indirect_start, indirect_end):


    branch_data = branch_trace[branch_start:branch_end] if branch_end else branch_trace[branch_start:]
    indirect_data = indirect_trace[indirect_start:indirect_end] if indirect_end else indirect_trace[indirect_start:]
    

    hasher = xxhash.xxh64()
    

    for b in branch_data:
        hasher.update(struct.pack('B', b))
    
 
    hasher.update(b'\xff\xff\xff\xff')
    

    for addr in indirect_data:
        hasher.update(struct.pack('<Q', addr))
    
    return hasher.hexdigest()

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
    parser.add_argument('-hi', '--hashinfile', dest='hashinfile', default=None,
            help='infile for hash data on prover side')
    parser.add_argument('-ii', '--indirectinfile', dest='indirectinfile', default=None,
            help='if detect attack use it')
    parser.add_argument('-bi', '--branchinfile', dest='branchinfile', default=None,
            help='infile for branch trace data on prover side')
    parser.add_argument('-db','--database_file', dest = 'database_file', default=None,
            help='hash-trace database file path')
    parser.add_argument('-bf','--binary_file', dest = 'binary_file', default=None,
            help='final binary file to analysis')
    parser.add_argument('-c', '--config', dest='config', default=None,
            help='pathname of configuration file')
    parser.add_argument('--cache', '-cache', dest='cache_file', default=None,
            help='path to cache file for persistent segment hash cache')
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
            hashinfile      =args.hashinfile,
            branchinfile      =args.branchinfile,
            database    =args.database_file,
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
            svf_result_file         =   args.svf_result_file,
            IR_file                 =   args.IR_file,
            cache_file              =   args.cache_file,
            indirectinfile          =   args.indirectinfile
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
    if opts.indirectinfile is not None:
        execution_indirect_trace=recover_indirect_trace(opts)
        for addr in execution_indirect_trace:
            print(hex(addr))    
        execution_indirect_trace+=parse_target_buffer_file(opts.indirectinfile)
        execution_branch_trace=recover_branch_trace(opts)
        print(execution_branch_trace)
        analysis_attack(opts,execution_branch_trace,execution_indirect_trace)
    else:
        execution_indirect_trace=recover_indirect_trace(opts)
        execution_branch_trace=recover_branch_trace(opts)
        analysis(opts,execution_branch_trace,execution_indirect_trace)


def get_inst_func(inst_addr):
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
                Callsite_addr=callsite_address_map[target_line]
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
                    target_addr=func_name_map[target_function]
                    ##print(target_addr)
                    callsite_to_target_map[Callsite_addr].add(target_addr)
def parse_target_buffer_file(filepath):

    target_addresses = []
    try:
        with open(filepath, 'rb') as f:
            while True:
                # 读取 4 字节 (uint32_t)
                chunk = f.read(4)
                if not chunk:
                    break
                if len(chunk) < 4:
                    logging.warning(f"Incomplete chunk of {len(chunk)} bytes at end of target_buffer file")
                    break
                
                # 解包为无符号 32 位整数（小端序）
                addr = struct.unpack('<I', chunk)[0]
                target_addresses.append(addr)
        
        logging.info(f"Parsed {len(target_addresses)} target addresses from {filepath}")
        return target_addresses
        
    except FileNotFoundError:
        logging.error(f"Target buffer file not found: {filepath}")
        return []
    except Exception as e:
        logging.error(f"Error parsing target buffer file: {e}")
        return []
def reverse_func_name_map(func_name_map):

    reverse_map={}
    for key in func_name_map.keys():
        value=func_name_map[key]
        reverse_map[int(value,16)]=key
    return reverse_map

loop_count=0

def parse_infile(infile_path):

    trace_data = []
    try:
        with open(infile_path, 'rb') as f:
            while True:
                # Read 8 bytes (uint64)
                chunk = f.read(8)
                if not chunk:
                    break
                if len(chunk) < 8:
                    logging.warning(f"Incomplete chunk of {len(chunk)} bytes at end of file")
                    break
                
                # Unpack as unsigned long long (8 bytes)
                # Assuming little-endian ('<Q') as is common on ARM/x86
                val = struct.unpack('<Q', chunk)[0]
                trace_data.append(val)
                    
        return trace_data
    except Exception as e:
        logging.error(f"Error parsing infile: {e}")
        return []

def parse_hash_db(db_path):

    db = {}
    current_segment = None
    current_hash = None
    try:
        with open(db_path, 'r') as f:
            lines = f.readlines()
            
        for line in lines:
            line = line.strip()
            if not line: continue
            
            parts = line.split(',')
            

            if line.startswith('1,'):

                start = int(parts[1], 16)
                end = int(parts[2], 16)
                current_segment = (start, end)
                if current_segment not in db:
                    db[current_segment] = {}
                    

            elif line.startswith('2,'):

                current_hash = parts[1]
                if current_segment:
                    db[current_segment][current_hash] = []
                    

            else:

                addr = int(line, 16)
                if current_segment and current_hash:
                    db[current_segment][current_hash].append(addr)                
        return db
    except Exception as e:
        logging.error(f"Failed to parse hash db {db_path}: {e}")
        raise
def recover_indirect_trace(opts):

    global indirect_segment
    hash_db_path = opts.database
    hash_trace_infile = opts.hashinfile

    trace_hashes = parse_infile(hash_trace_infile)
    # for(h) in trace_hashes:
    #     print(h)
    logging.info(f"Parsed {len(trace_hashes)} hashes from infile")  

    hash_db = parse_hash_db(hash_db_path)
    logging.info(f"Parsed hash database with {len(hash_db)} segments")

    execution_trace = []
    i=0
    for h in trace_hashes:
        h_str = str(h)

        if h_str == '0':
            indirect_segment.append(i)
            continue  
        

        for segment in hash_db.keys():
            segment_hashes = hash_db[segment]
            if h_str in segment_hashes:
                trace_addresses = segment_hashes[h_str]
                execution_trace.extend(trace_addresses)
                i += len(trace_addresses) 
                break
                #logging.info(f"Matched hash {h_str} in segment {segment}, added {len(trace_addresses)} addresses to trace")
    logging.info(f"Recovered execution trace with {len(execution_trace)} addresses")
    return execution_trace
def recover_branch_trace(opts):

    branch_trace = []
    branch_file = opts.branchinfile
    global branch_segment
    i=0
    try:

        with open(branch_file, 'rb') as f:
            while True:
                b = f.read(1)
                #print(b)
                if not b:
                    break
                if b == b'\x00':
                    branch_trace.append(0)
                    i=i+1
                elif b == b'\x01':
                    branch_trace.append(1)
                    i=i+1
                elif b==b'\x02':
                    branch_segment.append(i)              

        logging.info(f"Parsed {len(branch_trace)} branch decisions from {branch_file}")
        return branch_trace
    except FileNotFoundError:
        logging.error(f"Branch trace file not found: {branch_file}")
        return []
    except Exception as e:
        logging.error(f"Error parsing branch trace file: {e}")
        return []


def worker_process(opts, current_address, branch_trace, indirect_trace, 
                   branch_start, indirect_start, process_id):

    try:
        with open(opts.binfile, "rb") as f:
            mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
            md.detail = True
            
            result = rtoai_verifier(
                opts, mm, md, current_address,
                branch_trace, indirect_trace,
                branch_start=branch_start,
                indirect_start=indirect_start,
                thread_id=process_id
            )
            mm.close()
            return process_id, result
    except Exception as e:
        print(f"[Process {process_id}] Exception: {e}")
        import traceback
        traceback.print_exc()
        return process_id, False

def worker_process_with_hash(opts, current_address, branch_trace, indirect_trace, 
                              branch_start, indirect_start, process_id, seg_hash):

    try:
        with open(opts.binfile, "rb") as f:
            mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
            md.detail = True
            
            result = rtoai_verifier(
                opts, mm, md, current_address,
                branch_trace, indirect_trace,
                branch_start=branch_start,
                indirect_start=indirect_start,
                thread_id=process_id
            )
            mm.close()
            return process_id, result, seg_hash
    except Exception as e:
        print(f"[Process {process_id}] Exception: {e}")
        import traceback
        traceback.print_exc()
        return process_id, False, seg_hash

def rtoai_verifier(opts,mm,md,current_address,branch_trace,indirect_trace,
                   branch_start=0, indirect_start=0, thread_id=0):

    global instrument_func_addr
    global shadow_instrument_function_addr
    global abort_func_addr
    global only_called_once_func_addr
    global to_insert_func_addr
    global func_addr_map
    global is_bcc_func_addr
    

    local_shadow_stack = []
    local_is_bcc_flag = 0
    local_branch_index = branch_start
    local_indirect_index = indirect_start
    
    skip_addresses = {
            opts.collect_start, opts.loop_latch, opts.loop_recordpoint,
            opts.recursive_recordpoint, opts.checkpoint, opts.recursive_latch
        }
    mm.seek(current_address-opts.load_address)
    code=mm.read(mm.size()-mm.tell())
    md.detail = True
    trace_data=branch_trace
    indirect_data=indirect_trace
    
    target_address = current_address
    
    while True:
        file_offset = target_address - opts.load_address
        mm.seek(file_offset)
        code = mm.read(mm.size() - file_offset)
        for i in md.disasm(code, target_address):
            if i.id==ARM_INS_B:
                if i.cc==ARM_CC_AL:
                    target_address=i.operands[0].imm
                    break
                else:
                    if local_is_bcc_flag==1:
                        if local_branch_index < len(trace_data) and 1 == trace_data[local_branch_index]:
                            target_address=i.operands[0].imm
                            local_is_bcc_flag=0
                            local_branch_index+=1
                            break
                        elif local_branch_index < len(trace_data) and 0 == trace_data[local_branch_index]:
                            target_address=i.address+4
                            local_is_bcc_flag=0
                            local_branch_index+=1
                            continue
                        else:
                            print(f"[Thread {thread_id}] Verification failed at instruction 0x{i.address:x}")
                            return False
                    else:
                        target_address=i.operands[0].imm
                        break
            elif i.id==ARM_INS_BL:
                target_address=i.operands[0].imm
                if target_address in is_bcc_func_addr:
                    local_is_bcc_flag=1
                    target_address=i.address+4
                    continue
                if target_address in instrument_func_addr:
                    target_address=i.address+4
                    continue
                elif target_address in abort_func_addr:
                    print(f"[Thread {thread_id}] Reached abort function, verification succeeded for this segment!")
                    return True
                elif target_address == opts.collect_start or \
                    target_address == opts.loop_latch or \
                    target_address == opts.loop_recordpoint or \
                    target_address == opts.recursive_recordpoint or \
                    target_address == opts.checkpoint or \
                    target_address == opts.recursive_latch:
                    target_address=i.address+4
                    continue
                elif target_address ==opts.collect_end:
                    print(f"[Thread {thread_id}] Verification succeeded!")
                    return True
                if i.cc==ARM_CC_AL:
                    target_address=i.operands[0].imm
                    if target_address not in only_called_once_func_addr:
                        local_shadow_stack.append(i.address+4)
                    break
            elif i.id==ARM_INS_BLX:
                if local_indirect_index < len(indirect_data):
                    target_address = indirect_data[local_indirect_index]
                    local_indirect_index+=1
                    local_shadow_stack.append(i.address+4)
                    break
                else:
                    print(f"[Thread {thread_id}] indirect_index out of range")
                    return False
            elif i.id==ARM_INS_BX:
                if(i.operands[0].reg == ARM_REG_LR):
                    if len(local_shadow_stack)==0:
                        print(f"[Thread {thread_id}] shadow stack is empty, verification failed")
                        return False
                    target_address=local_shadow_stack.pop()
                    break
                elif i.operands[0].reg ==ARM_REG_R8:
                    if len(local_shadow_stack)==0:
                        print(f"[Thread {thread_id}] shadow stack is empty, verification failed")
                        return False
                    real_target=local_shadow_stack.pop()
                    if local_indirect_index < len(indirect_data):
                        fake_target=indirect_data[local_indirect_index]
                        local_indirect_index+=1
                        if real_target != fake_target:
                            print(f"[Thread {thread_id}] Verification failed at instruction 0x{i.address:x}")
                            return False
                        target_address=real_target
                        break
                    else:
                        print(f"[Thread {thread_id}] indirect_index out of range")
                        return False
            elif i.id==ARM_INS_POP:
                if i.operands[-1].reg == ARM_REG_PC:
                    if len(local_shadow_stack)==0:
                            print(f"[Thread {thread_id}] shadow stack is empty, verification failed")
                            return False
                    target_address=local_shadow_stack.pop()
                    break
                else:
                    continue
    
    print(f"[Thread {thread_id}] Segment verification completed successfully!")
    return True
        
def rtoai_verifier_optimized(opts, mm, md, current_address, branch_trace, indirect_trace):

    global instrument_func_addr, shadow_instrument_function_addr, abort_func_addr
    global shadow_stack, only_called_once_func_addr, is_bcc_func_addr
    global branch_index, indirect_index, is_bcc_flag


    skip_addresses = {
        opts.collect_start, opts.loop_latch, opts.loop_recordpoint,
        opts.recursive_recordpoint, opts.checkpoint, opts.recursive_latch
    }

    text_start = 0x20000
    text_end = 0x1000000
    load_address = opts.load_address
    

    mm.seek(text_start - load_address)

    full_code_buffer = mm.read(text_end - opts.load_address) 
    buffer_len = len(full_code_buffer)
   #print(f"Loaded code segment: {buffer_len} bytes from 0x{text_start:x} to 0x{text_end:x}")
    

    trace_data = branch_trace
    indirect_data = indirect_trace
    _shadow_stack = shadow_stack
    

    target_address = current_address
    

    md.detail = True

    while True:

        offset = target_address - text_start
        

        if offset < 0 or offset >= buffer_len:
            print(f"Address out of bounds: {hex(target_address)}")
            return False


        code_slice = full_code_buffer[offset:]
        

        iterator = md.disasm(code_slice, target_address)
        
        for i in iterator:

            if i.id == ARM_INS_B:
                if i.cc == ARM_CC_AL:

                    target_address = i.operands[0].imm
                    break 
                else:

                    if is_bcc_flag == 1:
                        ##print(branch_index)
                        taken = trace_data[branch_index]
                        branch_index += 1
                        is_bcc_flag = 0
                        
                        if taken == 1:

                            target_address = i.operands[0].imm
                            break 
                        elif taken == 0:

                            continue 
                        else:
                            print(f"Verification failed at instruction 0x{i.address:x}")
                            exit()
                    else:

                        target_address = i.operands[0].imm
                        break

            elif i.id == ARM_INS_BL:
                target_val = i.operands[0].imm
                
  
                if target_val in is_bcc_func_addr:
                    is_bcc_flag = 1
                    continue 
                
                if target_val in instrument_func_addr:
                    continue
                
                if target_val in skip_addresses:
                    continue 
                
                if target_val in abort_func_addr:
                    return
                
                if target_val == opts.collect_end:
                    print("Verification succeeded!")
                    return True


                if i.cc == ARM_CC_AL:
                    if target_val not in only_called_once_func_addr:
                        _shadow_stack.append(i.address + 4)
                    target_address = target_val
                    break 

            elif i.id == ARM_INS_BLX:
                target_address = indirect_data[indirect_index]
                indirect_index += 1
                _shadow_stack.append(i.address + 4)
                break


            elif i.id == ARM_INS_BX:
                reg = i.operands[0].reg
                if reg == ARM_REG_LR:
                    if not _shadow_stack:
                        print("Shadow stack empty")
                        exit()
                        return False
                    target_address = _shadow_stack.pop()
                    break
                elif reg == ARM_REG_R8:
                    if not _shadow_stack:
                        print("Shadow stack empty")
                        exit()
                        return False
                    real_target = _shadow_stack.pop()
                    fake_target = indirect_data[indirect_index]
                    indirect_index += 1
                    
                    if real_target != fake_target:
                        print(f"Verification failed: real {hex(real_target)} != fake {hex(fake_target)}")
                        exit()
                        return False
                    print(f"Verified BX R8 target: {hex(real_target)}")
                    target_address = real_target
                    break


            elif i.id == ARM_INS_POP:

                if len(i.operands) > 0 and i.operands[-1].reg == ARM_REG_PC:
                    if not _shadow_stack:
                        print("Shadow stack empty")
                        return False
                    target_address = _shadow_stack.pop()
                    break
                else:
                    continue 


            pass
def analysis(opts,execution_branch_trace,execution_indirect_trace):

    global func_addr_map
    global func_name_map
    global callsite_to_target_map
    global branch_segment
    global indirect_segment
    #disassembly_file_name="../output/arducopter_output.S"
    disassembly_file_name=opts.disassemble_file
    #IR_file_path='../output/after_insert_dummy.ll'
    IR_file_path=opts.IR_file
    #svf_result_file="../../virtualcall/output/indirectcall.txt"
    svf_result_file=opts.svf_result_file
    # only_called_once_func_file="../output/only_called_once_func_backup.txt"
    only_called_once_func_file=opts.only_called_once_file
    # to_insert_func_file="../output/ToInsertFunc.txt"
    to_insert_func_file=opts.to_insert_func_file
    map_address_to_funcname(disassembly_file_name,func_name_map)
    func_addr_map=reverse_func_name_map(func_name_map)
    global to_insert_func
    init_set(to_insert_func_file,to_insert_func)
    global to_insert_func_addr
    to_insert_func_addr=get_instrument_func_addr(to_insert_func,func_name_map)
    construct_indirectcallsite_to_target_map(svf_result_file,disassembly_file_name,IR_file_path)
    # for key,values in callsite_to_target_map.items():
    #     for value in values:
    #         print(key+"---"+value)
    instrument_functions=["__collect_bx_ret","__collect_indirect_call_pred","__collect_indirect_jump_pred","__collect_ldmia_ret",
                          "__collect__icall","__collect_direct_call","__collect__icall_shadow_stack",
                          "__collect_indirect_call_pred_shadow_stack","__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack","def_check","use_check","Critical_def_check","non_sen_def_check","use_check_for_basic_type_in_struct","def_check_for_basic_type_in_struct","Critical_def_check_for_float","__str_sfi","def_check_for_ptr_in_struct","use_check_for_ptr_in_struct"]
    shadow_instrument_function=["__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack"]
    is_bcc_function=["__collect__conditional_branch_pred"]
    abort_func=["abort@plt","exit@plt","__assert_fail@plt","tcpError"]
    if disassembly_file_name == "../output/arducopter_output.S":
        abort_func.append("_ZN6AP_HAL5panicEPKcz")
    avoid_functions=get_avoid_handle_func(disassembly_file_name)
    instrument_functions.extend(avoid_functions)
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
    global is_bcc_func_addr
    is_bcc_func_addr=get_instrument_func_addr(is_bcc_function,func_name_map)
    
    for value in execution_indirect_trace:
        print(hex(value))
    

    with open(opts.binfile,"rb") as f:
        binary_data = f.read()
    
    current_address=opts.text_start
    

    num_segments = len(branch_segment)
    

    branch_starts = branch_segment
    indirect_starts = indirect_segment
    

    branch_ends = [x - 1 for x in branch_segment[1:]] + [len(execution_branch_trace)]
    indirect_ends = [x - 1 for x in indirect_segment[1:]] + [len(execution_indirect_trace)]
    
    print(f"Total segments: {num_segments}")
    print(f"Branch starts: {branch_starts}")
    print(f"Indirect starts: {indirect_starts}")
    
    start_time = time.time()
    

    load_segment_cache(opts.cache_file)
    
   
    global segment_hash_cache
    

    segment_hashes = []
    for i in range(num_segments):
        seg_hash = compute_segment_hash(
            execution_branch_trace, execution_indirect_trace,
            branch_starts[i], branch_ends[i],
            indirect_starts[i], indirect_ends[i]
        )
        segment_hashes.append(seg_hash)

    results = []
    segments_to_verify = []  # (segment_id, hash)
    for i in range(num_segments):
        seg_hash = segment_hashes[i]
        if seg_hash in segment_hash_cache:

            cached_result = segment_hash_cache[seg_hash]
            results.append((i, cached_result))
            print(f"[Segment {i}] Cache HIT at zero phase, hash={seg_hash[:16]}...")
        else:

            segments_to_verify.append((i, seg_hash))

    if len(segments_to_verify) ==0:
        end_time = time.time()
        all_passed = all(result for _, result in results)
        if all_passed:
            print("All segments verification succeeded from cache!")
        else:
            failed_segments = [tid for tid, result in results if not result]
            print(f"Verification failed in segments: {failed_segments}")
        print(f"Total time is {(end_time-start_time):.2f} s")
        print(f"Cache size: {len(segment_hash_cache)} entries")
        return

    import random
    num_initial_verify = min(max(num_segments // 50, 3), num_segments)  
    initial_indices = random.sample(range(num_segments), num_initial_verify)
    print(f"Phase 1: Randomly verifying {num_initial_verify} segments to build cache...")
    

    initial_args = []
    for idx in initial_indices:
        initial_args.append((
            opts, current_address,
            execution_branch_trace, execution_indirect_trace,
            branch_starts[idx], indirect_starts[idx], idx, segment_hashes[idx]
        ))
    
    max_workers = min(len(initial_args), os.cpu_count() or 4)
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = [executor.submit(worker_process_with_hash, *args) for args in initial_args]
        for future in as_completed(futures):
            try:
                seg_id, result, seg_hash = future.result()
                if result:
                    segment_hash_cache[seg_hash] = result
                    #print(f"[Phase 1] Segment {seg_id} verified, hash={seg_hash[:16]}... cached")
            except Exception as e:
                print(f"[Phase 1] Exception: {e}")
    
    print(f"Phase 1 complete. Cache now has {len(segment_hash_cache)} unique entries.")
    

    cache_hits = 0
    cache_misses = 0
    

    segments_to_verify = []  # (segment_id, hash)
    cached_results = []      # (segment_id, result)
    initial_set = set(initial_indices) 
    
    for i in range(num_segments):
        seg_hash = segment_hashes[i]
        if i in initial_set:

            cached_results.append((i, segment_hash_cache.get(seg_hash, True)))
            cache_hits += 1
        elif seg_hash in segment_hash_cache:

            cached_result = segment_hash_cache[seg_hash]
            cached_results.append((i, cached_result))
            cache_hits += 1
            #print(f"[Phase 2][Segment {i}] Cache HIT, hash={seg_hash[:16]}...")
        else:

            segments_to_verify.append((i, seg_hash))
            cache_misses += 1
    
    print(f"Phase 2 Cache stats: {cache_hits} hits, {cache_misses} misses (saved {cache_hits - num_initial_verify} verifications)")
    
    results = list(cached_results) 
    

    if segments_to_verify:
        max_workers = min(len(segments_to_verify), os.cpu_count() or 4)
        print(f"Phase 2: Using {max_workers} processes to verify remaining {len(segments_to_verify)} segments")
        

        process_args = []
        for seg_id, seg_hash in segments_to_verify:
            process_args.append((
                opts, current_address,
                execution_branch_trace, execution_indirect_trace,
                branch_starts[seg_id], indirect_starts[seg_id], seg_id, seg_hash
            ))
        
        with ProcessPoolExecutor(max_workers=max_workers) as executor:
            futures = []
            for args in process_args:
                future = executor.submit(worker_process_with_hash, *args)
                futures.append(future)
            

            for future in as_completed(futures):
                try:
                    thread_id, result, seg_hash = future.result()
                    results.append((thread_id, result))
                    

                    if result:
                        segment_hash_cache[seg_hash] = result
                        print(f"[Segment {thread_id}] Verified and cached, hash={seg_hash[:16]}...")
                    else:
                        print(f"[Segment {thread_id}] Verification FAILED")
                        
                except Exception as e:
                    print(f"Process raised exception: {e}")
                    import traceback
                    traceback.print_exc()
                    results.append((None, False))
    
    end_time = time.time()
    

    all_passed = all(result for _, result in results)
    if all_passed:
        print("All segments verification succeeded!")
    else:
        failed_segments = [tid for tid, result in results if not result]
        print(f"Verification failed in segments: {failed_segments}")
    
    print(f"Total time is {(end_time-start_time):.2f} s")
    print(f"Cache size: {len(segment_hash_cache)} entries")
    

    save_segment_cache(opts.cache_file)


def analysis_attack(opts,execution_branch_trace,execution_indirect_trace):

    global func_addr_map
    global func_name_map
    global callsite_to_target_map
    global branch_segment
    global indirect_segment
    #disassembly_file_name="../output/arducopter_output.S"
    disassembly_file_name=opts.disassemble_file
    #IR_file_path='../output/after_insert_dummy.ll'
    IR_file_path=opts.IR_file
    #svf_result_file="../../virtualcall/output/indirectcall.txt"
    svf_result_file=opts.svf_result_file
    # only_called_once_func_file="../output/only_called_once_func_backup.txt"
    only_called_once_func_file=opts.only_called_once_file
    # to_insert_func_file="../output/ToInsertFunc.txt"
    to_insert_func_file=opts.to_insert_func_file
    map_address_to_funcname(disassembly_file_name,func_name_map)
    func_addr_map=reverse_func_name_map(func_name_map)
    global to_insert_func
    init_set(to_insert_func_file,to_insert_func)
    global to_insert_func_addr
    to_insert_func_addr=get_instrument_func_addr(to_insert_func,func_name_map)
    construct_indirectcallsite_to_target_map(svf_result_file,disassembly_file_name,IR_file_path)
    # for key,values in callsite_to_target_map.items():
    #     for value in values:
    #         print(key+"---"+value)
    instrument_functions=["__collect_bx_ret","__collect_indirect_call_pred","__collect_indirect_jump_pred","__collect_ldmia_ret",
                          "__collect__icall","__collect_direct_call","__collect__icall_shadow_stack",
                          "__collect_indirect_call_pred_shadow_stack","__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack","def_check","use_check","Critical_def_check","non_sen_def_check","use_check_for_basic_type_in_struct","def_check_for_basic_type_in_struct","Critical_def_check_for_float","__str_sfi","def_check_for_ptr_in_struct","use_check_for_ptr_in_struct"]
    shadow_instrument_function=["__collect_ldmia_ret_shadow_stack","__collect_bx_ret_shadow_stack"]
    is_bcc_function=["__collect__conditional_branch_pred"]
    abort_func=["abort@plt","exit@plt","__assert_fail@plt","tcpError"]
    if disassembly_file_name == "../output/arducopter_output.S":
        abort_func.append("_ZN6AP_HAL5panicEPKcz")
    avoid_functions=get_avoid_handle_func(disassembly_file_name)
    instrument_functions.extend(avoid_functions)
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
    global is_bcc_func_addr
    is_bcc_func_addr=get_instrument_func_addr(is_bcc_function,func_name_map)
    
    for value in execution_indirect_trace:
        print(hex(value))
    

    with open(opts.binfile,"rb") as f:
        binary_data = f.read()
    current_address=opts.text_start
    with open(opts.binfile, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        md.detail = True
    rtoai_verifier_optimized(opts,mm,md,current_address,execution_branch_trace,execution_indirect_trace)
    mm.close()


if __name__ == '__main__':
    main()