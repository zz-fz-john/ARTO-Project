# boost
if (EXISTS "${BOOST_INSTALL_DIR}")
else()
    set(BOOST_INSTALL_DIR $ENV{BOOST_DIR}/boost_install CACHE PATH "Boost installation directory")
    # message(STATUS "boost_install_dir: $ENV{BOOST_DIR}/boost_install")
    if(EXISTS "${BOOST_INSTALL_DIR}")
    else()
        message(FATAL_ERROR "\
            WARNING: The BOOST_INSTALL_DIR var was not set (required for an out-of-source build)!\n
            Please set this to environment variable to point to the BOOST_INSTALL_DIR directory or set this variable to cmake configuration\n
            (e.g. on linux: export BOOST_INSTALL_DIR=/path/to/dir) \n or (make the project via: cmake -DBOOST_INSTALL_DIR=your_path_to_boost)")
    endif()
endif()

# set(BOOST_ROOT /home/lqs66/Desktop/boost_1_74_0/boost_install)
message(STATUS boost_install_dir: ${BOOST_INSTALL_DIR})
set(BOOST_ROOT ${BOOST_INSTALL_DIR})
set(Boost_NO_SYSTEM_PATHS ON)

FIND_PACKAGE(Boost COMPONENTS program_options REQUIRED)

if(Boost_FOUND)
    message("Boost version: ${Boost_VERSION}")
    message("Boost include directory: ${Boost_INCLUDE_DIRS}")
    message("Boost library directory: ${Boost_LIBRARY_DIRS}")
    include_directories (${Boost_INCLUDE_DIRS})
endif()