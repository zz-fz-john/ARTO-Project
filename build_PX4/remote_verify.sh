#!/bin/bash


cd ../pre-analysis16.0/util/verifier/ || { echo "Failed to enter verifier directory"; exit 1; }
python3 ../generate_final_hash/gen_config.py ../output/px4_output.S \
    --binary-file ../output/px4_output \
    --program-header px4_output.ph \
    -o ../output/px4_replay_single.cfg

python3 ./verifier.py \
    -bf ../output/px4_rewrite \
    -hi ../../../px4-1.15/hash_value.txt \
    -bi ../../../px4-1.15/branch_trace.txt \
    -dis ../output/px4_output.S  ../output/px4_rewrite\
    -db ../output/hash_database_single_on_server_px4.txt \
    -IR ../output/after_insert_dummy_px4.ll \
    -svf ../../virtualcall/output/indirectcall_px4.txt \
    -tiff ../output/ToInsertFunc_px4.txt \
    -ocof ../output/only_called_once_func_backup_px4.txt \
     -c ../output/px4_replay_single.cfg  -v -v -v