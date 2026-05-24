CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
LLVM_LINK="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-link"
LLVM_OPT=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/opt 
LLVM_DIS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis
LLVM_CHECKPOINT_PASS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMInsertCheckPointsOnLoop.so
LLVM_HEXBOX_PASS="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMHexbox.so"
CLANG="${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/clang"
cd bd
rm -f src/"$1"/"$1"_after_insert_checkpoint.o ||exit 1
$LLVM_LINK \
    -opaque-pointers=0 \
    src/"$1"/*.o \
    support/*.o \
    config/arm/boards/generic/boardsupport.o \
    config/arm/chips/generic/chipsupport.o \
    -o src/"$1"/"$1".bc
$CLANG -O0 -g \
    src/"$1"/"$1".bc \
    -mcpu=cortex-a53 \
    -target arm-linux-gnueabihf \
    -static \
    -o src/"$1"/"$1"_org \
    -lm \
    || exit 1
cd src/"$1" || exit 1

$LLVM_OPT -S -passes=wholeprogramdevirt -whole-program-visibility -wholeprogramdevirt-summary-action=export -opaque-pointers=0 "$1".bc -o after_devirt_"$1".ll

$LLVM_OPT -S  -passes="mergereturn" -opaque-pointers=0 after_devirt_"$1".ll -o after_merge_"$1".ll

$LLVM_OPT -load $LLVM_CHECKPOINT_PASS --Break-Constant-GEPs -enable-new-pm=0 -opaque-pointers=0 after_merge_"$1".ll -o "$1"_breakconstant.bc

$LLVM_DIS -opaque-pointers=0 "$1"_breakconstant.bc -o "$1"_breakconstant.ll

$LLVM_OPT -load $LLVM_CHECKPOINT_PASS  --generate-process-function -enable-new-pm=0 -opaque-pointers=0 "$1"_breakconstant.ll
#at embecn/bd/src/benchmark_name
cd ../../../../pre-analysis16.0/virtualcall/build || exit 1
echo "insert metadata and get direct  call-site result"
./indCallAnalysis --guid --input \
    ../../../embench_test/bd/src/"$1"/"$1"_breakconstant.ll
echo "get target set  of virtual indirect call-site"

mkdir -p ../output
./indCallAnalysis  --input \
   ../../../embench_test/bd/src/"$1"/"$1"_breakconstant.ll

echo "Copy output files"
cp ../output/indirectcall.txt \
    ../../../embench_test/bd/src/"$1"/indirectcall.txt

cp ../output/indirectcall.txt ../output/indirectcall_"$1".txt

cp ../output/direct_call_result.txt \
    ../../../embench_test/bd/src/"$1"/direct_call_result.txt

cp ../output/direct_call_result.txt ../output/direct_call_result_"$1".txt

cp ../output/callsite_target_map.txt \
   ../../../embench_test/bd/src/"$1"/callsite_target_map.txt
cd ../../../embench_test || exit 1

#at embench_test
cd ../pre-analysis16.0/util/static_analysis ||exit 1
mkdir -p ../output
echo "Obtain call subgraph and related functions"
python3 find_func_in_path.py --avoid_handle_function ../../../embench_test/bd/src/"$1"/avoid_handle_function.txt \
    --mainjob_file_path ../../../embench_test/bd/src/"$1"/critical_function.txt \
    --recursive_func_file ../output/recursive_header_func.txt \
    --indirect_file ../../virtualcall/output/indirectcall.txt \
    --direct_file ../../virtualcall/output/direct_call_result.txt \
    --ToInsertFuncFile ../output/ToInsertFunc.txt
cp ../output/ToInsertFunc.txt ../../../embench_test/bd/src/"$1"/ToInsertFunc.txt

cp ../output/ToInsertFunc.txt  ../output/ToInsertFunc_"$1".txt

cp ../output/recursive_header_func.txt ../../../embench_test/bd/src/"$1"/recursive_function.txt
cp ../output/only_called_once_func.txt ../../../embench_test/bd/src/"$1"/only_called_once_func.txt
cp ../output/only_called_once_func.txt  ../output/only_called_once_func_backup_"$1".txt 


cp ../output/leaf_func.txt  ../../../embench_test/bd/src/"$1"/leaf_func.txt 

cd ../../../embench_test || exit 1
cd bd/src/"$1" || exit 1

$LLVM_OPT  --load=$LLVM_HEXBOX_PASS -enable-new-pm=0  -opaque-pointers=0  -HexboxAnaysis --hexbox-analysis-results=./analysis_result.json --hexbox-indirect-call-analysis-filepath=./indirectcall.txt  --hexbox-analysis-direct-callfilepath=./direct_call_result.txt  <./"$1"_breakconstant.ll > "$1"_after_hexbox_analysis.bc ||exit 1

python2 ../../../../pre-analysis16.0/util/graph_analysis/analyzer.py -j=./analysis_result.json -s=../../../sec_mask_result.txt -o=./compartments_result.json -m=operation -b=STM32F479 -T=../../../arm_link_script_syringe.txt -f=./critical_function.txt -L=./arm_link_script_syringe_intermidea.txt

$LLVM_OPT  -load $LLVM_HEXBOX_PASS --hexbox-application -enable-new-pm=0 -opaque-pointers=0  --hexbox-policy=./compartments_result.json ./"$1"_breakconstant.ll -o ./"$1"_after_compartment_llvm_link.bc

$LLVM_DIS -opaque-pointers=0  ./"$1"_after_compartment_llvm_link.bc -o ./"$1"_after_compartment_llvm_link.ll

