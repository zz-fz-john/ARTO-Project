# SVF config

if (EXISTS "${SVF_DIR}")
    MESSAGE(STATUS "SVF_DIR: "${SVF_DIR})
else()
    set(SVF_DIR $ENV{SVF_DIR})
    if(EXISTS "${SVF_DIR}")
    else()
        message(FATAL_ERROR "\
WARNING: The SVF_DIR var was not set (required for an out-of-source build)!\n\
Please set this to environment variable to point to the SVF_DIR directory or set this variable to cmake configuration\n
(e.g. on linux: export SVF_DIR=/path/to/SVF/dir) \n or \n \n(make the project via: cmake -DSVF_DIR=your_path_to_SVF) ")
    endif()
endif()

MESSAGE(STATUS "SVF_DIR: "${SVF_DIR})
MESSAGE(STATUS "SVF_BIN: "${SVF_BIN})

#set SVF env
if (CMAKE_BUILD_TYPE MATCHES "Release")
    set(SVF_BIN "${SVF_DIR}/Release-build")
    MESSAGE (STATUS "building SVF in release mode")
else()
    MESSAGE (STATUS "building SVF in debug mode")
    if (EXISTS "${SVF_DIR}/Debug-build")
        set(SVF_BIN "${SVF_DIR}/Debug-build")
    else()
        set(SVF_BIN "${SVF_DIR}/Release-build")
    endif()
endif()

MESSAGE(STATUS "SVF_DIR: "${SVF_DIR})
MESSAGE(STATUS "SVF_BIN: "${SVF_BIN})

find_package(SVF CONFIG HINTS ${SVF_DIR} ${SVF_BIN})
message(STATUS "SVF STATUS:
    Found:                              ${SVF_FOUND}
    Version:                            ${SVF_VERSION}
    Build mode:                         ${SVF_BUILD_TYPE}
    C++ standard:                       ${SVF_CXX_STANDARD}
    RTTI enabled:                       ${SVF_ENABLE_RTTI}
    Exceptions enabled:                 ${SVF_ENABLE_EXCEPTIONS}
    Install root directory:             ${SVF_INSTALL_ROOT}
    Install binary directory:           ${SVF_INSTALL_BIN_DIR}
    Install library directory:          ${SVF_INSTALL_LIB_DIR}
    Install include directory:          ${SVF_INSTALL_INCLUDE_DIR}
    Install 'extapi.bc' file path:      ${SVF_INSTALL_EXTAPI_FILE}")

# set(SVF_HEADER "${SVF_DIR}/include")

# If the SVF CMake package was found, show how to use some "modern" features of this approach; otherwise use old system
if("${SVF_FOUND}")
    message(STATUS "Found installed SVF instance; importing using modern CMake methods")

    # Check that SVF & the found LLVM instance match w.r.t. RTTI/exception handling support
    if(NOT (${SVF_ENABLE_RTTI} STREQUAL ${LLVM_ENABLE_RTTI}))
        message(FATAL_ERROR "SVF & LLVM RTTI support mismatch (SVF: ${SVF_ENABLE_RTTI}, LLVM: ${LLVM_ENABLE_RTTI})!")
    endif()
    if(NOT (${SVF_ENABLE_EXCEPTIONS} STREQUAL ${LLVM_ENABLE_EH}))
        message(FATAL_ERROR "SVF & LLVM exceptions support mismatch (SVF: ${SVF_ENABLE_EXCEPTIONS}, LLVM: ${LLVM_ENABLE_EH})!")
    endif()

    # Include SVF's include directories for all targets & include the library directories to find the library objects
    include_directories(SYSTEM ${SVF_INSTALL_INCLUDE_DIR})
    link_directories(${SVF_INSTALL_LIB_DIR})
    # Link the LLVM libraries (single dynamic library/multiple static libraries) to the example executable
else()
    message(STATUS "Failed to find installed SVF instance; using legacy import method")
    message(FATAL_ERROR "SVF & LLVM RTTI support mismatch (SVF: ${SVF_ENABLE_RTTI}, LLVM: ${LLVM_ENABLE_RTTI})!")
endif()

set(SVF_LIB SvfLLVM SvfCore)


#set z3 env
if (DEFINED Z3_DIR)
    set(ENV{Z3_DIR} "${Z3_DIR}")
endif()
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    if(EXISTS "${Z3_DIR}/src")
        find_package(Z3 REQUIRED CONFIG)
        include_directories(${Z3_CXX_INCLUDE_DIRS})
    else()
        find_library(Z3_LIBRARIES NAMES libz3.a libz3.so
                HINTS $ENV{Z3_DIR}
                PATH_SUFFIXES bin)
        find_path(Z3_INCLUDES NAMES z3++.h
                HINTS $ENV{Z3_DIR}
                PATH_SUFFIXES include z3)
        if(NOT Z3_LIBRARIES OR NOT Z3_INCLUDES)
            message(FATAL_ERROR "Z3 not found!")
        endif()
        include_directories(${Z3_INCLUDES})
        LINK_DIRECTORIES(${Z3_DIR}/bin)
    endif()
else()
    find_library(Z3_LIBRARIES NAMES libz3.a libz3.so
            HINTS $ENV{Z3_DIR}
            PATH_SUFFIXES bin)
    find_path(Z3_INCLUDES NAMES z3++.h
            HINTS $ENV{Z3_DIR}
            PATH_SUFFIXES include z3)
    if(NOT Z3_LIBRARIES OR NOT Z3_INCLUDES)
        message(FATAL_ERROR "Z3 not found!")
    endif()
    include_directories(${Z3_INCLUDES})
    LINK_DIRECTORIES(${Z3_DIR}/bin)
endif()

message(STATUS "Z3 STATUS:
  Z3 library file:        ${Z3_LIBRARIES}
  Z3 include directory:   ${Z3_INCLUDES}")
