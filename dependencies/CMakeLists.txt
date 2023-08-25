set(INSTALL_DESTINATION_DEFAULT
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        FRAMEWORK DESTINATION lib
        BUNDLE DESTINATION bin
        RUNTIME DESTINATION bin)

add_subdirectory( boringssl )

set (OPENSSL_FOUND ON)
set (OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl")
set (OPENSSL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include")

LIST(APPEND BORINGSSL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/boringssl/crypto")
LIST(APPEND BORINGSSL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/boringssl/decrepit")
LIST(APPEND BORINGSSL_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/boringssl/ssl")
set (BORINGSSL_LIBDIR "${CMAKE_CURRENT_BINARY_DIR}/boringssl")

# TODO: When BORINGSSL_LIBRARIES is set, brotli will be disabled.
# TODO:   Should check on adding brotli as a submodule if it's needed
set (picotls_BUILD_TESTS OFF)
add_subdirectory( picotls )

# picotls still attempts to find OpenSSL, which will override some variables. Need
#   set them again after picotls
set (OPENSSL_FOUND ON)
set (OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl")
set (OPENSSL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include")

set (PICOQUIC_PTLS_SUBMODULE ON)
set (picoquic_BUILD_TESTS OFF)
add_subdirectory( picoquic )

# Quiet warnings
set_property(GLOBAL PROPERTY RULE_MESSAGES OFF)
target_compile_options(picotls-core PRIVATE -w)
target_compile_options(picotls-openssl PRIVATE -w)
target_compile_options(picoquic-core PRIVATE -w)

if (NOT TARGET nlohmann_json)
    add_subdirectory(json)
endif()
