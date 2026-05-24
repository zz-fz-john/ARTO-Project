#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
LLVM_OPT="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/opt"
LLVM_DIS="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis"
LLVM_HEXBOX_PASS="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMHexbox.so"
echo "set work dir to  ${LLVM_HEXBOX_PASS}"
cd "$1"/build || exit 1
$LLVM_OPT  --load=$LLVM_HEXBOX_PASS -enable-new-pm=0  -opaque-pointers=0  -HexboxAnaysis --hexbox-analysis-results=./analysis_result.json --hexbox-indirect-call-analysis-filepath=./indirectcall.txt  --hexbox-analysis-direct-callfilepath=./direct_call_result.txt  <./test_combo_breakconstant.ll > after_hexbox_analysis.bc ||exit 1

python2 ../../../pre-analysis16.0/util/graph_analysis/analyzer.py -j=./analysis_result.json -s=../../sec_mask_result.txt -o=./compartments_result.json -m=operation -b=STM32F479 -T=../../arm_link_script_syringe.txt -f=./critical_function.txt -L=./arm_link_script_syringe_intermidea.txt ||exit 1

$LLVM_OPT   -load $LLVM_HEXBOX_PASS --hexbox-application -enable-new-pm=0 -opaque-pointers=0  --hexbox-policy=./compartments_result.json ./test_combo_breakconstant.ll -o ./after_compartment_llvm_link.bc ||exit 1
$LLVM_DIS  -opaque-pointers=0  ./after_compartment_llvm_link.bc -o ./after_compartment_llvm_link.ll ||exit 1
cd ../ || exit 1