add_executable(server echoServer.cc)
target_link_libraries(server PRIVATE cantina::logger quicr-transport)
if (NOT PLATFORM_ESP_IDF)
  target_link_libraries(server PRIVATE picotls-core picotls-openssl)
endif()

target_compile_options(server
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)
set_target_properties(server
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)


add_executable(client client.cc)
target_link_libraries(client PRIVATE cantina::logger quicr-transport)
if (NOT PLATFORM_ESP_IDF)
  target_link_libraries(client PRIVATE picotls-core picotls-openssl)
endif()

target_compile_options(client
        PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Wpedantic -Wextra -Wall>
        $<$<CXX_COMPILER_ID:MSVC>: >)
set_target_properties(client
        PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS ON)

