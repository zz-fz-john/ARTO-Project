# ARTO: Efficient Execution Integrity Attestation for Real-Time Operation of Cyber-Physical Systems

ARTO is an execution integrity attestation framework designed for real-time cyber-physical systems, implementing compartmentalization and runtime measurement techniques for ArduPilot-based autopilot systems.

## Overview

ARTO provides:
- **Compartmentalization**: Software-based fault isolation using modified LLVM compiler
- **Runtime Measurement**: Control-flow and data-flow integrity verification 
- **Mission Verification**: Post-execution integrity attestation


## Project Structure

```
ARTO/
├── llvm-16.0/                           
│   └── llvm-project-16.0.0/                            # Modified LLVM 16.0 compiler toolchain
│       ├── llvm/lib/Transforms/Hexbox/                 # Compartmentalization passes
│       │   ├── HexboxAnalysis.cpp                      # Static analysis for compartment generation
│       │   └── HexboxApplication.cpp                   # LLVM IR instrumentation pass
|       ├──llvm/lib/Transfors/InsertCheckpoint
|       |   ├──InsertCheckpoint.cpp                     # llvm fronted pass to insert checkpoint
|       |   └──insert_dymmy_code.cpp                    # llvm fronted pass to insert dummy code
│       └── llvm/lib/Target/ARM/                        # ARM backend instrumentation
│           ├── SFI.cpp                                 # Software Fault Isolation (sandboxing)
│           └── collect_indirectJump.cpp                # Control-flow tracking
│
├── pre-analysis16.0/                                   # Pre-compilation analysis tools
│   ├── util/verifier/verifier.py                       # remote verifer
│   ├── util/generate_final_hash/simulated_executor.py   # simulted_executor                   
|   └── virtualcall/src/main.cpp                        # A program analysis framework combined SVF,SMLTA and llvm virtual call devrit to resolve indirect call
│       
├── measurement-engine/                  # Runtime measurement components
│   └── first-measure-in-secure-world/
│       └── runtime-measure/             # Hash-based runtime measurement 
│
├── oat-evaluation/                      # small embed program
│
├── ardupilot/                           # ArduPilot with ARTO instrumentation
│   ├── compile_*_step.txt               # Multi-stage build scripts
│   └── ArduCopter/                      # Instrumented copter firmware
│
├── ardurover/                           # ArduRover with ARTO support
├── px4-1.15/                            # PX4 autopilot integration
├── build_PX4/                           # PX4 specific build scripts
├── embench_test/                        # Embench benchmark suite for performance testing
├── build_target/                        # Build scripts for custom target programs
│
├── WCET                                 # use Extreme Value Theory (EVT) to compute wcet
├── schedulibility                       # schedulibility use MAST
├── WPP                                  # WPP algorithm
├── build_llvm.sh                        # LLVM toolchain build script
├── build_svf.sh                         # SVF static analysis framework
└── toolchain.mk                         # Toolchain configuration
```

## Prerequisites

### Hardware Requirements
- **Development Machine**: Ubuntu 20.04 LTS (x86_64)
- **Target Platform**: Raspberry Pi 3 
- Network connectivity between development machine and Raspberry Pi

### Software Dependencies

Install the following on Ubuntu 20.04:

