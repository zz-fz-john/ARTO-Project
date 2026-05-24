#!/bin/bash


cd ../pre-analysis16.0/util/verifier/ || { echo "Failed to enter verifier directory"; exit 1; }
python3 ../generate_final_hash/gen_config.py ../output/arducopter_output.S \
    --binary-file ../output/arducopter_output \
    --program-header arducopter_output.ph \
    -o ../output/arducopter_replay_single.cfg

python3 ./verifier.py \
    -bf ../output/arducopter_rewrite \
    -hi ../../../ardupilot/hash_value.txt \
    -bi ../../../ardupilot/branch_trace.txt \
    -dis ../output/arducopter_output.S  ../output/arducopter_rewrite\
    -db ../output/hash_database_single_on_server.txt \
    -IR ../output/after_insert_dummy_copter.ll \
    -svf ../../virtualcall/output/indirectcall_copter.txt \
    -tiff ../output/ToInsertFunc_copter.txt \
    -ocof ../output/only_called_once_func_backup_copter.txt \
    --cache ./segment_cache.json \
     -c ../output/arducopter_replay_single.cfg  -v -v -v