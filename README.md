# Transport Library

Single threaded, aysnc and queued transport api for QUICR

# Buillding

0. Needs C++20 clang, and cmake
1. Download and install openssl
- on a mac you can do ' ```brew install openssl```
- find where it is ```brew info openssl```
- set path to find it ```export
  PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig"```
2. Clone the repos
   ``` git clone git@github.com:Quicr/transport.git  ```
   ``` cd transport ```
   ``` git submodule update --init --recursive ```
3. In the same parent directory,  run
    - make all
    - make client
    - make server

# Running

In order to test QUIC, the server needs to have a certificate. The program expects
the

Generate self-signed certificate for really server to test with

with BoringSSL:

```
openssl req -nodes -x509 -newkey rsa:2048 -days 365 -keyout server-key.pem -out server-cert.pem
```

with MbedTLS:
```
openssl req -nodes -x509 -newkey ec:<(openssl ecparam -name secp256r1) -days 365 -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=test.quicr.ctgpoc.com" -keyout server-key.pem -out server-cert.pem
```

Run:

```
RELAY_PORT=1234 build/cmd/server
```

and

```
RELAY_HOST=localhost RELAY_PORT=1234 build/cmd/client
```

## Development

This project generally follows Google's C++ styling, specifically enforced for naming and formatting: https://google.github.io/styleguide/cppguide.html

# Notes

1. cmd/ - has client and server examples



