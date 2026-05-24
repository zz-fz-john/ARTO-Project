#!/bin/bash
echo "Setting working directory"
cd ../pre-analysis16.0/util/static_analysis ||exit 1
mkdir -p ../output
echo "Merging svf indirect call, llvm-cfi and deep-type results" # Problematic, not executed, svf analysis has been integrated into the previous indirect call analysis
#python3 merge_static_analysis_result.py
echo "Obtaining call subgraph and related functions"
python3 find_func_in_path.py --avoid_handle_function ../../../px4-1.15/build/emlid_navio2_default/avoid_handle_function.txt \
    --mainjob_file_path ../../../px4-1.15/build/emlid_navio2_default/critical_function.txt \
    --recursive_func_file ../output/recursive_header_func.txt \
    --indirect_file ../../virtualcall/output/indirectcall.txt \
    --direct_file ../../virtualcall/output/direct_call_result.txt \
    --ToInsertFuncFile ../output/ToInsertFunc.txt
cp ../output/ToInsertFunc.txt ../../../px4-1.15/build/emlid_navio2_default/ToInsertFunc.txt

cp ../output/ToInsertFunc.txt ../output/ToInsertFunc_px4.txt

cp ../output/recursive_header_func.txt ../../../px4-1.15/build/emlid_navio2_default/recursive_function.txt

cp ../output/only_called_once_func.txt ../../../px4-1.15/build/emlid_navio2_default/only_called_once_func.txt
cp ../output/only_called_once_func.txt ../output/only_called_once_func_px4.txt


cp ../output/leaf_func.txt  ../../../px4-1.15/build/emlid_navio2_default/leaf_func.txt
cd ../../../build_PX4 || exit 1