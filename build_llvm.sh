#get dependicy
sudo apt-get install libgmp-dev libmpfr-dev texinfo bison flex
cd  ~
mkdir gold
cd gold
#download repository
git clone --depth 1 https://sourceware.org/git/binutils-gdb.git binutils
#compile suggest use gcc 7.5
cd binutils && mkdir build && cd build
../configure --enable-gold --enable-plugins --disable-werror
make all  

#Get unmodified LLVM 16.0 TOOlchain
cd ~
mkdir llvm-arm-cross
cd llvm-arm-cross
git clone https://github.com/llvm/llvm-project.git --branch llvmorg-16.0.0 llvm-project-16.0.0
mkdir -p build && cd build
cmake -G "Ninja" \
    -DLLVM_ENABLE_PROJECTS="lld;llvm;clang" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DEFAULT_TARGET_TRIPLE=arm-linux-gnueabihf \
    -DLLVM_TARGETS_TO_BUILD=ARM \
    -DLLVM_TARGET_ARCH=ARM \
    -DLLVM_BINUTILS_INCDIR=$BINUTILS_INCLUDE \
    -DLLVM_ENABLE_LIBXML2=0 \
    ../llvm
ninja -j$(nproc)

#Build Modified LLVM 16.0 Toolchain
cd ~/ARTO
cd llvm-16.0
cd llvm-project-16.0.0
mkdir build
cd build
cmake -G "Ninja" -DLLVM_ENABLE_PROJECTS="lld;llvm;clang" -DCMAKE_BUILD_TYPE=Release -DLLVM_DEFAULT_TARGET_TRIPLE=arm-linux-gnueabihf -DLLVM_TARGETS_TO_BUILD=ARM -DLLVM_BINUTILS_INCDIR=~/gold/binutils/include -DLLVM_TARGET_ARCH=ARM  -DLLVM_ENABLE_LIBXML2=0 ../llvm
ninja -j8

cd ~/ARTO
./build_svf.sh
make toolchains
