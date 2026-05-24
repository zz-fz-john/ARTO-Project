#!/bin/bash

cd ../pre-analysis16.0/util/verifier/ || { echo "Failed to enter verifier directory"; exit 1; }
python3 ../generate_final_hash/gen_config.py ../output/"$1"_output.S \
    --binary-file ../output/"$1"_output \
    --program-header "$1"_output.ph \
    -o ../output/"$1"_replay_single.cfg

python3 ./verifier.py \
    -bf ../output/"$1"_rewrite \
    -hi ../../../oat-evaluation/"$1"/build/hash_value.txt \
    -bi ../../../oat-evaluation/"$1"/build/branch_trace.txt \
    -dis ../output/"$1"_output.S  ../output/"$1"_rewrite\
    -db ../output/hash_database_single_on_server_"$1".txt \
    -IR ../output/after_insert_dummy_"$1".ll \
    -svf ../../virtualcall/output/indirectcall_"$1".txt \
    -tiff ../output/ToInsertFunc_"$1".txt \
    -ocof ../output/only_called_once_func_backup_"$1".txt \
     -c ../output/"$1"_replay_single.cfg  -v -v -v