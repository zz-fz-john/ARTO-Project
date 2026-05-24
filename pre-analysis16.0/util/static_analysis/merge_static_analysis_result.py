import binascii
import logging
import math
import mmap
import os.path
import struct
import sys
from argparse import Namespace
from bitarray import bitarray
from capstone import *
from capstone.arm import *
from enum import Enum
from datetime import datetime
from xprint import to_hex,to_x
import subprocess
from elftools.elf.elffile import ELFFile
import struct
import re
import angr
import cxxfilt
import sys
sys.path.append("..")
lib_path=".."
sys.path.append(lib_path)
from tools import demanglecxx
from tools.fileOperation import *


  
def main():
    demangle_file="../../virtualcall/output/callsite_target_map.txt"##
    meta_data_file="../../virtualcall/output/indirectcall.txt"

    svf_result_file="../../svf_analysis_result/ander-arducopter16_devirt-svf-patched_3_type_match.txt"
    final_file="../output/static_final_result.txt"
    callsite_target_map_use_meta_data=get_analysis_result(meta_data_file)
    callsite_target_map_use_svf=get_analysis_result(svf_result_file)
    demangle_result=get_analysis_result(demangle_file)
    after_filt_result=filt_svf_result_using_demangle(callsite_target_map_use_svf,demangle_result)#
    final_result=merge_result(after_filt_result,callsite_target_map_use_meta_data)
    with open(final_file,'w') as file:
        for callsite,targets in final_result.items():
            res=callsite.split("----")
            functionName=res[0]
            IR_callsite=res[1]
            file.write("In function :"+functionName+"  indirect callsite :   "+IR_callsite+'\n')
            for target in targets:
                file.write("--target  "+target+'\n')
    file.close()
if __name__ == '__main__':
    main()