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

add_subdirectory( picotls )

# picotls still attempts to find OpenSSL, which will override some variables. Need
#   set them again after picotls
set (OPENSSL_FOUND ON)
set (OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl")
set (OPENSSL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include")

set (PICOQUIC_PTLS_SUBMODULE ON)
set (picoquic_BUILD_TESTS ON)
add_subdirectory( picoquic )
