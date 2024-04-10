# ----------------------------------------------------
# Submodules
# ----------------------------------------------------
add_subdirectory(logger)

if (NOT PLATFORM_ESP_IDF)
        set (BUILD_SHARED_LIBS OFF)
        set (BUILD_STATIC_LIBS ON)

        # BoringSSL (include only for upstream openssl usage)
        set (boringssl_BUILD_TESTS OFF)
        add_subdirectory( boringssl )

        # Setup OpenSSL vars for submodules
        set (OPENSSL_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl CACHE STRING "" FORCE)
        set (OPENSSL_LIBDIR ${CMAKE_CURRENT_BINARY_DIR}/boringssl CACHE STRING "" FORCE)
        set (OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include CACHE STRING "" FORCE)
        set (OPENSSL_LIBRARIES BoringSSL::ssl CACHE STRING "" FORCE)
        set (OPENSSL_SSL_LIBRARIES BoringSSL::ssl CACHE STRING "" FORCE)
        set (OPENSSL_SSL_LIBRARY BoringSSL::ssl CACHE STRING "" FORCE)
        set (OPENSSL_CRYPTO_LIBRARY BoringSSL::crypto CACHE STRING "" FORCE)
        set (OPENSSL_CRYPTO_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "" FORCE)

        # Create aliases for this and upstream submodules
        add_library( BoringSSL::ssl ALIAS ssl)
        add_library( BoringSSL::crypto ALIAS crypto)
        add_library( BoringSSL::decrepit ALIAS decrepit)
        add_library( OpenSSL::SSL ALIAS ssl)
        add_library( OpenSSL::Crypto ALIAS crypto)
        export(TARGETS ssl crypto decrepit FILE boringssl.cmake) # Need to export as alias alone in upstream submodules doesn't work


        # picoTLS
        set (WITH_OPENSSL ON)
        set (PICOTLS_USE_BROTLI OFF)
        set (picotls_BUILD_TESTS OFF)
        add_subdirectory( picotls )
        add_dependencies( picotls-openssl ssl decrepit crypto)
        add_dependencies( picotls-core decrepit crypto ssl )
        add_dependencies( test-openssl.t decrepit crypto ssl )

        # picoQUIC
        set (PICOQUIC_PTLS_SUBMODULE ON)
        option (WITH_OPENSSL ON)
        set (picoquic_BUILD_TESTS OFF)
        add_subdirectory( picoquic )
        add_dependencies( picoquic-core picotls-core )
endif()

