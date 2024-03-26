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
        set (OPENSSL_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl CACHE TRUE "" FORCE)
        set (OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include CACHE TRUE "" FORCE)
        set (OPENSSL_LIBRARIES BoringSSL::ssl CACHE TRUE "" FORCE)
        set (OPENSSL_SSL_LIBRARIES BoringSSL::ssl CACHE TRUE "" FORCE)
        set (OPENSSL_SSL_LIBRARY BoringSSL::ssl CACHE TRUE "" FORCE)
        set (OPENSSL_CRYPTO_LIBRARY BoringSSL::ssl CACHE TRUE "" FORCE)
        set (OPENSSL_CRYPTO_LIBRARIES BoringSSL::ssl CACHE TRUE "" FORCE)

        # Create aliases for this and upstream submodules
        add_library( BoringSSL::ssl ALIAS ssl)
        add_library( BoringSSL::crypto ALIAS crypto)
        add_library( OpenSSL::SSL ALIAS ssl)
        add_library( OpenSSL::Crypto ALIAS crypto)
        export(TARGETS ssl crypto FILE boringssl.cmake) # Need to export as alias alone in upstream submodules doesn't work


        # picoTLS
        set (WITH_OPENSSL ON)
        set (PICOTLS_USE_BROTLI OFF)
        set (picotls_BUILD_TESTS OFF)
        add_subdirectory( picotls )
        add_dependencies( picotls-core BoringSSL::ssl )

        # picoQUIC
        set (PICOQUIC_PTLS_SUBMODULE ON)
        option (WITH_OPENSSL ON)
        set (picoquic_BUILD_TESTS OFF)
        add_subdirectory( picoquic )
        add_dependencies( picoquic-core picotls-core )
endif()

