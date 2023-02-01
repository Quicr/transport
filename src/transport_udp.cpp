#include <cassert>
#include <iostream>
#include <string.h> // memcpy
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#if defined(__linux__)
#include <net/ethernet.h>
#include <netpacket/packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#endif

#include "transport_udp.h"

using namespace qtransport;

UDPTransport::~UDPTransport() {
  // TODO: Close all media streams and connections
}

UDPTransport::UDPTransport(const TransportRemote &server,
                           TransportDelegate &delegate, bool isServerMode,
                           LogHandler &logger)
    : delegate(delegate) {

  this->isServerMode = isServerMode;
  serverInfo = server;
}

TransportStatus UDPTransport::status() const {
  return (fd > 0) ? TransportStatus::Ready : TransportStatus::Disconnected;
}

MediaStreamId UDPTransport::createMediaStream(
    const qtransport::TransportContextId &context_id,
    bool use_reliable_transport) {

  return last_media_stream_id++;
}

TransportContextId UDPTransport::start() {

  if (isServerMode) {
    return connect_server();
  } else {
    return connect_client();
  }

  return 0;
}

void UDPTransport::closeMediaStream(const TransportContextId &context_id,
                                    const MediaStreamId mStreamId) {

  if (dequeue_data_map[context_id].count(mStreamId) > 0) {
    dequeue_data_map[context_id].erase(mStreamId);
  }
}

void UDPTransport::close(const TransportContextId &context_id) {

  if (not isServerMode) {
    if (fd > 0) {
      ::close(fd);
    }
    fd = 0;

  } else {
    addrKey ak;

    addr_to_key(remote_contexts[context_id].addr, ak);

    remote_addrs.erase(ak);
    remote_contexts.erase(context_id);
    dequeue_data_map.erase(context_id);
  }
}

void UDPTransport::addr_to_key(sockaddr_storage &addr, addrKey &key) {

  key.port = 0;
  key.ip_lo = 0;
  key.ip_hi = 0;

  switch (addr.ss_family) {
  case AF_INET: {
    sockaddr_in *s = (sockaddr_in *)&addr;

    key.port = s->sin_port;
    key.ip_lo = s->sin_addr.s_addr;
    break;
  }
  default: {
    // IPv6
    sockaddr_in6 *s = (sockaddr_in6 *)&addr;

    key.port = s->sin6_port;

    key.ip_hi = (uint64_t)&s->sin6_addr;
    key.ip_lo = (uint64_t)&s->sin6_addr + 8;
    break;
  }
  }
}

void UDPTransport::addr_to_remote(sockaddr_storage &addr,
                                  TransportRemote &remote) {
  char ip[INET6_ADDRSTRLEN];

  remote.proto = TransportProtocol::UDP;

  switch (addr.ss_family) {
  case AF_INET: {
    sockaddr_in *s = (sockaddr_in *)&addr;

    remote.port = s->sin_port;
    inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
    break;
  }
  default: {
    // IPv6
    sockaddr_in6 *s = (sockaddr_in6 *)&addr;

    remote.port = s->sin6_port;
    inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
    break;
  }
  }
}

/*
 * Blocking socket writer. This should be called in its own thread
 *
 * Writer will perform the following:
 *  - loop reads data from fd_write_queue and writes it to the socket
 */
