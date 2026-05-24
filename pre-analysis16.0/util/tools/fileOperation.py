import re
import networkx as nx
import sys
import os
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import demanglecxx
#根据文件初始化函数集合
def init_set(filename,set_name):
    with open(filename,'r') as f:
        for avoid_line in f:
            avoid_func_name=avoid_line.strip()
            set_name.add(avoid_func_name)
def init_only_called_once_func(filename,only_called_once_func_set):
    "初始化只调用一次的函数集合"
    with open(filename,'r') as f:
        for avoid_line in f:
            avoid_func_name=avoid_line.strip()
            func_name=avoid_func_name.split('-')[0]
            only_called_once_func_set.add(func_name)

#根据分析结果，获取只调用一次的函数和它对应的调用指令的编号。
def get_only_called_once_func(file_name,only_called_once_func_map):
    with open(file_name,'r') as file:
        for line in file:
            text=line.strip()
            parts=text.split('-')
            only_called_once_func_map[parts[0]]=(parts[1],parts[2])

##获取函数名到地址的映射
def map_address_to_funcname(disassembly_file_path,func_name_map):
    """获取函数名到地址的映射"""
    function_pattern=re.compile(r"([0-9A-Fa-f]+) <([^>]+)>:")  # 匹配函数名的正则表达式                       
    with open(disassembly_file_path,'r') as file:
        for line in file:
            line=line.strip()
            func_match=function_pattern.match(line)
            if func_match:
                func_addr=func_match.group(1)
                func_name=func_match.group(2)
                func_name_map[func_name]='0x'+func_addr


def get_avoid_handle_func(disassembly_file_name):
    """获取plt的函数名"""
    plt_functions = []
    plt_pattern = re.compile(r"([0-9A-Fa-f]+) <([^>]+@plt)>:")  # Pattern to match functions with @plt suffix
    with open(disassembly_file_name, 'r') as file:
        for line in file:
            line = line.strip()
            plt_match = plt_pattern.match(line)
            if plt_match:
                if plt_match.group(2) =="abort@plt":
                    continue    
                func_name = plt_match.group(2)
                plt_functions.append(func_name)
    return plt_functions


def get_instrument_func_addr(function_list,func_name_map):
    """获取特殊函数的地址"""
    func_addr=[]
    for value in function_list:
        if value not in func_name_map:
            print(f"Warning: Function {value} not found in function name map.")
            continue
        addr=int(func_name_map[value],16)
        func_addr.append(addr)
    return func_addr

def map_address_to_callsite(disassembly_file_path,IR_file_path,callsite_address_map):##建立IR中间接调用指令到二进制程序中地址的映射
    """
    利用objdump -D 指令生成.s文件
    读取.s文件
    若位于新的函数中，记录下函数名和函数地址
    查找该函数内的间接调用指令，并记录下该指令的地址
    查找该指令在IR中对应的callsite
    将callsite和该指令的地址放在一个map中
    """
    current_function=None
    callsite_address=None
    find_funcName=False
    find_callsite=False
    function_pattern = re.compile(r"([0-9A-Fa-f]+) <([^>]+)>:")  # 匹配函数名的正则表达式
    indirect_call_pattern = re.compile(r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+blx\s+\w+")##匹配间接调用指令
    dummy_code_pattern=re.compile(r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F]+\s+movw\s+r8,\s*#(\d+)")##匹配dummy_code
    with open(disassembly_file_path,'r') as file:
        for line in file:
            line=line.strip()
            func_match=function_pattern.match(line)
            if func_match:##找到了函数的起始位置
                current_function=func_match.group(2)
                find_funcName=True
                continue
            if find_funcName:
                indirect_match=indirect_call_pattern.match(line)
                if indirect_match:
                    callsite_address='0x'+indirect_match.group(1)
                    #print(callsite_address)
                    find_callsite=True
                    continue
            if find_callsite and find_funcName:
                dummy_code_match=dummy_code_pattern.match(line)
                if dummy_code_match:
                    dummy_code_value=dummy_code_match.group(2)
                    #print(dummy_code_value)
                    callsite=current_function+'----'+dummy_code_value
                    callsite_address_map[callsite]=callsite_address
                    find_callsite=False
                    #print(callsite_address)
            
def find_callsite_in_IR(file_path,target_line):##找到IR中 间接跳转指令对应的编号
    """
    找到插入的dummycode的下一行
    下一行为间接调用指令
    该间接调用指令的目标集合已经通过svf分析出来了
    需要将他和二进制地址对应起来
    :param file_path ：文件路径,为插入dummy_code的.ll文件
    :param target_line ：目标行的内容,目标行格式形如  call void @llvm.dummy.1._Z12start_threadPv()
    :return :目标行的下一行内容,如果目标行在文件末尾则返回None
    """
    with open(file_path,'r') as file:
        lines=file.readlines()
        for i in range (len(lines)):
            if target_line in lines[i]:
                if i < len(lines):
                    return lines[i-1].strip()
                else:
                    return None
                
def get_analysis_result(result_file):
    """
    #读取间接分析的文件,然后通过文件来得到建立IRcallsite到target function的映射
    """
    svf_result={}
    IRCallsite_pattern=re.compile(r"In function\s*:\s*([^\s]+)\s*indirect callsite\s*:\s*(.*)")
    target_pattern=re.compile(r'--target\s*(\S+)')
    function_name=None
    IR_inst_number=None
    with open(result_file,'r') as file:
        for line in file:
            line=line.strip()
            IRcallsite_match=IRCallsite_pattern.match(line)
            if IRcallsite_match:
                function_name=IRcallsite_match.group(1)
                IR_inst_number=IRcallsite_match.group(2)
                # res2=IR_inst.rfind(")")
                # IR_inst1=IR_inst[:res2+1]
                #print(IR_inst1)
                target_line=function_name+"----"+IR_inst_number
                if target_line not in svf_result:
                    svf_result[target_line]=set()
                continue
            #if Callsite_addr!=None:
            else:
                target_match=target_pattern.match(line)
                if target_match:
                    target_function=target_match.group(1)
                    svf_result[target_line].add(target_function)
    return svf_result

def filt_svf_result_using_demangle(svf_result,demangle_result):
    final_result={}
    for callsite,targets in svf_result.items():
        if callsite not in final_result:
            final_result[callsite]=set()
        if callsite not in demangle_result:##如果不是虚函数调用，则只将svf_result中的结果添加到final_result中
            for target in targets:
                final_result[callsite].add(target)
        else:##如果是虚函数调用，则用demangle_name中的内容对svf——result进行过滤，并将最终结果添加到final_result中
            for target in targets:
                demangle_name=demanglecxx.demangle(target)
                if demangle_name.funcName in demangle_result[callsite]:
                    final_result[callsite].add(target)
    return final_result 

def merge_result(callsite_target_map_use_svf,callsite_target_map_use_meta_data):
    final_result={}
    for callsite,targets in callsite_target_map_use_svf.items():
        if callsite not in final_result:
            final_result[callsite]=set()
        #判断目标集合是否为空,若为空，则添加devirt pass分析的结果，否则，使用通过源代码过滤后的svf结果
        if len(callsite_target_map_use_svf[callsite])==0:           
            if callsite in callsite_target_map_use_meta_data:
                for target in callsite_target_map_use_meta_data[callsite]:
                    final_result[callsite].add(target)
        else:
            for target in targets:
                final_result[callsite].add(target)
    return final_result