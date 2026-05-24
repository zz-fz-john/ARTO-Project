#!/bin/bash
#use to get bc file
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
echo "Running build_test_step_0.sh script..."
LLVM_OPT=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/opt 

LLVM_DIS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis

LLVM_CHECKPOINT_PASS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMInsertCheckPointsOnLoop.so
echo "Current directory is: $CURRENT_DIR"
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/aarch32"

mkdir -p build/"$1"/
cd build/"$1"

${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis "$1".bc -o "$1".ll || exit 1
${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/clang "$1".ll -O3 -target arm-linux-gnueabihf  -lm -lresolv  -lpthread -o "$1"_test_combo ||exit 1

$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -d  "$1"_test_combo > "$1"_test_combo.S || exit 1

$LLVM_OPT -S -passes=wholeprogramdevirt -whole-program-visibility -wholeprogramdevirt-summary-action=export -opaque-pointers=0 "$1".bc -o after_devirt.ll ||exit 1


$LLVM_OPT -S  -passes="mergereturn" -opaque-pointers=0 after_devirt.ll -o after_merge.ll || exit 1
$LLVM_OPT -load $LLVM_CHECKPOINT_PASS --Break-Constant-GEPs -enable-new-pm=0 -opaque-pointers=0 after_merge.ll -o test_combo_breakconstant.bc ||exit 1

$LLVM_DIS -opaque-pointers=0 test_combo_breakconstant.bc -o test_combo_breakconstant.ll ||exit 1


$LLVM_OPT -load $LLVM_CHECKPOINT_PASS  --generate-process-function -enable-new-pm=0 -opaque-pointers=0 test_combo_breakconstant.ll ||exit 1

echo "Completed IR preprocessing"
cd ../..

