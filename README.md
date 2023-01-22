# Transport Library

Single threaded, aysnc and queued transport api for QUICR

# Buillding

0. Needs C++17 clang, and cmake 
1. Download and install openssl
- on a mac you can do ' ```brew install openssl```
- find where it is ```brew info openssl```
- set path to find it ```export
  PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig"```
2. Clone the repos 
   ``` git clone git@github.com:Quicr/transport.git ```
   ``` git submodule init --recursive ```
   ``` git submodule update --recursive ```
3. In the same parent directory,  run 
    - make all
    - make client
    - make server
      
# Notes

1. cmd/ - has client and server examples
   
   
