# Test Binary

add_executable(transport_test
                main.cpp
                stream_buffer.cpp)
target_include_directories(transport_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(transport_test PRIVATE quicr-transport doctest::doctest)

target_compile_options(transport_test
    PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)

set_target_properties(transport_test
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

if(CLANG_TIDY_EXE)
    set_target_properties(transport_test PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(transport_test PRIVATE  -Wall -pedantic -Wextra)
endif()

if(MSVC)
    target_compile_options(transport_test PRIVATE /W4 /WX)
    target_compile_definitions(transport_test _CRT_SECURE_NO_WARNINGS)
endif()

include(${CMAKE_SOURCE_DIR}/dependencies/doctest/scripts/cmake/doctest.cmake)
doctest_discover_tests(transport_test)
