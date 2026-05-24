#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
LLVM_OPT="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/opt"
LLVM_DIS="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis"
LLVM_CHECKPOINT_PASS="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMInsertCheckPointsOnLoop.so"
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/aarch32"
cd ../px4-1.15 || exit 1
rm -rf build
source set_compiler.sh
make emlid_navio2

cd build/emlid_navio2_default

$HOME/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang++ -O0 -Wl,-plugin-opt=no-opaque-pointers -Wl,--export-dynamic -fno-exceptions -fno-jump-tables -fuse-ld=lld -flto -Wl,--plugin-opt=emit-llvm -Wl,--lto-whole-program-visibility $(find ./platforms/posix/CMakeFiles/px4.dir/ -type f -name "*.o")  -o px4.bc  $(find ./ -type f -name "*.a")
$LLVM_DIS -opaque-pointers=0 px4.bc   -o px4.ll

$HOME/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang++ -c -o px4.o -x ir px4.bc


$HOME/ARTO/toolchains/aarch32/arm-linux-gnueabihf/bin/ld   -EL -z relro -X --hash-style=gnu --eh-frame-hdr -m armelf_linux_eabi -dynamic-linker /lib/ld-linux-armhf.so.3 -o bin/px4_org $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/Scrt1.o $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/crti.o $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/crtbeginS.o -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9 -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../arm-linux-gnueabihf/lib/../lib -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../lib -L/lib/arm-linux-gnueabihf -L/lib/../lib -L/usr/lib/arm-linux-gnueabihf -L/usr/lib/../lib -L/usr/lib/gcc-cross/arm-linux-gnueabihf/9/../../../../arm-linux-gnueabihf/lib -L/lib -L/usr/lib -plugin $HOME/llvm-arm-cross/llvm-project-16.0.0/build/lib/LLVMgold.so -plugin-opt=mcpu=arm1176jzf-s --gc-sections px4.o -Bdynamic $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libm.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libdl.so.2 $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libstdc++.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/lib/libm.so.6 $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libgcc_s.so.1  $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/libgcc.a -lpthread -lc $TOOLCHAIN_DIR/arm-linux-gnueabihf/lib/libgcc_s.so.1 $TOOLCHAIN_DIR/lib/gcc/arm-linux-gnueabihf/8.2.1/libgcc.a /usr/lib/gcc-cross/arm-linux-gnueabihf/9/crtendS.o $TOOLCHAIN_DIR/arm-linux-gnueabihf/libc/usr/lib/crtn.o 

$HOME/ARTO/toolchains/aarch32/bin/arm-linux-gnueabihf-objdump -d  ./bin/px4_org > ./px4_org.S
echo "Optimize pass to avoid unnecessary indirect calls and remove function aliases"
$LLVM_OPT -S  -opaque-pointers=0 -passes="globalopt" -o px4_modify.ll px4.ll

echo "Use whole-program devirtualization pass to remove virtual function calls with only one target" 
$LLVM_OPT -S -passes=wholeprogramdevirt -whole-program-visibility -wholeprogramdevirt-summary-action=export -opaque-pointers=0 px4_modify.ll -o after_devirt.ll
 
echo "Merge multiple Ret in functions to facilitate correspondence with statements in svf later" 
$LLVM_OPT -S  -passes="mergereturn" -opaque-pointers=0 after_devirt.ll -o after_merge.ll


echo "Unfold nested GEP structures in IR to facilitate correspondence with statements in svf later, and identify key operations"
$LLVM_OPT -load $LLVM_CHECKPOINT_PASS --Break-Constant-GEPs -enable-new-pm=0 -opaque-pointers=0 after_merge.ll -o px4_breakconstant.bc

$LLVM_DIS -opaque-pointers=0 px4_breakconstant.bc -o px4_breakconstant.ll


echo "Generate avoid_handle_function.txt and to_process_function.txt"
$LLVM_OPT -load $LLVM_CHECKPOINT_PASS  --generate-process-function -enable-new-pm=0 -opaque-pointers=0 px4_breakconstant.ll

echo "Completed preprocessing of IR"

echo "✅ Build completed!"
cd ../../../build_PX4 || exit 1