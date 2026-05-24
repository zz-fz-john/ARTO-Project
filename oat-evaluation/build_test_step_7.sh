#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/aarch32"
rm -f  "$1"/build/"$1"_rewrite
cd ../pre-analysis16.0/util/binary_rewrite/ || { echo "Failed to enter binary rewrite directory"; exit 1; }
cp ../output/"$1"_output ../output/"$1"_rewrite
python3 add_data_base.py  ../output/"$1"_rewrite ../output/final_hash_database_single_on_server_"$1".txt .custom_ro_data ||exit 1
rm -f ../output/"$1"_after.S 
$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -s -j .custom_ro_data  ../output/"$1"_rewrite > ../output/"$1"_after.S
cp ../output/"$1"_rewrite ${ROOT_DIR}/oat-evaluation/"$1"/build/

