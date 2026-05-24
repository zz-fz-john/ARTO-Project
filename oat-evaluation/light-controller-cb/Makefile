# I guess that requiring people to instal Scons is silly, so I'll add this
# Makefile.

OPT = ~/work/llvm-env/build/bin/opt
VM = your-server-for-llvm:~/work/ra-project/evaluation/light-controller-cb/

NOVA_PATH = ~/work/llvm-env/build/lib/
LLC_ARM = ~/work/llvm-env/build/bin/llc

FILE=insPi.sh

UNAME := $(shell uname -m)
WARNING_FLAGS := -Wall -Wextra -Wshadow -Wswitch-enum \
    -Wswitch-default
C_WARNING_FLAGS := -Wmissing-prototypes -Wmissing-declarations \
    -Wstrict-prototypes

ifeq ($(UNAME), armv6l)
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS} -DRASPI=1
else ifeq ($(UNAME), armv7l)
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS} -DRASPI=2
else
	CFLAGS := ${WARNING_FLAGS} ${C_WARNING_FLAGS}
endif
CFLAGS=-O0 -flto  -g -Xclang -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline
CXXFLAGS=-O0 -flto -g -Xclang  -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fwhole-program-vtables -fno-exceptions  -fno-jump-tables -fno-inline 
LDLIBS := -ltelldus-core -rdynamic
util:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm util.c -o build/util.bc
light-controller: 
	clang  $(CFLAGS) $(LDLIBS) light-controller.c -o build/server

lc-bc:
	#clang -D_GNU_SOURCE --target=aarch64 -S -emit-llvm light-controller.c -o light-controller.bc
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm  light-controller.c -o build/light-controller.bc

test-combo: lc-bc util
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/llvm-link -opaque-pointers=0  build/util.bc build/light-controller.bc -o build/test_combo.bc

opt-combo:
	$(OPT) -load $(NOVA_PATH)/LLVMNova.so -nova < test_combo.bc > combo.bc 2>err.log
	$(OPT) -load $(NOVA_PATH)/LLVMCollectCFVHints.so -collect-ibranch-hints-pass -collect-icall-hints-pass -collect-cond-branch-hints-pass < combo.bc > combo_hints.bc 2>>err.log
	$(LLC_ARM) -march=aarch64  -aarch64-enable-cfv combo_hints.bc -o combo.s

send-combo-bc:
	scp -i vm test_combo.bc $(VM)

fetch-combo-s:
	scp $(VM)/combo.s . 

fetch-file:
	scp $(VM)/$(FILE) . 

bin: test-combo
	clang combo.s -L../../data/runtime -lm $(LDLIBS) -lrt  -lsoftboundcets_rt  -lnova -lteec -lc 

run-client: 
	./interactive_control.py example.json 
	
run-server: light-controller
	./server

sync:
	cp light-controller.c Makefile interactive_control.py ~/tee_workspace/ra-project/evaluation/rc-cb/ 

.PHONY:	clean
clean:
	rm -f *.o
	rm -f *.pyc
	rm -f light-controller
	rm -f build/
