#!/bin/bash
#run on rpi3
 
#scp branch_trace.txt target_buffer.txt hash_value.txt <your name>@192.168.1.100:~/ARTO/embench_test/   
cd ../pre-analysis16.0/util/verifier/ || { echo "Failed to enter verifier directory"; exit 1; }
python3 ../generate_final_hash/gen_config.py ../output/"$1"_output.S \
    --binary-file ../output/"$1"_output \
    --program-header "$1".ph \
    -o ../output/replay_single.cfg

python3 ./verifier.py \
    -bf ../output/"$1"_output \
    -hi ../../../embench_test/hash_value.txt \
    -bi ../../../embench_test/branch_trace.txt \
    -ii ../../../embench_test/target_buffer.txt \
    -dis ../output/"$1"_output.S  ../output/"$1"_output\
    -db ../output/hash_database_single_on_server_"$1".txt \
    -IR ../output/after_insert_dummy_"$1".ll \
    -svf ../../virtualcall/output/indirectcall_"$1".txt \
    -tiff ../output/ToInsertFunc_"$1".txt \
    -ocof ../output/only_called_once_func_backup_"$1".txt \
    --cache ./segment_cache.json \
     -c ../output/replay_single.cfg  -v -v -v