```bash
# Core development tools
sudo apt-get update
sudo apt-get install -y \
    git cmake build-essential make \
    texinfo bison flex ninja-build \
    ncurses-dev texlive-full binutils-dev \
    libgmp-dev libmpfr-dev curl libssl-dev libtinfo5 python-tk

# Python 2 and dependencies (required for ArduPilot build)

sudo apt-get install python-pip
# if previous command error run 
curl -O https://bootstrap.pypa.io/pip/2.7/get-pip.py
sudo python2 get-pip.py
python -m pip install --upgrade "pip < 19.2"
sudo python -m pip install --upgrade "pip < 21.0"
sudo apt install -y python2 python2-dev python-is-python2
sudo apt install -y graphviz libgraphviz-dev pkg-config python2-dev
# Python packages for analysis and simulation
pip2 install future lxml==5.0.0 pymavlink==2.4.43  MAVProxy==1.8.48 empy pexpect

python2 -m pip install --user \
  networkx==1.11 \
  matplotlib==2.2.5 \
  pygraphviz==1.5 \
  pyserial==3.5 \
  bitarray==2.7.2  

pip2 install pydotplus python-louvain bitarray==2.7.2 capstone==4.0.2 enum34 pyelftools==0.26 pyblake2


# python3 and dependencies
sudo apt install -y \
  python3-pygraphviz \
  python3-networkx \
  python3-matplotlib \
  python3-serial
sudo apt install python3-pip
pip3 install  future lxml pymavlink MAVProxy \
    pydotplus python-louvain bitarray \
    capstone enum34 pyelftools pyblake2 pygraphviz keystone-engine lief==0.10.1 kconfiglib symforce pyros-genmsg networkx==1.11 cxxfilt
    
/usr/bin/python3 -m pip install --user empy==3.3.4
# ARM cross-compilation toolchain
sudo apt-get install -y \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf

# build svf tool,you could run the command in build_svf.sh step by step
./build_svf.sh
```

## Building ARTO

### Step 1: Clone the Repository

```bash
git clone <repository-url> ARTO
cd ARTO
export ARTO_ROOT=$(pwd)
```

### Step 2: Build Gold Linker (Required for LLVM)

```bash
cd ~
mkdir -p gold && cd gold

# Clone binutils with Gold linker support
git clone --depth 1 https://sourceware.org/git/binutils-gdb.git binutils

# Build Gold linker (use GCC 7.5 if possible)
cd binutils
mkdir build && cd build
../configure --enable-gold --enable-plugins --disable-werror
make all -j$(nproc)

export BINUTILS_INCLUDE=~/gold/binutils/include
```
### step 3： Get unmodified LLVM 16.0 TOOlchain
If you don't want the original flight controller, you can skip this step.
```bash

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

```
### Step 4: Build Modified LLVM 16.0 Toolchain

```bash
cd $ARTO_ROOT
bash build_llvm.sh
```

Or manually:

```bash
cd $ARTO_ROOT/llvm-16.0/llvm-project-16.0.0
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

# Add LLVM to PATH
echo "export PATH=\$PATH:$ARTO_ROOT/llvm-16.0/llvm-project-16.0.0/build/bin" >> ~/.bashrc
source ~/.bashrc
```

### Step 5: Setup ARM Cross-Compiler

Use the provided Makefile to download ARM toolchains:

```bash
cd $ARTO_ROOT
make toolchains
# This will download:
# - aarch32: gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabihf
# - aarch64: gcc-arm-8.2-2018.08-x86_64-aarch64-linux-gnu

# Add aarch32 toolchain to PATH (for ArduPilot builds)
echo "export PATH=\$PATH:$ARTO_ROOT/toolchains/aarch32/bin" >> ~/.bashrc
source ~/.bashrc

# Verify installation
arm-linux-gnueabihf-gcc --version
```

### Step 6: Build Runtime Measurement Components

Build the measurement engine components:

```bash
cd $ARTO_ROOT/measurement-engine/first-measure-in-secure-world/runtime-measure
make first_measure

# This builds:
# - trampoline.o: Control-flow tracking trampoline
# - CFeventSingleThread.o: Control-flow event handling
# - data_flow.o: Data-flow integrity checking
# - blake2s.o / xxhash3.o: Hash computation
# - dummycode.o: Dummy code for instrumentation
# - heap_section.o: Heap memory tracking
```

### Step 7: Build ArduCopter with ARTO Instrumentation

Navigate to the `ardupilot` directory and follow the sequential build steps.

