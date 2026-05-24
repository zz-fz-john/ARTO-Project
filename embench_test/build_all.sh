#!/bin/bash
./build_embench_0.sh "$1" ||exit 1
./build_embench_1.sh "$1" ||exit 1
./build_embench_2.sh "$1" ||exit 1
./build_embench_3.sh "$1" ||exit 1
./build_embench_4.sh "$1" ||exit 1
./build_embench_5.sh "$1" ||exit 1
./build_embench_6.sh "$1" ||exit 1