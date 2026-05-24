import argparse
import sys
import re
import os
from elftools.elf.elffile import ELFFile

def get_load_address(binfile):
    with open(binfile, "rb") as f:
        elf = ELFFile(f)
        for segment in elf.iter_segments():
            if segment.header.p_type == "PT_LOAD":  
                return segment.header.p_vaddr  
    return None
DEFAULT_OUTPUT_CONFIG = 'output.cfg'
CONFIG_DEFAULTS={
    'load_address'   		:'0x00000',
    'text_start'     		:'0x400430',
    'text_end'       		:'0x400818',
    'omit_addresses' 		:'0x000000',
    'collect_end'    		:'0x000000',
    'collect_start'  		:'0x000000',
	'loop_recordpoint'    	:'0x000000',
	'recursive_recordpoint'	:'0x000000',
	'recursive_latch'		:'0x000000',
	'loop_latch'			:'0x000000',	
	'checkpoint' 			: '0x00000',
}
def extract_text_start_and_end_from_ph(phfile):
	text_start = ''
	text_end = ''

	# text_start_re = re.compile('^\s+\[[\s\d]+\]\s\.text\s+PROGBITS\s+0000000000([0-9a-f]{6})\s+[0-9a-f]+$')
	# text_end_re = re.compile('^\s+\[[\s\d]+\]\s\.fini\s+PROGBITS\s+0000000000([0-9a-f]{6})\s+[0-9a-f]+$')
	text_start_re = re.compile('^\s+\[[\s\d]+\]\s\.text\s+PROGBITS\s+([0-9a-f]{8})\s+([0-9a-f]{6})\s+[0-9a-f]+')
	text_end_re = re.compile('^\s+\[[\s\d]+\]\s\.fini\s+PROGBITS\s+([0-9a-f]{8})\s+([0-9a-f]{6})\s+[0-9a-f]+')

	ph = open(phfile, 'r')
	lines = ph.readlines()
	for line in lines:
		res = text_start_re.match(line)
		if res != None:
			text_start = '0x' + res.group(1)

		res = text_end_re.match(line)
		if res != None:
			text_end = '0x' + res.group(1)
	return text_start, text_end

