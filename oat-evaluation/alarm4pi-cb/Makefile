TEST ?= test_combo
#CC = aarch64-linux-gnu-gcc
CC = gcc
NUC_PC ?= your-pc-ip 
NUC_TARGET ?= ~/work/ra-project/evaluation/alarm4pi/
objdump ?= aarch64-linux-gnu-objdump 
hikey ?= your-hikey-board-ip 
# TEEC_EXPORT ?= /home/linaro/work/tee/export
TA ?= /home/linaro/work/tee/ta/
hikey_target ?= ~/work/alarm4pi/

NOVA_PATH = ~/work/llvm-env/build/lib/
# LLC_ARM = ~/work/llvm-env/buildc/bin/llc
LLC_ARM = /home/osboxes/Desktop/llvm/build/bin/llc

TEEC_EXPORT ?=/home/osboxes/Desktop/raspbian-tee/optee_client/out/export

CFLAGS += -Wall -I$(TA)/include -I$(TEEC_EXPORT)/include -I./include
#Add/link other required libraries here
# LDADD += -lteec -L$(TEEC_EXPORT)/lib -lresolv -pthread
LDADD += -lteec -L$(TEEC_EXPORT)/lib -I$(TEEC_EXPORT)/include -mfloat-abi=hard -O0 -g -gdwarf-5 -fPIC -c -lresolv -pthread

LDADD_ORIG += -lresolv -pthread
OPT = ~/work/llvm-env/build/bin/opt
VM = your-server-for-llvm:~/work/ra-project/evaluation/alarm4pi-cb/

TARGET = --target=arm-linux-gnueabihf

RPI3_GCC_CONFIG_ORG = -I/media/rpi/usr/include/arm-linux-gnueabihf -mfloat-abi=hard -O0 -g -gdwarf-2
RPI3_GCC_CONFIG = -I/usr/arm-linux-gnueabihf/include -mfloat-abi=hard -g0 -gdwarf-2
RPI3_LLC_CONFIG = -float-abi=hard -filetype=obj 
CFLAGS=-O0 -flto  -g -Xclang -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline -fwhole-program-vtables
CXXFLAGS=-O0 -flto -g -Xclang  -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fwhole-program-vtables -fno-exceptions  -fno-jump-tables -fno-inline 
ta_trampoline:
	$(GCC) $(LDADD)  ta_trampoline.c -o ta_trampoline.o -lc

util:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm util.c -o build/util.bc

bcm-gpio:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm bcm_gpio.c -o  build/bcm_gpio.bc

alarm4pi:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm alarm4pi.c -o  build/alarm4pi.bc

log-msgs:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm log_msgs.c -o  build/log_msgs.bc

proc-helper:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm proc_helper.c -o  build/proc_helper.bc
public-ip:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm public_ip.c -o  build/public_ip.bc

pushover:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm pushover.c -o  build/pushover.bc

gpio-polling:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm gpio_polling.c -o  build/gpio_polling.bc

gpio-polling-orig:
	clang $(TARGET) $(RPI3_GCC_CONFIG) -S -emit-llvm gpio_polling_orig.c -o  build/gpio_polling_orig.bc

test-orig: util alarm4pi gpio-polling-orig pushover public-ip proc-helper log-msgs bcm-gpio
	$(CC) $(LDADD_ORIG) -g util.o alarm4pi.o gpio_polling_orig.o pushover.o public_ip.o proc_helper.o log_msgs.o bcm_gpio.o  -o  build/test_orig
	
test-combo: util alarm4pi gpio-polling pushover public-ip proc-helper log-msgs bcm-gpio 
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/llvm-link  -opaque-pointers=0  build/util.bc  build/alarm4pi.bc  build/gpio_polling.bc  build/pushover.bc  build/public_ip.bc  build/proc_helper.bc  build/log_msgs.bc  build/bcm_gpio.bc -o  build/test_combo.bc 

llc-comp:
	$(LLC_ARM)  $(RPI3_LLC_CONFIG) after_compartment_test_combo.bc -o after_comp_combo.s

opt-combo:
	$(OPT) -load $(NOVA_PATH)/LLVMNova.so -nova < test_combo.bc > combo.bc 2>err.log
	$(OPT) -load $(NOVA_PATH)/LLVMCollectCFVHints.so -collect-ibranch-hints-pass -collect-icall-hints-pass -collect-cond-branch-hints-pass < combo.bc > combo_hints.bc 2>>err.log
	$(LLC_ARM) -march=aarch64  -aarch64-enable-cfv combo_hints.bc -o combo.s

send-combo-s:
	scp combo.s machine-name:~/tmp/hikey-relay/

send-combo-bc:
	scp -i vm test_combo.bc $(VM)

fetch-combo-s:
	scp $(VM)/combo.s . 

bin: test-combo
	#clang combo.s -L../../data/runtime -lm -lrt  -lsoftboundcets_rt  -lnova -lteec -lpthread  -lc -lresolv
	clang combo.s -L../../data/runtime -lm -lrt  -lnova -lteec -lpthread  -lc -lresolv

dis-combo:
	llvm-dis < test_combo.bc >test_combo.dis

send-combo:
	scp -i pi $(TEST) $(NUC_PC):~/work/ra-project/evaluation/hook-a64/

send-hikey:
	scp $(TEST) $(hikey):$(hikey_target)

sync-hikey:
	scp -r *.c *.h hooks_a64.S *.ld Makefile lib/ $(hikey):$(hikey_target)

sync-nuc:
	scp -i pi -r *.c *.h hooks_a64.S *.ld Makefile lib/ $(NUC_PC):$(NUC_TARGET)

clean:
	rm -f  *.o
	rm -f  test
	rm -rf build/
	rm -f *.bc
dump:
	$(objdump) $(TEST) -D > $(TEST).dump

ph:
	readelf -S  $(TEST)

sh:
	readelf -l  $(TEST)

# Compile Note: cdefs.h not found can be solved by the following cmd
#time CPATH=/usr/include/aarch64-linux-gnu/ make test-combo
