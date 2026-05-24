import argparse
import binascii
import configparser
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
from pyblake2 import blake2s
import struct
import re
checkpoints_to_hash_map={}
SKIP_HASH_VALUE = "3244421341483603138"
section_counts = {}
section_banned_seen = {}
section_allowed_seen = {}

def filt_repeat_hash(file_path):
    global checkpoints_to_hash_map
    global section_counts, section_banned_seen, section_allowed_seen
    #checkpoint_pattern=re.compile(r'1,([0-9A-Fa-f]+),([0-9A-Fa-f]+)')
    #hash_pattern=re.compile(r'2,([0-9A-Fa-f]+)')
    with open(file_path,'r') as file:
        lines=file.readlines()
        for i in range (len(lines)):
            #res=checkpoint_pattern.match(lines[i])
            res=lines[i].split(',')
            if res[0]=='1':
                checkpointSection=res[1]+','+res[2].strip()
                if checkpointSection not in  checkpoints_to_hash_map:
                    checkpoints_to_hash_map[checkpointSection]=set()
                
                section_counts[checkpointSection] = section_counts.get(checkpointSection, 0) + 1
                # Initialize flags
                if checkpointSection not in section_banned_seen:
                    section_banned_seen[checkpointSection] = False
                if checkpointSection not in section_allowed_seen:
                    section_allowed_seen[checkpointSection] = False
                # recordpoint_res=lines[i+1].split(',')
                hash_res=lines[i+1].split(',')
                if hash_res[0]=='2':
                    hash_string=hash_res[1].strip()
                   
                    if hash_string == SKIP_HASH_VALUE:
                        section_banned_seen[checkpointSection] = True
                       
                    else:
                        checkpoints_to_hash_map[checkpointSection].add(hash_string)
                        section_allowed_seen[checkpointSection] = True
            #     if recordpoint_res[0]=='2':
            #         recordpointsection=recordpoint_res[1]+","+recordpoint_res[2].strip()
            #         if recordpointsection not in checkpoints_to_hash_map[checkpointSection]:
            #             checkpoints_to_hash_map[checkpointSection][recordpointsection]=set()
            #         hash_res=lines[i+2].split(',')
            #         if hash_res[0]=='3':
            #             hash_string=hash_res[1].strip()
            #             checkpoints_to_hash_map[checkpointSection][recordpointsection].add(hash_string)                    
            #     # else:
            #     #     hash_res=lines[i+1].split(',')
            #     #     if hash_res[0]=='2':
            #     #         hash_string=hash_res[1].strip()
            #     #         checkpoints_to_hash_map[checkpointSection].add(hash_string)
            # # if res!= None:
            # #     checkpointSection=res.group(1)+','+res.group(2)
            # #     print(checkpointSection)
            # #     if not checkpoints_to_hash_map.has_key(checkpointSection):
            # #         checkpoints_to_hash_map[checkpointSection]=set()
            # #     else:
            # #         hash_match=hash_pattern.match(lines(i+1))
            # #         if hash_match!= None:
            # #             hash_string=hash_match.group(1)
            # #             checkpoints_to_hash_map[checkpointSection].add(hash_string)
            # # else:
            # #     continue

def build_hash_edges_map(file_path):
    """
    Parse inputfile, mapping each hash value to the list of jump edge addresses that follow it.
    Rules:
    - Line starts with '1,': New checkpoint section, reset current hash
    - Line starts with '2,': Hash definition, start collecting subsequent edges (until next '1,' or new '2,')
    - Other lines: If current hash is valid, consider it an edge address, add to that hash's list
    - Skip hash specified by SKIP_HASH_VALUE
    Returns: dict[str, list[str]], key is hash string, value is list of edge address strings
    """
    mapping = {}
    current_hash = None
    with open(file_path, 'r') as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split(',')
            if parts[0] == '1':
                current_hash = None
                continue
            elif parts[0] == '2':
                h = parts[1].strip()
                if h == SKIP_HASH_VALUE:
                    current_hash = None  # Skip this hash
                else:
                    current_hash = h
                    if current_hash not in mapping:
                        mapping[current_hash] = []
            else:
                # Considered an edge address (e.g., 0x20978)
                if current_hash is not None:
                    mapping[current_hash].append(line)
    return mapping

def write_hash_edges_map(mapping, out_path):
    """Write hash->edge mapping to file, line format: hash,edge_addr"""
    with open(out_path, 'w') as f:
        for h, edges in mapping.items():
            for e in edges:
                f.write(f"{h},{e}\n")

def main():
    global checkpoints_to_hash_map
    parser = argparse.ArgumentParser(description='generate final hash database')
    parser.add_argument('-i','--inputfile',dest='inputfile',default=None,
			help='input hash file')
    parser.add_argument("-o",'--outfile',dest='outfile',default=None,
			help='out file name')
    parser.add_argument("-eo",'--edges_out',dest='edges_out',default=None,
            help='output file for hash-edge mapping')
    args=parser.parse_args()
    opts=Namespace(
		inputfile=args.inputfile,
        outfile=args.outfile,
        edges_out=args.edges_out
    )
    hash_count=len(checkpoints_to_hash_map.keys()) # Get how many checkpoints there are
    filt_repeat_hash(opts.inputfile)
    # If a checkpointSection has only one hash and that hash is the value to be skipped, delete the section
    for key in list(checkpoints_to_hash_map.keys()):
        only_once = section_counts.get(key, 0) == 1
        has_only_banned = (section_banned_seen.get(key, False) is True) and (section_allowed_seen.get(key, False) is False)
        if only_once and has_only_banned:
            del checkpoints_to_hash_map[key]
    
    with open(opts.outfile,'w') as file:
        key_num=len(checkpoints_to_hash_map.keys())
        file.write(str(key_num)+'\n')
        for key,values in  checkpoints_to_hash_map.items():
            file.write('1,' + key + ',' + str(len(values)) + '\n')
            for hash_string in values:
                file.write('2,' + hash_string + '\n')
            
            
            # value_num=len(values)
            # file.write('1,'+key+','+str(value_num)+'\n')         
            # for value in values:
            #     file.write('2,'+value+'\n')

    # Generate and write out the hash->edge mapping (if output path is provided)
    if opts.edges_out:
        edges_map = build_hash_edges_map(opts.inputfile)
        write_hash_edges_map(edges_map, opts.edges_out)
    

if __name__ == '__main__':
    main()