```bash
cd $ARTO_ROOT/ardupilot

# Step 0: Build ArduCopter and generate LLVM IR
bash ./compile_0_step.txt
# - Configures build environment (clang, flags)
# - Compiles ArduCopter with waf
# - Generates LLVM bitcode and performs IR optimization
# - Outputs: arducopter.ll, arducopter_breakconstant.ll, arducopter_org

# Step 1: Indirect call analysis (SVF-based)
bash ./compile_1_step.txt
# - Analyzes virtual/indirect calls
# - Generates call-site to target mapping
# - Outputs: indirectcall.txt, direct_call_result.txt

# Step 2: Static analysis for critical function identification
bash ./compile_2_step.txt
# - Finds functions in critical paths
# - Identifies recursive and leaf functions
# - Outputs: ToInsertFunc.txt, recursive_function.txt, only_called_once_func.txt

# Step 3: Compartmentalization analysis and application
bash ./compile_3_step.txt
# - Performs HexboxAnalysis for compartment boundaries
# - Runs analyzer.py to generate compartmentalization policy
# - Applies HexboxApplication to instrument IR
# - Outputs: analysis_result.json, compartments_result.json, after_compartment_llvm_link.ll

# Step 4: Insert checkpoints and enable data-flow integrity
bash ./compile_4_step.txt
# - Inserts checkpoints for control-flow tracking
# - Enables data-flow integrity checking (NOVA pass)
# - Inserts dummy code for measurement
# - Generates dummycode.S
# - Outputs: after_insert_dummy.ll

# Step 5: Compile measurement engine and link final binary
bash ./compile_5_step.txt
# - Compiles measurement-engine components
# - Uses llc to generate ARM object file
# - Links with measurement libraries and custom linker script
# - Outputs: arducopter (initial binary), arducopter_output (rewritten)

# Step 6: Generate offline evidence database
bash ./compile_6_step.txt
# - Generates configuration for replay
# - Creates offline hash database for verification
# - Outputs: hash_database_single_on_server.txt, replay_single.cfg

# Step 7: Binary rewriting for integrity database
bash ./compile_7_step.txt
# - Embeds hash database into binary .custom_ro_data section
# - Outputs: arducopter_rewrite (final instrumented binary)

# Step 8: Deploy to Raspberry Pi
bash ./compile_8_step.txt
# - Transfers binaries and configuration files to Pi via scp
# - Copies measurement components
# - Deploys: arducopter_rewrite, arducopter_output, copter.parm, etc.
```


## Running ARTO on Raspberry Pi 3

### Step 1: Start ArduCopter on Raspberry Pi

SSH into your Raspberry Pi 3:

```bash
ssh pi@<RPI_IP>
cd ~
./arducopter_rewrite -S --model + --speedup 1 --defaults ./copter.parm -I0
```

### Step 2: Start Mission Simulation on Host

On your development machine:

```bash
cd ~/MAVProxy/MAVProxy
python3 mavproxy.py \
    --master tcp:<RPI_IP>:5760 \
    --out <HOST_IP>:14550 \
    --console \
    --map
```

Wait approximately 1 minute for initialization. The console should display:
```
AP: EKF2 IMU0 is using GPS
AP: EKF2 IMU1 is using GPS
```

### Step 3: Execute Mission Commands

In the MAVProxy console:

```
STABILIZE> mode guided
GUIDED> arm throttle
GUIDED> takeoff 20
```

After mission completion, stop both ArduCopter (on Pi) and MAVProxy (on host).

## Mission Verification

### Step 1: Transfer Runtime Measurements

Retrieve the execution traces from the Raspberry Pi:

```bash
cd $ARTO_ROOT/ardupilot
scp pi@<RPI_IP>:/home/pi/branch_trace.txt .
scp pi@<RPI_IP>:/home/pi/hash_value.txt .
```

### Step 2: Verify Mission Integrity

```bash
# Execute the verification script
bash ./remote_verify.sh
```

The verification engine will analyze the runtime measurements and report any integrity violations.






