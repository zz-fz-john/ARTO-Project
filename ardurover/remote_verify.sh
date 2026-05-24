#!/bin/bash
#run on rpi3
#scp branch_trace.txt hash_value.txt  <your name>@192.168.1.100:~/ARTO/ardurover

cd ../pre-analysis16.0/util/verifier/ || { echo "Failed to enter verifier directory"; exit 1; }
python3 ../generate_final_hash/gen_config.py ../output/ardurover_output.S \
    --binary-file ../output/ardurover_output \
    --program-header ardurover_output.ph \
    -o ../output/ardurover_replay_single.cfg

python3 ./verifier.py \
    -bf ../output/ardurover_rewrite \
    -hi ../../../ardurover/hash_value.txt \
    -bi ../../../ardurover/branch_trace.txt \
    -dis ../output/ardurover_output.S  ../output/ardurover_rewrite\
    -db ../output/hash_database_single_on_server_rover.txt \
     -c ../output/ardurover_replay_single.cfg  -v -v -v