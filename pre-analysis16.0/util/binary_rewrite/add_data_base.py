from capstone import *
from keystone import *
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import Section
import struct
import sys
import mmap
import os
import binascii

# Parse input data
def parse_data_file(file_path):
    segments = []
    current_segment = None

    with open(file_path, 'r') as f:
        i=0
        for line in f:
            if i==0:
                i+=1
                continue
            parts = line.strip().split(',')
            if parts[0] == '1':  # Code segment
                element_type=int(parts[0])
                start = int(parts[1], 16)
                end = int(parts[2], 16)
                length = int(parts[3])
                #current_segment = {"type":element_type,'start': start, 'end': end, 'length': length, 'hashes': []}
                current_segment = {'start': start, 'end': end, "type":element_type,'length': length, 'hashes': []}
                segments.append(current_segment)
            elif parts[0] == '2' and current_segment:  # Hash
                #hash_data = bytes.fromhex(parts[1]) 
                hash_data = int(parts[1])
                current_segment['hashes'].append(hash_data)
    return segments
def hex_to_u64_le(hexstr: str):
    v = int(hexstr, 16)
    return struct.pack("<Q", v)

# Find custom section (supports .custom_ro_data)
def find_custom_section(elf_file, section_name):
    with open(elf_file, 'rb') as f:
        elf = ELFFile(f)
        for section in elf.iter_sections():
            if section.name == section_name:
                return section.header['sh_offset'], section.header['sh_size']
    return None, None
# Write data to the custom section of the ELF
def write_to_elf(binary_path, segments, data_offset, data_size):
    with open(binary_path, 'r+b') as f:
        f.seek(data_offset)
        written_size = 0  # To record written data size

        for seg in segments:
            packed_data = struct.pack('<III', seg['start'], seg['end'], seg['type'])  # Code segment info
            if(len(seg["hashes"])%2 != 0):
                packed_data += struct.pack('<I', len(seg['hashes'])+1)  # Number of hashes
                seg["hashes"].append(0)  # Pad with an extra hash value
            else:
                packed_data += struct.pack('<I', len(seg['hashes']))  # Number of hashes
            for h in seg['hashes']:
                if isinstance(h, int):
                    packed_data += struct.pack("<Q", h)
                else:
                    # already bytes
                    packed_data += h

            # Check if segment size is exceeded
            if written_size + len(packed_data) > data_size:
                print(f"Error: Data segment {data_size} bytes is insufficient for {written_size + len(packed_data)} bytes")
                return
            
            f.write(packed_data)
            written_size += len(packed_data)


# Disassemble ELF executable code for verification
def disassemble_elf(binary_path):
    with open(binary_path, 'rb') as f:
        elf = ELFFile(f)
        code_section = elf.get_section_by_name('.text')
        code = code_section.data()
        addr = code_section['sh_addr']
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        for i in md.disasm(code, addr):
            print(f"0x{i.address:x}:\t{i.mnemonic}\t{i.op_str}")

# Run main logic
def main(elf_path, data_path):
    segments = parse_data_file(data_path)
    section_name = ".custom_ro_data"
    data_offset, data_size = find_custom_section(elf_path, section_name)
    if data_offset is None:
        print("Section .custom_ro_data not found")
        return

    print(f"Data segment offset: 0x{data_offset:x}, size: {data_size} bytes")

    write_to_elf(elf_path, segments, data_offset, data_size)

    print("Write complete, starting disassembly verification:")
    #disassemble_elf(elf_path)

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python3 binary_rewriter.py <ELF file path> <data file path>")
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])