if __name__ == '__main__':
	parser = argparse.ArgumentParser(description='Extract Trampoline Function Address From Objdump File')
	parser.add_argument("--binary-file",dest='binary_file',default=None,help='binary file to extract')
	parser.add_argument('file', nargs='?', metavar='FILE',
		help='objdump file to extract')
	parser.add_argument('--program-header', dest='ph', default=None,
		help='program header file to extract text start and text end')
	parser.add_argument('-L', '--load-address', dest='load_address', default=None,
		help='load address of binary image')
	parser.add_argument('--text-start', dest='text_start', default=None,
		help='start address of section to instrument')
	parser.add_argument('--text-end', dest='text_end', default=None,
		help='end address of section to instrument')
	parser.add_argument('-o', '--outfile', dest='outfile', default=None,
		help='outfile for branch table')
	args = parser.parse_args()
	if args.file is None:
		sys.exit("%s: lack of input file!" % (sys.argv[0]))
	elif not os.path.isfile(args.file):
		sys.exit("%s: file '%s' not found" % (sys.argv[0], args.file))
	dumpfile = args.file#
	lines = open(dumpfile, 'r').readlines()
	outfile = args.outfile if args.outfile != None else DEFAULT_OUTPUT_CONFIG
	ofd = open(outfile, 'w')
	phfile = args.ph if args.ph != None else None
	# if phfile is not None:
	# 	text_start, text_end = extract_text_start_and_end_from_ph(phfile)
	# 	CONFIG_DEFAULTS['text_start'] = text_start
	# 	CONFIG_DEFAULTS['text_end'] = text_end


	if args.load_address is not None:
		CONFIG_DEFAULTS['load_address'] = args.load_address
	if args.text_start is not None:
		CONFIG_DEFAULTS['text_start'] = args.text_start
	if args.text_end is not None:
		CONFIG_DEFAULTS['text_end'] = args.text_end

	bl_cfa_init = re.compile(r'\s*([0-9a-fA-F]+):\s*([0-9a-fA-F]{8})\s+bl\s+([0-9a-fA-F]+)\s+<start_collecting>')
	#bl_cfa_init = re.compile('^\s+[0-9a-f]{6}\:\s+[0-9a-f]+\s+bl\s+([0-9a-f]+)\s+<start_collecting>')
	bl_cfa_quote = re.compile(r'\s*([0-9a-fA-F]+):\s*([0-9a-fA-F]{8})\s+bl\s+([0-9a-fA-F]+)\s+<end_collecting>')
	loop_recordpoint=re.compile(r"([0-9A-Fa-f]+) <loop_recordpoint>:")
	recursive_recordpoint=re.compile(r"([0-9A-Fa-f]+) <recursive_recordpoint>:")
	checkpoint=re.compile(r"([0-9A-Fa-f]+) <checkpoint>:")
	recursive_latch=re.compile(r"([0-9A-Fa-f]+) <ret_recursive_recordpoint>:")
	loop_latch=re.compile(r"([0-9A-Fa-f]+) <loop_latch>:")
	omit_addresses = '0x000000'

	for line in lines:
		res = bl_cfa_init.match(line)
		if res != None:
			CONFIG_DEFAULTS['text_start']='0x'+res.group(1)
			CONFIG_DEFAULTS['collect_start'] = '0x' + res.group(3)
			print('0x' + res.group(1))
		res= bl_cfa_quote.match(line)
		if res != None:
			CONFIG_DEFAULTS['text_end']='0x'+res.group(1)
			CONFIG_DEFAULTS['collect_end'] = '0x' + res.group(3)
		res=loop_recordpoint.match(line)
		if res!= None:
			CONFIG_DEFAULTS['loop_recordpoint'] = '0x' + res.group(1)
		res=checkpoint.match(line)
		if res != None:
			CONFIG_DEFAULTS['checkpoint']='0x' +res.group(1)
		res=recursive_recordpoint.match(line)
		if res!=None:
			CONFIG_DEFAULTS['recursive_recordpoint']='0x'+res.group(1)
		res=recursive_latch.match(line)
		if res!=None:
			CONFIG_DEFAULTS['recursive_latch']='0x'+res.group(1)
		res=loop_latch.match(line)
		if res!=None:
			CONFIG_DEFAULTS["loop_latch"]='0x'+res.group(1)


	print('omit_addresses:' + omit_addresses)
	load_address=get_load_address(args.binary_file)
	CONFIG_DEFAULTS['omit_addresses'] = omit_addresses
	CONFIG_DEFAULTS['load_address']=hex(load_address)
	ofd.write('[code-addresses]\n')
	ofd.write('load_address   			= %s\n' % CONFIG_DEFAULTS['load_address'])
	ofd.write('text_start     			= %s\n' % CONFIG_DEFAULTS['text_start'])
	ofd.write('text_end       			= %s\n' % CONFIG_DEFAULTS['text_end'])
	ofd.write('collect_start  			= %s\n' % CONFIG_DEFAULTS['collect_start'])
	ofd.write('collect_end    			= %s\n' % CONFIG_DEFAULTS['collect_end'])
	ofd.write('loop_recordpoint		 	= %s\n' % CONFIG_DEFAULTS['loop_recordpoint'] )
	ofd.write('recursive_recordpoint	= %s\n' % CONFIG_DEFAULTS['recursive_recordpoint'])
	ofd.write('recursive_latch			= %s\n' % CONFIG_DEFAULTS['recursive_latch'])
	ofd.write('omit_addresses 			= %s\n' % CONFIG_DEFAULTS['omit_addresses'])
	ofd.write('loop_latch				= %s\n' % CONFIG_DEFAULTS['loop_latch'])
	ofd.write('checkpoint 				= %s\n' % CONFIG_DEFAULTS['checkpoint'])
	ofd.write('\n')

	ofd.close()