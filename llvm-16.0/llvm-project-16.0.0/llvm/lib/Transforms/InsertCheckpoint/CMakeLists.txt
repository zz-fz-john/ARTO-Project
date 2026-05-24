# If we don't need RTTI or EH, there's no reason to export anything
# from the hello plugin.
if( NOT LLVM_REQUIRES_RTTI )
  if( NOT LLVM_REQUIRES_EH )
    set(LLVM_EXPORTED_SYMBOL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/insertCheckpoint.exports)
  endif()
endif()

if(WIN32 OR CYGWIN)
  set(LLVM_LINK_COMPONENTS Core Support)
endif()
SET(CMAKE_CXX_FLAGS "-Wall -fno-rtti")

add_llvm_library( LLVMInsertCheckPointsOnLoop  MODULE
    get_critical_func.cpp
    InsertCheckpoint.cpp
    insert_dummy_code.cpp
    AnalysisProcessFunction.cpp
    find_debug_info.cpp
    dynamic_collect.cpp
    get_func_name.cpp
    BreakconstantExpr.cpp
    DEPENDS
    intrinsics_gen
    PLUGIN_TOOL
    opt

)
