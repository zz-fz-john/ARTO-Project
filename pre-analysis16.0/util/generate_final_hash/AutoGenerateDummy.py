def main():
    define_string="#define ASSEMBLY"
    section_string=".section .dummycode , \"axw\""
    global_string=".global "
    dummycode="../output/dummycodeName.txt"
    dummmy_code_name=set()
    with open(dummycode,'r')as file:
        for line in file:
            line=line.strip()
            dummmy_code_name.add(line)
    file.close()
    dummycodefile="../output/dummycode.S"
    with open(dummycodefile,'w') as file:
        file.write(define_string+'\n')
        file.write(section_string+'\n')
        for dummy in dummmy_code_name:
            file.write(global_string+dummy+'\n')
        for dummy in dummmy_code_name:
            file.write(dummy+':'+'\n')
            file.write("\t"+"nop"+'\n')
            file.write('\n')

if __name__ == '__main__':
    main()