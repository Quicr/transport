# This is just a convenience Makefile to avoid having to remember
# all the CMake commands and their arguments.

# Set CMAKE_GENERATOR in the environment to select how you build, e.g.:
#   CMAKE_GENERATOR=Ninja

BUILD_DIR?=build
CLANG_FORMAT=clang-format -i

.PHONY: all test clean cclean format

all: ${BUILD_DIR}
	cmake --build ${BUILD_DIR} --parallel 8

${BUILD_DIR}: CMakeLists.txt cmd/CMakeLists.txt
	cmake -B${BUILD_DIR} -DBUILD_TESTING=ON -DQTRANSPORT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DUSE_MBEDTLS=ON .

cert:
	@echo "Creating certs in ${BUILD_DIR}/cmd"
	@openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
		-subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=test.quicr.ctgpoc.com" \
		-keyout ${BUILD_DIR}/cmd/server-key.pem -out ${BUILD_DIR}/cmd/server-cert.pem

clean:
	cmake --build ${BUILD_DIR} --target clean

cclean:
	rm -rf ${BUILD_DIR}

format:
	find include -iname "*.h" -or -iname "*.cpp" -or -iname "*.cc" | xargs ${CLANG_FORMAT}
	find src -iname "*.h" -or -iname "*.cpp" -or -iname "*.cc" | xargs ${CLANG_FORMAT}
	find cmd -iname "*.h" -or -iname "*.cpp" -or -iname "*.cc" | xargs ${CLANG_FORMAT}
	find test -iname "*.h" -or -iname "*.cpp" -or -iname "*.cc" | xargs ${CLANG_FORMAT}
