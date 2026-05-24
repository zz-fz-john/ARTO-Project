#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
echo "Current directory is: $CURRENT_DIR"
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/aarch32"
echo "Compiling measurement files"
echo "Compiling measurement-engine..."
cd ${ROOT_DIR}/measurement-engine/first-measure-in-secure-world/runtime-measure/ || { echo "Failed to enter measurement directory"; exit 1; }
make first_measure|| { echo "Make failed"; exit 1; }

echo " Compiling embench test program $1"
echo "Compiling "$1" "
cd ../../../embench_test
cp sec_mask_result.txt bd/src/"$1"/
cd bd/src/"$1"/ || { echo "Failed to enter  "$1" "; exit 1; }

echo "Compiling with llc"
LLVM_LLC="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llc"
LLVM_LIB="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib"
echo "Running llc to compile "$1"_after_insert_dummy.ll..."

$LLVM_LLC -O3 -filetype=obj  -march=arm "$1"_after_insert_dummy.ll -o "$1"_after_insert_checkpoint.o 2>&1 || { echo "llc failed"; exit 1; }
rm -f "$1"_after_insert_checkpoint.S
$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -d  "$1"_after_insert_checkpoint.o > "$1"_after_insert_checkpoint.S || { echo "objdump failed"; exit 1; }
echo "llc compilation completed, starting linking"

${ROOT_DIR}/toolchains/aarch32/arm-linux-gnueabihf/bin/ld -T arm_link_script_syringe_intermidea.txt  -EL -z relro -X --hash-style=gnu --eh-frame-hdr -m armelf_linux_eabi -dynamic-linker /lib/ld-linux-armhf.so.3 -o "$1" $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/Scrt1.o $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/crti.o $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/crtbeginS.o -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9 -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../arm-linux-gnueabihf/lib/../lib -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../lib -L/lib/arm-linux-gnueabihf -L/lib/../lib -L/usr/lib/arm-linux-gnueabihf -L/usr/lib/../lib -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../arm-linux-gnueabihf/lib -L/lib -L/usr/lib -plugin $LLVM_LIB/LLVMgold.so -plugin-opt=mcpu=arm1176jzf-s --gc-sections -Bstatic "$1"_after_insert_checkpoint.o  ../../../../measurement-engine/first-measure-in-secure-world/output/trampoline.o ../../../../measurement-engine/first-measure-in-secure-world/output/CFeventSingleThread.o ../../../../measurement-engine/first-measure-in-secure-world/output/data_flow.o ../../../../measurement-engine/first-measure-in-secure-world/output/heap_section.o  ../../../../measurement-engine/first-measure-in-secure-world/output/xxhash3.o  ../../../../measurement-engine/first-measure-in-secure-world/output/dummycode.o -Bdynamic $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libm.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libdl.so.2 $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libstdc++.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libm.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libgcc_s.so.1  $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/libgcc.a -lresolv -lpthread -lc $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libgcc_s.so.1 $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/libgcc.a /usr/lib/gcc-cross/arm-linux-gnueabihf/9/crtendS.o $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/crtn.o 

rm -f ./"$1".S
$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -d  ./"$1" > ./"$1".S
echo "Script execution completed successfully."

echo "Replacing return instructions of functions called only once with absolute addresses"
cd ../../../../pre-analysis16.0/util/binary_rewrite/ || { echo "Failed to enter binary rewrite directory"; exit 1; }
rm -f ../output/"$1"_output

python3 binary_rewrite.py --binary_path ${ROOT_DIR}/embench_test/bd/src/"$1"/"$1" --disassembly_file_name ${ROOT_DIR}/embench_test/bd/src/"$1"/"$1".S --only_called_once_func_file ../output/only_called_once_func_backup_"$1".txt --ToInsertFuncFile ../output/ToInsertFunc_"$1".txt --output_binary_path ../output/"$1"_output || { echo "Binary rewrite failed"; exit 1; }

$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -d  ../output/"$1"_output > ../output/"$1"_output.S
echo "Script execution completed successfully"  