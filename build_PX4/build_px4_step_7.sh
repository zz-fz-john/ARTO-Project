#!/bin/bash
CURRENT_DIR=$(pwd)
ROOT_DIR="${CURRENT_DIR}/.."
TOOLCHAIN_DIR="${ROOT_DIR}/toolchains/aarch32"
rm -f  build/SITL_arm_linux_gnueabihf/px4_rewrite
cd ../pre-analysis16.0/util/binary_rewrite/ || { echo "Failed to enter binary rewrite directory"; exit 1; }
cp ../output/px4_output ../output/px4_rewrite
python3 add_data_base.py  ../output/px4_rewrite ../output/final_hash_database_single_on_server_px4.txt .custom_ro_data
rm -f ../output/px4_after.S
$TOOLCHAIN_DIR/bin/arm-linux-gnueabihf-objdump -s -j .custom_ro_data  ../output/px4_rewrite > ../output/px4_after.S
cp ../output/px4_rewrite ${ROOT_DIR}/px4-1.15/build/emlid_navio2_default/bin/
cd ../../../px4-1.15/build/emlid_navio2_default/bin/
find ./ -type l -exec ln -sf  ./px4_rewrite {} \;
#find ./ -type l -exec ln -sf  ./px4_org {} \;
