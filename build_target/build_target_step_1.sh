CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
LLVM_OPT=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/opt 

LLVM_DIS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/bin/llvm-dis

LLVM_CHECKPOINT_PASS=${ROOT_DIR}/llvm-16.0/llvm-project-16.0.0/build/lib/LLVMInsertCheckPointsOnLoop.so
echo "get indirct call target set"
cd ../pre-analysis16.0/virtualcall/build || exit 1
echo "insert metadata and get direct  call-site result"
./indCallAnalysis --guid --input \
    ${ROOT_DIR}/build_target/build/"$1"/test_combo_breakconstant.ll ||exit 1
echo "get target set  of virtual indirect call-site"
mkdir -p ../output
./indCallAnalysis  --input \
    ${ROOT_DIR}/build_target/build/"$1"/test_combo_breakconstant.ll || exit 1
echo "copy output files"
cp ../output/indirectcall.txt \
    ../../../build_target/build/"$1"

cp ../output/indirectcall.txt ../output/indirectcall_"$1".txt

cp ../output/direct_call_result.txt \
    ../../../build_target/build/"$1"

cp ../output/direct_call_result.txt ../output/direct_call_result_"$1".txt

cp ../output/callsite_target_map.txt \
   ../../../build_target/build/"$1"
cd ../../../build_target || exit 1