ccc=../../../toolchains/aarch32/bin/arm-linux-gnueabihf-gcc
CCCC=../../../toolchains/aarch32/bin/arm-linux-gnueabihf-g++
RESERVE_R8 = -ffixed-r8


first_measure:
	mkdir -p ../output
	$(ccc)  -I../ta/include  -I$(TEEC_EXPORT)/include -fPIC -g -c -gdwarf-5 $(RESERVE_R8) -O3 -Wall CFeventSingleThread.c  -o ../output/CFeventSingleThread.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall trampoline.S -o ../output/trampoline.o
	$(ccc) -I../ta/include  -I$(TEEC_EXPORT)/include -fPIC -g -c -gdwarf-5 -O3 -Wall $(RESERVE_R8) data_flow.c -o ../output/data_flow.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall trampoline_cplus.S -o ../output/trampoline_cplus.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall $(RESERVE_R8)  blake2s-ref.c -o ../output/blake2s.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall $(RESERVE_R8) xxhash64.c -o ../output/xxhash64.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a53 $(RESERVE_R8) xxhash3.c -o ../output/xxhash3.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O3 -Wall $(RESERVE_R8) xxhash.c -o ../output/xxhash.o
	$(ccc) -fPIC -g -c -gdwarf-5 -O0 -Wall heap_section.S -o ../output/heap_section.o

	../../../toolchains/aarch32/bin/arm-linux-gnueabihf-g++ -fPIC -g -c -gdwarf-5 -O0 -Wall dummycode.S -o ../output/dummycode.o


clean:
	rm ../output/* 
