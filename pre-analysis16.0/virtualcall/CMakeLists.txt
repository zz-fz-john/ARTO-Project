cmake_minimum_required(VERSION 3.21)
project(indirectCallAnalysis)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE
      STRING "Build type (default Release):" FORCE)
endif()

include(llvm.cmake)
include(boost.cmake)
include(svf.cmake)

include_directories("include")
include_directories("src")

file(GLOB SOURCES "src/*.cpp"  "src/*.cc")
message("LLVM STATUS: llvm        llvm16")
add_executable(indCallAnalysis ${SOURCES})
target_link_libraries(indCallAnalysis llvm16 ${Boost_LIBRARIES})
target_link_libraries(indCallAnalysis  ${SVF_LIB})
target_link_libraries(indCallAnalysis ${Z3_LIBRARIES})

