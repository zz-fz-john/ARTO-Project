#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
echo "Setting working directory"
cd ../pre-analysis16.0/virtualcall/build ||exit 1
echo "insert metadata and get direct  call-site result"
./indCallAnalysis --guid --input \
    ${ROOT_DIR}/px4-1.15/build/emlid_navio2_default/px4_breakconstant.ll
echo "get target set  of virtual indirect call-site"
mkdir -p ../output
./indCallAnalysis  --input \
    ${ROOT_DIR}/px4-1.15/build/emlid_navio2_default/px4_breakconstant.ll
echo "copy"
cp ../output/indirectcall.txt \
    ../../../px4-1.15/build/emlid_navio2_default/

cp ../output/indirectcall.txt ../output/indirectcall_px4.txt

cp ../output/direct_call_result.txt \
    ../../../px4-1.15/build/emlid_navio2_default/
cp ../output/direct_call_result.txt ../output/direct_call_result_px4.txt

cp ../output/callsite_target_map.txt \
   ../../../px4-1.15/build/emlid_navio2_default/
cd ../../../build_PX4 || exit 1