void UDPTransport::fd_writer(const bool &stop) {
  std::cout << "Starting transport writer thread" << std::endl;

  while (not stop) {
    auto cd = fd_write_queue.pop();

    if (cd) {
      if (dequeue_data_map.count(cd->mStreamId) == 0 or
          remote_contexts.count(cd->contextId) == 0) {
        // Drop/ignore connection data since the connection or media stream no
        // longer exists
        continue;
      }

      auto &r = remote_contexts.at(cd->contextId);

      int numSent =
          sendto(fd, cd.value().data.data(), cd.value().data.size(),
                 0 /*flags*/, (struct sockaddr *)&r.addr, sizeof(sockaddr_in));

      if (numSent < 0) {
        int e = errno;
        std::cerr << "sending on UDP socket got error: " << strerror(e)
                  << std::endl;
        assert(0); // TODO

      } else if (numSent != (int)cd.value().data.size()) {
        assert(0); // TODO
      }

    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }
}

/*
 * Blocking socket FD reader. This should be called in its own thread.
 *
 * Reader will perform the following:
 *  - Receive data from socket
 *  - Lookup addr in map to find context and name info
 *  - If context doesn't exist, then it's a new connection and the delegate will
 * be called after creating new context
 *  - Create connData and send to queue
 *  - Call on_recv_notify() delegate to notify of new data available. This is
 * not called again if there is still pending data to be dequeued for the same
 * mediaStreamId
 */
void UDPTransport::fd_reader(const bool &stop) {
  std::cout << "Starting transport reader thread" << std::endl;

  const int dataSize = 9000; // TODO Add config var to set this value, can be up
                             // to 64k with gso/ip frags

  struct sockaddr_storage remoteAddr;
  memset(&remoteAddr, 0, sizeof(remoteAddr));
  socklen_t remoteAddrLen = sizeof(remoteAddr);

  while (not stop) {
    std::vector<uint8_t> buffer;
    buffer.resize(dataSize);

    int rLen = recvfrom(fd, buffer.data(), buffer.size(), 0 /*flags*/,
                        (struct sockaddr *)&remoteAddr, &remoteAddrLen);

    if (rLen < 0) {
      int e = errno;
      if (e == EAGAIN) {
        // timeout on read
        continue;

      } else {
        std::cerr << "reading from UDP socket got error: " << strerror(e)
                  << std::endl;
        assert(0); // TODO
      }
    }

    if (rLen == 0) {
      continue;
    }

    buffer.resize(rLen);

    connData cd;
    cd.data = buffer;
    cd.mStreamId = 0;

    addrKey ra_key;
    addr_to_key(remoteAddr, ra_key);

    if (remote_addrs.count(ra_key) == 0) {
      if (isServerMode) {
        // New remote address/connection
        TransportRemote remote;
        addr_to_remote(remoteAddr, remote);

        Addr r;
        r.addr_len = remoteAddrLen;
        memcpy(&(r.addr), &remoteAddr, remoteAddrLen);

        ++last_context_id;
        ++last_media_stream_id;

        remote_contexts[last_context_id] = r;
        remote_addrs[ra_key] = {last_context_id, last_media_stream_id};

        cd.contextId = last_context_id;
        cd.mStreamId = last_media_stream_id;

        // Create dequeue
        dequeue_data_map[last_context_id][last_media_stream_id].setLimit(50000);

        cd.contextId = last_context_id;

        // Notify caller that there is a new connection
        delegate.on_new_connection(last_context_id, std::move(remote));

      } else {
        // Client mode doesn't support creating connections based on received
        // packets
        continue;
      }
    } else {
      auto sctx = remote_addrs[ra_key];
      cd.contextId = sctx.tcid;
      cd.mStreamId = sctx.msid;
    }

    // Add data to caller queue for processing
    auto &dq = dequeue_data_map[cd.contextId][cd.mStreamId];

    dq.push(cd);

    if (dq.size() < 2) {
      // Notify the caller that there is data to process
      delegate.on_recv_notify(cd.contextId, cd.mStreamId);
    }
  }
}

TransportError UDPTransport::enqueue(const TransportContextId &context_id,
                                     const MediaStreamId &mStreamId,
                                     std::vector<uint8_t> &&bytes) {
  if (bytes.empty()) {
    return TransportError::None;
  }

  if (remote_contexts.count(context_id) == 0) {
    // Invalid context id
    return TransportError::InvalidContextId;
  }

  if (dequeue_data_map[context_id].count(context_id) == 0) {
    // Invalid stream Id
    return TransportError::InvalidStreamId;
  }

  connData cd;
  cd.data = bytes;
  cd.contextId = context_id;
  cd.mStreamId = mStreamId; // Always use stream ID 0 since UDP does not have
                            // multiple streams

  if (not fd_write_queue.push(cd)) {
    return TransportError::QueueFull;
  }

  return TransportError::None;
}

std::optional<std::vector<uint8_t>>
UDPTransport::dequeue(const TransportContextId &context_id,
                      const MediaStreamId &mstreamId) {

  if (remote_contexts.count(context_id) == 0) {
    // Invalid context id
    return std::nullopt;
  }

  if (dequeue_data_map[context_id].count(context_id) == 0) {
    // Invalid stream Id
    return std::nullopt;
  }

  auto &dq = dequeue_data_map[context_id][mstreamId];

  if (dq.size() <= 0) {
    return std::nullopt;
  }

  return dq.pop().value().data;
}

TransportContextId UDPTransport::connect_client() {
  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    throw std::runtime_error("socket() failed");
  }

  struct sockaddr_in srvAddr;
  srvAddr.sin_family = AF_INET;
  srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  srvAddr.sin_port = 0;
  int err = bind(fd, (struct sockaddr *)&srvAddr, sizeof(srvAddr));
  if (err) {
    assert(0);
  }

  std::string sPort = std::to_string(htons(serverInfo.port));
  struct addrinfo hints = {}, *address_list = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  err = getaddrinfo(serverInfo.host_or_ip.c_str(), sPort.c_str(), &hints,
                    &address_list);
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

  struct sockaddr_in *ipv4 = (struct sockaddr_in *)&serverAddr.addr;
  memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
  ipv4->sin_port = htons(serverInfo.port);
  serverAddr.addr_len = sizeof(sockaddr_in);

  addrKey sa_key;
  addr_to_key(serverAddr.addr, sa_key);

  ++last_context_id;
  ++last_media_stream_id;

  remote_contexts[last_context_id] = serverAddr;
  remote_addrs[sa_key] = {last_context_id, last_media_stream_id};

  // Create dequeue
  dequeue_data_map[last_context_id][last_media_stream_id].setLimit(50000);

  // Notify caller that the connection is now ready
  delegate.on_connection_status(last_context_id, TransportStatus::Ready);

  bool stop = false;
  std::thread fd_reader_thr(&UDPTransport::fd_reader, this, stop);
  std::thread fd_writer_thr(&UDPTransport::fd_writer, this, stop);

  fd_reader_thr.detach();
  fd_writer_thr.detach();
  return last_context_id;
}

TransportContextId UDPTransport::connect_server() {
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

  struct sockaddr_in srv_addr;
  memset((char *)&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_port = htons(serverInfo.port);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  err = bind(fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (err < 0) {
    assert(0); // TODO
  }

  std::cout << "UdpSocket: port " << serverInfo.port << ", fd " << fd
            << std::endl;

  bool stop = false;
  std::thread fd_reader_thr(&UDPTransport::fd_reader, this, stop);
  std::thread fd_writer_thr(&UDPTransport::fd_writer, this, stop);
  fd_reader_thr.detach();
  fd_writer_thr.detach();

  return last_context_id;
}
