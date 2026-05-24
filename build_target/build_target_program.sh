#!/bin/bash
./build_target_step_0.sh "$1" || exit 1
./build_target_step_1.sh "$1" || exit 1
./build_target_step_2.sh "$1" || exit 1
./build_target_step_3.sh "$1" || exit 1
./build_target_step_4.sh "$1" || exit 1
./build_target_step_5.sh "$1" || exit 1
# ./build_target_step_6.sh "$1" || exit 1
./build_target_step_7.sh "$1" || exit 1
./build_target_step_8.sh "$1" || exit 1


