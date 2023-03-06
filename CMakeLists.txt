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

# Bring in dependencies
set( PICOQUIC_FETCH_PTLS Off )
#add_subdirectory( contrib/picoquic )
  
#find_package(OpenSSL REQUIRED)
#message(STATUS "root: ${OPENSSL_ROOT_DIR}")
#message(STATUS "OpenSSL_VERSION: ${OPENSSL_VERSION}")
#message(STATUS "OpenSSL_INCLUDE_DIR: ${OPENSSL_INCLUDE_DIR}")
#message(STATUS "OpenSSL_LIBRARIES: ${OPENSSL_LIBRARIES}")

add_library(quicr-transport
        src/transport.cpp
        src/transport_udp.cpp
        )

target_link_libraries(quicr-transport)
target_include_directories(quicr-transport PUBLIC include src )
set_target_properties(quicr-transport
        PROPERTIES
        CXX_STANDARD 17
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

install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION lib)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/transport DESTINATION include/${PROJECT_NAME})
