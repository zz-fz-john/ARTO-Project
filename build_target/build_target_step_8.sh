#!/bin/bash
scp ./build/"$1"/"$1"_rewrite  ./build/"$1"/"$1"_test_combo ./build/"$1"/"$1" pi@192.168.1.101:/home/pi

