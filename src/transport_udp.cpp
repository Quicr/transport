#include <cassert>
#include <iostream>
#include <string.h> // memcpy
#include <thread>
#include <unistd.h>

#if defined(__linux) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netdb.h>
#endif
#if defined(__linux__)
#include <net/ethernet.h>
#include <netpacket/packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "transport_udp.h"

using namespace qtransport;

UDPTransport::~UDPTransport() { close(); }

TransportStatus UDPTransport::status() const {
  return (fd > 0) ? TransportStatus::Ready : TransportStatus::Disconnected;
}

MediaStreamId
UDPTransport::createMediaStream(const qtransport::TransportContextId &tcid,
                                bool use_reliable_transport) {
  throw std::runtime_error("not supported");
}

TransportContextId UDPTransport::start() {

  if (m_isServer) {
    return connect_server();
  } else {
    connect_client();
  }

  return 0;
}

void UDPTransport::close() {
  if (fd > 0) {
    ::close(fd);
  }
  fd = 0;
}

Error UDPTransport::enqueue(const TransportContextId &tcid,
                            const MediaStreamId &msid,
                            std::vector<uint8_t> &&bytes) {
  if (bytes.empty()) {
    return Error::None;
  }

  if (remote_contexts.count(tcid) == 0) {
    return Error::None;
  }

  auto &r = remote_contexts.at(tcid);

  int numSent = sendto(fd, bytes.data(), bytes.size(), 0 /*flags*/,
                       (struct sockaddr *)&r.addr, sizeof(sockaddr_in));
  if (numSent < 0) {
#if defined(_WIN32)
    int error = WSAGetLastError();
    if (error == WSAETIMEDOUT) {
      return false;
    } else {
      std::cerr << "sending on UDP socket got error: " << WSAGetLastError()
                << std::endl;
      assert(0);
    }
#else
    int e = errno;
    std::cerr << "sending on UDP socket got error: " << strerror(e)
              << std::endl;
    assert(0); // TODO
#endif
  } else if (numSent != (int)bytes.size()) {
    assert(0); // TODO
  }

  return Error::None;
}

std::optional<std::vector<uint8_t>>
UDPTransport::dequeue(const TransportContextId &tcid,
                      const MediaStreamId & /*msid*/) {

  const int dataSize = 1500;
  std::vector<uint8_t> buffer;
  buffer.resize(dataSize);
  struct sockaddr_storage remoteAddr;
  memset(&remoteAddr, 0, sizeof(remoteAddr));
  socklen_t remoteAddrLen = sizeof(remoteAddr);
  int rLen = recvfrom(fd, buffer.data(), buffer.size(), 0 /*flags*/,
                      (struct sockaddr *)&remoteAddr, &remoteAddrLen);
  if (rLen < 0) {
#if defined(_WIN32)
    int error = WSAGetLastError();
    if (error == WSAETIMEDOUT) {
      return false;
    } else {
      std::cerr << "reading from UDP socket got error: " << WSAGetLastError()
                << std::endl;
      assert(0);
    }
#else
    int e = errno;
    if (e == EAGAIN) {
      // timeout on read
      return std::nullopt;
    } else {
      std::cerr << "reading from UDP socket got error: " << strerror(e)
                << std::endl;
      assert(0); // TODO
    }
#endif
  }

  if (rLen == 0) {
    return std::nullopt;
  }

  buffer.resize(rLen);
  // save the remote address
  if (m_isServer) {
    if (remote_contexts.count(tcid) == 0) {
      Remote r;
      r.addr_len = remoteAddrLen;
      memcpy(&(r.addr), &remoteAddr, remoteAddrLen);
      remote_contexts[tcid] = r;
    }
  }
  return buffer;
}

UDPTransport::UDPTransport(const std::string &server_name_in,
                           uint16_t server_port_in,
                           ITransport::TransportDelegate &delegate_in)
    : m_isServer(false), server_name(std::move(server_name_in)),
      server_port(server_port_in), delegate(delegate_in) {}

TransportContextId UDPTransport::connect_client() {
  // create a Client
#if defined(_WIN32)
  WSADATA wsaData;
  int wsa_err = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (wsa_err) {
    assert(0);
  }
#endif

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    throw std::runtime_error("socket() failed");
  }

  // make socket non blocking IO
  struct timeval timeOut;
  timeOut.tv_sec = 0;
  timeOut.tv_usec = 2000; // 2 ms
  int err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeOut,
                       sizeof(timeOut));
  if (err) {
    assert(0); // TODO
  }

  struct sockaddr_in srvAddr;
  srvAddr.sin_family = AF_INET;
  srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  srvAddr.sin_port = 0;
  err = bind(fd, (struct sockaddr *)&srvAddr, sizeof(srvAddr));
  if (err) {
    assert(0);
  }

  std::string sPort = std::to_string(htons(server_port));
  struct addrinfo hints = {}, *address_list = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  err = getaddrinfo(server_name.c_str(), sPort.c_str(), &hints, &address_list);
  if (err) {
    assert(0);
  }
  struct addrinfo *item = nullptr, *found_addr = nullptr;
  for (item = address_list; item != nullptr; item = item->ai_next) {
    if (item->ai_family == AF_INET && item->ai_socktype == SOCK_DGRAM &&
        item->ai_protocol == IPPROTO_UDP) {
      found_addr = item;
      break;
    }
  }
  if (found_addr == nullptr) {
    assert(0);
  }

  struct sockaddr_in *ipv4 = (struct sockaddr_in *)&server_sockaddr;
  memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
  ipv4->sin_port = htons(server_port);
  server_sockaddr_len = sizeof(server_sockaddr);

  Remote r = {};
  r.addr_len = sizeof(server_sockaddr);
  ;
  memcpy(&(r.addr), &server_sockaddr, sizeof(server_sockaddr));

  remote_contexts[transport_context_id] = r;
  return transport_context_id;
}

///
/// Server
///

UDPTransport::UDPTransport(uint16_t server_port_in,
                           ITransport::TransportDelegate &delegate_in)
    : server_port(server_port_in), m_isServer(true), delegate(delegate_in) {}

TransportContextId UDPTransport::connect_server() {
#if defined(_WIN32)
  WSADATA wsaData;
  int wsa_err = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (wsa_err) {
    assert(0);
  }
#endif
  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    assert(0); // TODO
  }

  // set for re-use
  int one = 1;
  int err =
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
  if (err != 0) {
    assert(0); // TODO
  }

  // make socket non blocking IO
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 2000; // 2 ms
  err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                   sizeof(timeout));
  if (err) {
    assert(0); // TODO
  }

  struct sockaddr_in srv_addr;
  memset((char *)&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_port = htons(server_port);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  err = bind(fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (err < 0) {
    assert(0); // TODO
  }

  remote_contexts[transport_context_id] = {};
  transport_context_id += 1;
  std::cout << "UdpSocket: port " << server_port << ", fd " << fd << std::endl;
  return transport_context_id;
}
