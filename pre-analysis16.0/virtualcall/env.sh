#!/bin/bash

PROJECTHOME=$(pwd)
SVFHOME=${PROJECTHOME}/"../type_match_svf3/SVF"
LLVM_DIR=${PROJECTHOME}"/../../llvm-16.0/llvm-project-16.0.0/"
#CTIRHome="ctir.obj"
#export CTIR_DIR="$SVFHOME/$CTIRHome/bin"
export LLVM_DIR=$LLVM_DIR
export LLVM_SRC=$LLVM_DIR
export LLVM_OBJ=$LLVM_DIR
export PATH=$LLVM_DIR/bin:$PATH
export SVF_DIR=$SVFHOME

#echo "export LLVM_DIR=$LLVM_DIR" >> ~/.bashrc
#echo "export Z3_DIR=$Z3_DIR" >> ~/.bashrc
#echo "export SVF_DIR=$SVF_DIR" >> ~/.bashrc
#echo "export PATH=$SVF_DIR/Release-build/bin:$LLVM_DIR/bin:$PROJECTHOME/bin:$PATH" >> ~/.bashrc

# Add compiled SVF binaries dir to $PATH
#export PATH=$SVF_DIR/Release-build/bin:$PATH

echo "LLVM_DIR="$LLVM_DIR
echo "SVF_DIR="$SVF_DIR

