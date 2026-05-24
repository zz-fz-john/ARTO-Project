#!/bin/bash
scp ./"$1"/build/"$1"_rewrite  ./"$1"/build/"$1"_test_combo ./"$1"/build/"$1" pi@192.168.1.101:/home/pi

