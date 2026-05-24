#!/bin/bash
cd ../px4-1.15 || { echo "Failed to enter PX4 directory"; exit 1; }
sshpass -p 'raspberry' rsync -avz build/emlid_navio2_default/bin  pi@192.168.1.101:~/px4/
sshpass -p 'raspberry' rsync -avz build/emlid_navio2_default/bin/px4-alias.sh  pi@192.168.1.101:~/px4/bin
sshpass -p 'raspberry' rsync -avz build/emlid_navio2_default/etc  pi@192.168.1.101:~/px4/
sshpass -p 'raspberry' rsync -avz ./posix-configs/rpi/px4_hil_complated.config pi@192.168.1.101:~/px4/
# cd ../measurement-engine/first-measure-in-secure-world/
# sshpass -p "raspberry" scp ./output/blake2s.o ./output/CFeventSingleThread.o  ./output/trampoline.o  ./output/dummycode.o ./output/heap_section.o pi@192.168.1.101:/home/pi/px4
# cd ../../pre-analysis16.0/util/output/
# sshpass -p "raspberry" scp ./px4_rewrite pi@192.168.1.101:/home/pi/px4
# sshpass -p "raspberry" scp ./px4_output pi@192.168.1.101:/home/pi/px4
##清理进程
#sudo pkill -9 -f px4
