#include <cassert>
#include <cstring> // memcpy
#include <iostream>
#include <sstream>
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
    : delegate(delegate), logger(logger) {

  this->isServerMode = isServerMode;
  serverInfo = server;
}

TransportStatus UDPTransport::status() const {
  return (fd > 0) ? TransportStatus::Ready : TransportStatus::Disconnected;
}

/*
 * UDP doesn't support multiple streams for clients.  This will return the same
 * stream ID used for the context
 */
MediaStreamId UDPTransport::createMediaStream(
    const qtransport::TransportContextId &context_id,
    bool use_reliable_transport) {

  if (remote_contexts.count(context_id) == 0) {
    std::stringstream err;
    err << "Invalid context id: " << context_id;
    logger.log(LogLevel::error, err.str());
    return 0; // Error
  }

  auto addr = remote_contexts[context_id];
  return remote_addrs[addr.key].msid;
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

  logger.log(LogLevel::info, "Starting transport writer thread");

  while (not stop) {
    auto cd = fd_write_queue.block_pop();

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
        std::stringstream err;
        err << "Error sending on UDP socket: " << strerror(errno);
        logger.log(LogLevel::error, err.str());
        assert(0); // TODO

      } else if (numSent != (int)cd.value().data.size()) {
        assert(0); // TODO
      }
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
  logger.log(LogLevel::info, "Starting transport reader thread");

  const int dataSize = 65535; // TODO Add config var to set this value.  Sizes
                              // larger than actual MTU require IP frags
  struct sockaddr_storage remoteAddr;
  memset(&remoteAddr, 0, sizeof(remoteAddr));
  socklen_t remoteAddrLen = sizeof(remoteAddr);

  while (not stop) {
    std::vector<uint8_t> buffer;
    buffer.resize(dataSize);

    int rLen = recvfrom(fd, buffer.data(), buffer.size(), 0 /*flags*/,
                        (struct sockaddr *)&remoteAddr, &remoteAddrLen);

    if (rLen < 0) {
      if (errno == EAGAIN) {
        // timeout on read
        continue;

      } else {
        std::stringstream err;
        err << "Error reading from UDP socket: " << strerror(errno);
        logger.log(LogLevel::error, err.str());
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
        r.key = ra_key;
        r.addr_len = remoteAddrLen;
        memcpy(&(r.addr), &remoteAddr, remoteAddrLen);

        ++last_context_id;
        ++last_media_stream_id;

        remote_contexts[last_context_id] = r;
        remote_addrs[ra_key] = {
            last_context_id,
            last_media_stream_id,
        };

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
  cd.mStreamId = mStreamId;

  if (not fd_write_queue.push(cd)) {
    return TransportError::QueueFull;
  }

  return TransportError::None;
}

std::optional<std::vector<uint8_t>>
UDPTransport::dequeue(const TransportContextId &context_id,
                      const MediaStreamId &mstreamId) {

  if (remote_contexts.count(context_id) == 0) {
    std::stringstream err;
    err << "dequeue: invalid context id: " << context_id;
    logger.log(LogLevel::warn, err.str());
    // Invalid context id
    return std::nullopt;
  }

  if (dequeue_data_map[context_id].count(context_id) == 0) {
    std::stringstream err;
    err << "dequeue: invalid stream id: " << mstreamId;
    logger.log(LogLevel::warn, err.str());

    return std::nullopt;
  }

  auto &dq = dequeue_data_map[context_id][mstreamId];

  if (dq.size() <= 0) {
    return std::nullopt;
  }

  return dq.pop().value().data;
}

TransportContextId UDPTransport::connect_client() {
  std::stringstream s_log;

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
    s_log << "client_connect: Unable to bind to socket: " << strerror(errno);
    logger.log(LogLevel::fatal, s_log.str());
    throw std::runtime_error(s_log.str());
  }

  std::string sPort = std::to_string(htons(serverInfo.port));
  struct addrinfo hints = {}, *address_list = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  err = getaddrinfo(serverInfo.host_or_ip.c_str(), sPort.c_str(), &hints,
                    &address_list);
  if (err) {
    strerror(1);
    s_log << "client_connect: Unable to resolve remote ip address: "
          << strerror(errno);
    logger.log(LogLevel::fatal, s_log.str());
    throw std::runtime_error(s_log.str());
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
    logger.log(LogLevel::fatal, "client_connect: No IP address found");
    throw std::runtime_error("client_connect: No IP address found");
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
  std::stringstream s_log;

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    s_log << "connect_server: Unable to create socket: " << strerror(errno);
    logger.log(LogLevel::fatal, s_log.str());
    throw std::runtime_error(s_log.str());
  }

  // set for re-use
  int one = 1;
  int err =
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
  if (err != 0) {
    s_log << "connect_server: setsockopt error: " << strerror(errno);
    logger.log(LogLevel::fatal, s_log.str());
    throw std::runtime_error(s_log.str());
  }

  struct sockaddr_in srv_addr;
  memset((char *)&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_port = htons(serverInfo.port);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  err = bind(fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (err < 0) {
    s_log << "connect_server: unable to bind to socket: " << strerror(errno);
    logger.log(LogLevel::fatal, s_log.str());
    throw std::runtime_error(s_log.str());
  }

  s_log.str(std::string());
  s_log << "connect_server: port: " << serverInfo.port << " fd: " << fd;
  logger.log(LogLevel::info, s_log.str());

  bool stop = false;
  std::thread fd_reader_thr(&UDPTransport::fd_reader, this, stop);
  std::thread fd_writer_thr(&UDPTransport::fd_writer, this, stop);
  fd_reader_thr.detach();
  fd_writer_thr.detach();

  return last_context_id;
}
