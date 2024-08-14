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

option(PLATFORM_ESP_IDF "Enabble suppport for esp-idf (Default OFF)" OFF)
option(USE_MBEDTLS OFF)
option(LINT "Lint using clang-tidy" OFF)

if (NOT PLATFORM_ESP_IDF)
  find_package(Threads REQUIRED)
  find_package(PkgConfig REQUIRED)
else()
  set(USE_MBEDTLS ON)
endif()

###
### Dependencies
###
set (CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
add_subdirectory(dependencies)

###
### Main code
###
set (BUILD_SHARED_LIBS OFF)
set (BUILD_STATIC_LIBS ON)

set (sources
    src/transport.cpp
    src/transport_udp.cpp
)

if (NOT PLATFORM_ESP_IDF)
    list(APPEND sources src/transport_picoquic.cpp)
endif()

add_library(quicr-transport ${sources})

target_link_libraries(quicr-transport
    PRIVATE
    spdlog
)

if (NOT PLATFORM_ESP_IDF)
  target_link_libraries(quicr-transport
          PUBLIC
          picoquic-core picoquic-log)
endif()

if (PLATFORM_ESP_IDF)
  add_compile_definitions(${LIB_NAME} PLATFORM_ESP)
endif()

target_include_directories(quicr-transport PUBLIC include src )
set_target_properties(quicr-transport
        PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)

# linting
if (LINT)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  find_program(CLANG_TIDY_EXE clang-tidy REQUIRED)
  set_target_properties(quicr-transport
    PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
      CXX_CLANG_TIDY_EXPORT_FIXES_DIR "clang-tidy-fixes")
endif (LINT)

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
