# This code is released under the
# Apache License Version 2.0 http://www.apache.org/licenses/.
#
# Copyright (c) 2012 Louis Dionne
#
cmake_minimum_required(VERSION 3.1)
project(FastPFOR CXX C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)

if (NOT CMAKE_BUILD_TYPE)
    message ( STATUS "No build type selected, default to Release" )
    set ( CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE )
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")
include(AppendCompilerFlags)
include(DetectCPUFeatures)

if ( NOT CMAKE_SIZEOF_VOID_P EQUAL 8 )
   MESSAGE( FATAL_ERROR "Please use a 64-bit system. " )
endif()

set ( FastPFOR_SRCS
    src/bitpacking.cpp
    src/bitpackingaligned.cpp
    src/bitpackingunaligned.cpp
#    src/horizontalbitpacking.cpp
    src/simdunalignedbitpacking.cpp
    src/simdbitpacking.cpp
    src/varintdecode.c
    src/streamvbyte.c
)

# helpers vars to shorten generate lines
set ( GNUCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>" )
set ( GNUC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>" )
set ( GNUC_CXX "$<OR:${GNUCXX},${GNUC}>" )

add_library ( FastPFOR STATIC ${FastPFOR_SRCS} )
target_include_directories ( FastPFOR PUBLIC headers )
target_compile_options ( FastPFOR PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:-D_CRT_SECURE_NO_WARNINGS -wd4267 -wd4244> )
target_compile_options ( FastPFOR PRIVATE $<${GNUC_CXX}:-msse4.1> )

set_target_properties ( FastPFOR PROPERTIES POSITION_INDEPENDENT_CODE TRUE )

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    # require at least gcc 4.7
    if (CXX_COMPILER_VERSION VERSION_LESS 4.7)
        message(STATUS "GCC version must be at least 4.7!")
    endif()
    # Uncomment the following lines to see how the code compiles without AVX,SSE4.2 and/or SSE2
    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -march=x86-64")
    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -march=core2")
    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse4.2")

    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Wcast-align -Ofast -lm  -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse41")
    #set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -lm  -std=c++11 -DHAVE_CXX0X -msse41")
    #set (CMAKE_C_FLAGS_RELEASE "-Wall -Wcast-align -Ofast -lm  -DNDEBUG -std=c99  -msse41")
    #set (CMAKE_C_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -lm  -std=c99 -msse41")

elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "Intel")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0.1)
        message( FATAL_ERROR "Intel version must be at least 14.0.1!")
    endif()
    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast  -DNDEBUG -std=c++11 -DHAVE_CXX0X  -march=native")
    #set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -ggdb   -std=c++11 -DHAVE_CXX0X  -march=native")
    #set (CMAKE_C_FLAGS_RELEASE "-Wall -Ofast  -DNDEBUG -std=c99  -march=native")
    #set (CMAKE_C_FLAGS_DEBUG   "-Wall -ggdb   -std=c99  -march=native")
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.2.1)
        message( FATAL_ERROR  "Clang version must be at least 4.2.1! (provided ${CXX_COMPILER_VERSION}" )
    endif()
    #set (CMAKE_CXX_FLAGS_RELEASE "-Wall -Wcast-align -O3 -DNDEBUG -std=c++11 -DHAVE_CXX0X -msse4.1 -march=native")
    #set (CMAKE_CXX_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -std=c++11 -DHAVE_CXX0X -msse4.1 -march=native")
    #set (CMAKE_C_FLAGS_RELEASE "-Wall -Wcast-align -O3 -DNDEBUG -std=c99 -msse4.1 -march=native")
    #set (CMAKE_C_FLAGS_DEBUG   "-Wall -Wcast-align -ggdb  -std=c99 -msse4.1 -march=native")

elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang")
    message ( STATUS "AppleClang version is ${CMAKE_CXX_COMPILER_VERSION}" )
elseif(WIN32)
    # TODO check MSVC versions?
else ()
    message(FATAL_ERROR "Please, use MSVC, GCC, Clang, or the Intel compiler!")
endif()

export ( TARGETS FastPFOR FILE "${CMAKE_CURRENT_BINARY_DIR}/fastpfor-targets.cmake" NAMESPACE FastPFOR:: )
add_library ( FastPFOR::FastPFOR ALIAS FastPFOR )