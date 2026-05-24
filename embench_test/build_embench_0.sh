

./build_all.py \
     --arch=arm \
     --chip=generic \
     --board=generic \
     --cc="$HOME/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang" \
     --ld="$HOME/llvm-arm-cross/llvm-project-16.0.0/build/bin/clang" \
     --cflags="-O0 -flto  -g -Xclang -no-opaque-pointers -gdwarf-4 -flto -fno-discard-value-names -fembed-bitcode -fno-exceptions -fno-jump-tables -fno-inline -emit-llvm -c --target=arm-linux-gnueabihf -mcpu=cortex-a53 -I/usr/arm-linux-gnueabihf/include -mfloat-abi=hard -g0 -gdwarf-4"

