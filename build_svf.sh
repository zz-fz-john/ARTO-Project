#build boost
cd ~
mkdir boost
cd boost
wget -O boost_1_74_0.tar.gz https://sourceforge.net/projects/boost/files/boost/1.74.0/boost_1_74_0.tar.gz/download
tar -xvzf boost_1_74_0.tar.gz
sudo apt-get update
sudo apt-get install build-essential g++ python-dev autotools-dev libicu-dev build-essential libbz2-dev libboost-all-dev
cd boost_1_74_0
./bootstrap.sh --prefix=/usr/
./b2
sudo ./b2 install
#build svf
cd ~
mkdir cmake
cd cmake
wget -O cmake-3.23.0.tar.gz https://cmake.org/files/v3.23/cmake-3.23.0.tar.gz
tar -xvzf cmake-3.23.0.tar.gz
cd cmake-3.23.0
./bootstrap
make -j8
sudo make install

cd ~/ARTO/pre-analysis16.0/type_match_svf3/SVF
./build.sh
#build virtualcall
cd ~/ARTO/pre-analysis16.0/virtualcall
source ./env.sh
mkdir build
cd build
cmake -DLLVM_DIR=../../../llvm-16.0/llvm-project-16.0.0/build/lib/cmake/llvm  -DBOOST_INSTALL_DIR=~/boost/boost_1_74_0/stage/lib/cmake/Boost-1.74.0 ..
# When performing cmake import, some files might not be found due to a bug in SVF's cmake
# Solution: Modify SVF/Release-build/lib/cmake/SVF/SVFConfig.cmake
# Change line 6 to:
# get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)
# Change lines 42 and 43 to:
# set_and_check(SVF_INSTALL_EXTAPI_DIR "${PACKAGE_PREFIX_DIR}/lib")
# set_and_check(SVF_INSTALL_EXTAPI_FILE "${PACKAGE_PREFIX_DIR}/lib/extapi.bc")
# Change line 29 to
# set_and_check(SVF_INSTALL_LIB_DIR "${PACKAGE_PREFIX_DIR}/lib")
make -j4