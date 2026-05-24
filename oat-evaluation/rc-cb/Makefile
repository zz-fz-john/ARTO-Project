# I guess that requiring people to instal Scons is silly, so I'll add this
# Makefile.

OPT = ~/work/llvm-env/build/bin/opt
VM = your-server-for-llvm:~/work/ra-project/evaluation/rc-cb/

NOVA_PATH = ~/work/llvm-env/build/lib/
LLC_ARM = ~/work/llvm-env/build/bin/llc

UNAME := $(shell uname -m)
WARNING_FLAGS := -Wall -Wextra -Wshadow -Wswitch-enum \
    -Wswitch-default
C_WARNING_FLAGS := -Wmissing-prototypes -Wmissing-declarations \
    -Wstrict-prototypes
RPI3_GCC_CONFIG = -I/media/rpi/usr/include/arm-linux-gnueabihf -mfloat-abi=hard -O0 -g -gdwarf-5 -fverbose-asm
RPI3_GCC_CONFIG += -Wall
ifeq ($(UNAME), armv6l)
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS} -DRASPI=1
else ifeq ($(UNAME), armv7l)
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS} -DRASPI=2
else
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS}
endif
CFLAGS=-O0 -flto  -g -Xclang -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline
CXXFLAGS=-O0 -flto -g -Xclang  -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fwhole-program-vtables -fno-exceptions  -fno-jump-tables -fno-inline 
LDLIBS := -lm -ljansson
SYSROOT=~/ARTO/toolchains/aarch32/arm-linux-gnueabihf/libc
#pi_pcm: pi_pcm.o mailbox.o
dtoa_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang  $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/dtoa.c  -o build/dtoa.bc
dump_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/dump.c  -o build/dump.bc
error_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/error.c  -o build/error.bc
hashtable_seed_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/hashtable_seed.c  -o build/hashtable_seed.bc
hashtable_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/hashtable.c  -o build/hashtable.bc
load_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/load.c  -o build/load.bc
memory_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/memory.c  -o build/memory.bc
pack_unpack_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/pack_unpack.c  -o build/pack_unpack.bc
strbuffer_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/strbuffer.c  -o build/strbuffer.bc
strconv_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/strconv.c  -o build/strconv.bc
utf_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT) $(CFLAGS) -S -emit-llvm  ./jansson/utf.c  -o build/utf.bc
value_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT)  $(CFLAGS) -S -emit-llvm  ./jansson/value.c  -o build/value.bc
version_bc:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) --sysroot=$(SYSROOT)  $(CFLAGS) -S -emit-llvm  ./jansson/version.c  -o build/version.bc
jansson_bc: dtoa_bc dump_bc error_bc hashtable_seed_bc hashtable_bc load_bc memory_bc pack_unpack_bc strbuffer_bc strconv_bc utf_bc value_bc version_bc
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/llvm-link -opaque-pointers=0  build/dtoa.bc build/dump.bc build/error.bc build/hashtable_seed.bc build/hashtable.bc build/load.bc build/memory.bc build/pack_unpack.bc build/strbuffer.bc build/strconv.bc build/utf.bc build/value.bc build/version.bc -o build/jansson.bc
pi_pcm:
	clang $(CFLAGS) $(LDLIBS) pi_pcm.c -o build/server

pi-pcm-bc:
	#clang -D_GNU_SOURCE --target=aarch64 -S -emit-llvm pi_pcm.c -o pi_pcm.bc
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -I~/ARTO/oat-evaluation/rc-cb/jansson -S -emit-llvm  pi_pcm.c -o build/pi_pcm.bc

util:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm util.c -o build/util.bc

test-combo: pi-pcm-bc jansson_bc util 
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/llvm-link -opaque-pointers=0 build/pi_pcm.bc build/jansson.bc  build/util.bc -o build/test_combo.bc


opt-combo:
	$(OPT) -load $(NOVA_PATH)/LLVMNova.so -nova < test_combo.bc > combo.bc 2>err.log
	$(OPT) -load $(NOVA_PATH)/LLVMCollectCFVHints.so -collect-ibranch-hints-pass -collect-icall-hints-pass -collect-cond-branch-hints-pass < combo.bc > combo_hints.bc 2>>err.log
	$(LLC_ARM) -march=aarch64  -aarch64-enable-cfv combo_hints.bc -o combo.s

send-combo-bc:
	scp -i vm test_combo.bc $(VM)

fetch-combo-s:
	scp $(VM)/combo.s . 

bin: test-combo
	clang combo.s -L../../data/runtime -lm -ljansson -lrt  -lsoftboundcets_rt  -lnova -lteec -lc 

run-client: 
	./interactive_control.py example.json 
	
run-server: pi_pcm
	./server

sync:
	cp pi_pcm.c Makefile interactive_control.py ~/tee_workspace/ra-project/evaluation/rc-cb/ 

.PHONY:	clean
clean:
	rm -f *.o
	rm -f *.pyc
	rm -f pi_pcm
	rm -f *.bc
	rm -rf build

