ccc=../../../toolchains/aarch32/bin/arm-linux-gnueabihf-gcc
CCCC=../../../toolchains/aarch32/bin/arm-linux-gnueabihf-g++
RESERVE_R8 = -ffixed-r8
all:
	$(ccc) $(CFLAGS) -fPIC -g -c -gdwarf-5 $(RESERVE_R8)  -Wall measure_time.c  -o ../output/measuer_time.o