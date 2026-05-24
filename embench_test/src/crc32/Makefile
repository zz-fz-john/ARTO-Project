RPI3_GCC_CONFIG_ORG = -I/media/rpi/usr/include/arm-linux-gnueabihf -mfloat-abi=hard -O0 -g -gdwarf-2
RPI3_GCC_CONFIG = -I/usr/arm-linux-gnueabihf/include -mfloat-abi=hard -g0 -gdwarf-2
RPI3_LLC_CONFIG = -float-abi=hard -filetype=obj 
CFLAGS=-O0 -flto  -g -Xclang -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline
CXXFLAGS=-O0 -flto -g -Xclang  -no-opaque-pointers --target=arm-linux-gnueabihf -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fwhole-program-vtables -fno-exceptions  -fno-jump-tables -fno-inline 

crc32:
	~/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang $(TARGET) $(RPI3_GCC_CONFIG) $(CFLAGS) -S -emit-llvm crc32.c -o  build/crc32.bc