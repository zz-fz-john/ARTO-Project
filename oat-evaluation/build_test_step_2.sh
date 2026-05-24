cd ../pre-analysis16.0/util/static_analysis ||exit 1
mkdir -p ../output
echo "Merging results of SVF indirect call, llvm-cfi and deep-type" # Issue: not executing, SVF analysis has been integrated into the previous indirect call analysis
#python3 merge_static_analysis_result.py
echo "Obtain call subgraph and related functions"
python3 find_func_in_path.py --avoid_handle_function ../../../oat-evaluation/"$1"/build/avoid_handle_function.txt \
    --mainjob_file_path ../../../oat-evaluation/"$1"/build/critical_function.txt \
    --recursive_func_file ../output/recursive_header_func.txt \
    --indirect_file ../../virtualcall/output/indirectcall.txt \
    --direct_file ../../virtualcall/output/direct_call_result.txt \
    --ToInsertFuncFile ../output/ToInsertFunc.txt ||exit 1
cp ../output/ToInsertFunc.txt ../../../oat-evaluation/"$1"/build/ToInsertFunc.txt
cp ../output/ToInsertFunc.txt ../output/ToInsertFunc_"$1".txt


cp ../output/recursive_header_func.txt ../../../oat-evaluation/"$1"/build/recursive_function.txt

cp ../output/only_called_once_func.txt ../../../oat-evaluation/"$1"/build/only_called_once_func.txt

cp ../output/only_called_once_func.txt ../output/only_called_once_func_"$1".txt

cp ../output/leaf_func.txt  ../../../oat-evaluation/"$1"/build/leaf_func.txt
