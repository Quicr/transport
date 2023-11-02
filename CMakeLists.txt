cmake_minimum_required(VERSION 3.17)

project(quicr-transport
        VERSION 1.0.0.0
        DESCRIPTION "transport lib for QUICR project"
        LANGUAGES CXX)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(QTRANSPORT_BUILD_TESTS "Build tests for quicr transport" ON)
else()
    option(QTRANSPORT_BUILD_TESTS "Build tests for quicr transport" OFF)
endif()

option(PLATFORM_ESP_IDF "Enabble suppport for esp-idf (Default OFF)" ON)

if (NOT PLATFORM_ESP_IDF)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
endif()

###
### Dependencies
###
add_subdirectory(dependencies)

###
### Main code
###

set (sources 
    src/transport.cpp
    src/transport_udp.cpp
)

if (NOT PLATFORM_ESP_IDF)
    list(APPEND sources src/transport_picoquic.cpp)
endif()

add_library(quicr-transport ${sources})

target_link_libraries(quicr-transport
    PUBLIC
    cantina::logger
)
  

if (NOT PLATFORM_ESP_IDF)
  target_link_libraries(quicr-transport
          PUBLIC
          picoquic-core)
endif()          

if (PLATFORM_ESP_IDF)
  add_compile_definitions(${LIB_NAME} PLATFORM_ESP)          
endif()

target_include_directories(quicr-transport PUBLIC include src )
set_target_properties(quicr-transport
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

target_compile_options(quicr-transport
        PRIVATE
            $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>,$<C_COMPILER_ID:GNU>>: -Wpedantic -Wextra>
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
