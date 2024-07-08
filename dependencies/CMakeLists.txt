# ----------------------------------------------------
# Submodules
# ----------------------------------------------------
add_subdirectory(logger)

if (NOT PLATFORM_ESP_IDF)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_STATIC_LIBS ON)

        # mbedtls
        if (${USE_MBEDTLS})
                # Get python requirements for mbedtls
                find_package(Python3 REQUIRED)
                execute_process(COMMAND ${Python3_EXECUTABLE} -m pip install -r ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/scripts/basic.requirements.txt)

                option(WITH_FUSION OFF)
                set(WITH_MBEDTLS ON)
                OPTION(ENABLE_TESTING OFF)
                OPTION(ENABLE_PROGRAMS OFF)
                SET(MBEDTLS_AS_SUBPROJECT ON)
                add_subdirectory(mbedtls)

                # Load the mbedtls package in before picoquic complains on first compile
                find_package(MbedTLS)

                set(MBEDTLS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls/include)
                set(MBEDTLS_LIBRARIES mbedtls mbedcrypto mbedx509)
                set(MBEDTLS_LIBRARY mbedtls)
                set(MBEDTLS_CRYPTO mbedcrypto)
                set(MBEDTLS_X509 mbedx509)

                # picoTLS
                set(WITH_OPENSSL OFF)
                set(PICOTLS_USE_BROTLI OFF)
                set(picotls_BUILD_TESTS OFF)
                add_subdirectory(picotls)
                add_dependencies(picotls-mbedtls mbedtls)

                # picoQUIC
                set(PICOQUIC_PTLS_SUBMODULE ON)
                option (WITH_OPENSSL OFF)
                set(picoquic_BUILD_TESTS OFF)
                add_subdirectory(picoquic)
                add_dependencies(picoquic-core mbedtls)
        else()
                option(WITH_FUSION OFF)
                OPTION(ENABLE_TESTING OFF)

                # BoringSSL (include only for upstream openssl usage)
                set(boringssl_BUILD_TESTS OFF)
                add_subdirectory(boringssl)

                # Setup OpenSSL vars for submodules
                set(OPENSSL_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl CACHE STRING "" FORCE)
                set(OPENSSL_LIBDIR ${CMAKE_CURRENT_BINARY_DIR}/boringssl CACHE STRING "" FORCE)
                set(OPENSSL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boringssl/include CACHE STRING "" FORCE)
                set(OPENSSL_LIBRARIES BoringSSL::ssl CACHE STRING "" FORCE)
                set(OPENSSL_SSL_LIBRARIES BoringSSL::ssl CACHE STRING "" FORCE)
                set(OPENSSL_SSL_LIBRARY BoringSSL::ssl CACHE STRING "" FORCE)
                set(OPENSSL_CRYPTO_LIBRARY BoringSSL::crypto CACHE STRING "" FORCE)
                set(OPENSSL_CRYPTO_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}" CACHE STRING "" FORCE)

                # Create aliases for this and upstream submodules
                add_library(BoringSSL::ssl ALIAS ssl)
                add_library(BoringSSL::crypto ALIAS crypto)
                add_library(BoringSSL::decrepit ALIAS decrepit)
                add_library(OpenSSL::SSL ALIAS ssl)
                add_library(OpenSSL::Crypto ALIAS crypto)
                export(TARGETS ssl crypto decrepit FILE boringssl.cmake) # Need to export as alias alone in upstream submodules doesn't work


                # picoTLS
                set(WITH_OPENSSL ON)
                set(PICOTLS_USE_BROTLI OFF)
                set(picotls_BUILD_TESTS OFF)
                add_subdirectory(picotls)
                add_dependencies(picotls-openssl ssl decrepit crypto)
                add_dependencies(picotls-core decrepit crypto ssl)
                add_dependencies(test-openssl.t decrepit crypto ssl)

                # picoQUIC
                set(PICOQUIC_PTLS_SUBMODULE ON)
                option(WITH_OPENSSL ON)
                set(picoquic_BUILD_TESTS OFF)
                add_subdirectory(picoquic)
                add_dependencies(picoquic-core picotls-core)


        endif()
endif()

