
# find_package(LLVM CONFIG NO_DEFAULT_PATH)

if (DEFINED LLVM_DIR)
    #ENV{LLVM_DIR} = ${LLVM_DIR}
    set(ENV{LLVM_DIR} "${LLVM_DIR}")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (DEFINED ENV{LLVM_DIR})

    #find LLVMConfig.cmake 
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LT_LLVM_INSTALL_DIR}")

    message("LLVM STATUS:
      llvm        ${LLVM_DIR}
      Definitions ${LLVM_DEFINITIONS}
      Includes    ${LLVM_INCLUDE_DIRS}
      Libraries   ${LLVM_LIBRARY_DIRS}
      Targets     ${LLVM_TARGETS_TO_BUILD}"
    )

    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    include(AddLLVM)

    add_definitions(${LLVM_DEFINITIONS})
    include_directories(${LLVM_INCLUDE_DIRS})
    link_directories(${LLVM_LIBRARY_DIRS})

else()
    message(FATAL_ERROR "\
WARNING: The LLVM_DIR var was not set (required for an out-of-source build)!\n\
Please set this to environment variable to point to the LLVM build directory\
(e.g. on linux: export LLVM_DIR=/path/to/llvm/build/dir)")
endif()

# set(LLVM_USE_SHARED_LIBS ON)

add_library (llvm16 INTERFACE)

llvm_map_components_to_libnames(llvm_libs bitwriter core ipo irreader instcombine instrumentation target linker analysis scalaropts support passes)
target_include_directories(llvm16 INTERFACE ${LLVM_INCLUDE_DIRS})
target_link_libraries(llvm16 INTERFACE ${llvm_libs})   

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
