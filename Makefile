# This is just a convenience Makefile to avoid having to remember
# all the CMake commands and their arguments.

# Set CMAKE_GENERATOR in the environment to select how you build, e.g.:
#   CMAKE_GENERATOR=Ninja

BUILD_DIR=build
CLANG_FORMAT=clang-format -i

.PHONY: all test clean cclean format

all: ${BUILD_DIR}
	cmake --build ${BUILD_DIR} --target forty_bytes 

${BUILD_DIR}: CMakeLists.txt
	cmake -B${BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug .


client: ${BUILD_DIR} cmd/client.cc
	cmake --build ${BUILD_DIR} --target client

server: ${BUILD_DIR} cmd/echoServer.cc
	cmake --build ${BUILD_DIR} --target server

clean:
	cmake --build ${BUILD_DIR} --target clean

cclean:
	rm -rf ${BUILD_DIR}

format:
	find include -iname "*.hh" -or -iname "*.cc" | xargs ${CLANG_FORMAT}
	find src -iname "*.hh" -or -iname "*.cc" | xargs ${CLANG_FORMAT}

