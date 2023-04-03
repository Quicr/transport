cmake_minimum_required(VERSION 3.17)

project(quicr-transport
        VERSION 1.0.0.0
        DESCRIPTION "transport lib for QUICR project"
        LANGUAGES CXX)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(QTRANSPORT_BUILD_TESTS "Build tests for quicr transport" ON)
else()
    option(QTRANSPORT_BUILD_TESTS "Build tests for quicr trasnsport" OFF)
endif()


find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

###
### Dependencies
###
add_subdirectory(dependencies)

###
### Main code
###
add_library(quicr-transport
        src/transport.cpp
        src/transport_udp.cpp
        src/transport_picoquic.cpp
        )

target_link_libraries(quicr-transport
        PUBLIC
        picoquic-core)
target_include_directories(quicr-transport PUBLIC include src )
set_target_properties(quicr-transport
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_options(quicr-transport
        PRIVATE
            $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>,$<C_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
            $<$<C_COMPILER_ID:MSVC>: >)

if(MSVC)
    target_compile_definitions(quicr-transport _CRT_SECURE_NO_WARNINGS)
endif()

###
### Applications
###

if(BUILD_TESTING AND QTRANSPORT_BUILD_TESTS)
  add_subdirectory(cmd)
endif()
