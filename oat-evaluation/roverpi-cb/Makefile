# I guess that requiring people to instal Scons is silly, so I'll add this
# Makefile.

OPT = ~/work/llvm-env/build/bin/opt
VM = /your/server/ip:~/work/ra-project/evaluation/roverpi-cb/

NOVA_PATH = ~/work/llvm-env/build/lib/
LLC_ARM = ~/work/llvm-env/build/bin/llc

FILE=insPi.sh
RPI3_GCC_CONFIG = -I/media/rpi/usr/include/arm-linux-gnueabihf -mfloat-abi=hard -O0 -g -gdwarf-5 -fverbose-asm
RPI3_GCC_CONFIG += -Wall
TARGET = --target=arm-linux-gnueabihf
CFLAGS=-O0 -flto  -g -Xclang -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline 
CXXFLAGS=-O0 -flto -g -Xclang  -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fwhole-program-vtables -fno-exceptions  -fno-jump-tables -fno-inline 
util:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm util.c -o build/util.bc
tcp: 
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm tcp.c -o build/tcp.bc

rovertcp:
	#clang -D_GNU_SOURCE --target=aarch64 -S -emit-llvm light-controller.c -o light-controller.bc
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm rovertcp.c -o build/rovertcp.bc

test-combo: tcp rovertcp util 
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/llvm-link -opaque-pointers=0  build/tcp.bc build/rovertcp.bc build/util.bc -o build/test_combo.bc

opt-combo:
	$(OPT) -load $(NOVA_PATH)/LLVMNova.so -nova < test_combo.bc > combo.bc 2>err.log
	$(OPT) -load $(NOVA_PATH)/LLVMCollectCFVHints.so -collect-ibranch-hints-pass -collect-icall-hints-pass -collect-cond-branch-hints-pass < combo.bc > combo_hints.bc 2>>err.log
	$(LLC_ARM) -march=aarch64  -aarch64-enable-cfv combo_hints.bc -o combo.s

client: client.c
	clang client.c -o client

send-combo-bc:
	scp -i vm test_combo.bc $(VM)

fetch-combo-s:
	scp $(VM)/combo.s . 

fetch-file:
	scp $(VM)/$(FILE) . 

bin: test-combo
	clang combo.s -L../../data/runtime -lm $(LDLIBS) -lrt  -lsoftboundcets_rt  -lnova -lteec -lc -lwiringPi 

.PHONY:	clean
clean:
	rm -rf  ./build
