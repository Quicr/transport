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

#if defined(PLATFORM_ESP)
#include <lwip/netdb.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

using namespace qtransport;

#if defined (PLATFORM_ESP)
static esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio)
{
    auto cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.pin_to_core = core_id;
    cfg.stack_size = stack;
    cfg.prio = prio;
    return cfg;
}
#endif

UDPTransport::~UDPTransport()
{
  // TODO: Close all streams and connections

  // Stop threads
  stop = true;

  // Clear threads from the queue
  fd_write_queue.stop_waiting();

  // Close socket fd
  if (fd >= 0)
    ::close(fd);

  logger->Log("Closing transport threads");
  for (auto &thread : running_threads) {
    if (thread.joinable())
        thread.join();
  }
}

UDPTransport::UDPTransport(const TransportRemote& server,
                           TransportDelegate& delegate,
                           bool isServerMode,
                           const cantina::LoggerPointer& logger)
  : stop(false)
  , logger(std::make_shared<cantina::Logger>("UDP", logger))
  , fd(-1)
  , isServerMode(isServerMode)
  , serverInfo(server)
  , delegate(delegate)
{
}

TransportStatus
UDPTransport::status() const
{
  return (fd > 0) ? TransportStatus::Ready : TransportStatus::Disconnected;
}

/*
 * UDP doesn't support multiple streams for clients.  This will return the same
 * stream ID used for the context
 */
DataContextId
UDPTransport::createDataContext(
  const qtransport::TransportConnId conn_id,
  [[maybe_unused]] bool use_reliable_transport,
  [[maybe_unused]] uint8_t priority,
  [[maybe_unused]] bool bidir)
{

  if (remote_contexts.count(conn_id) == 0) {
    logger->error << "Invalid context id: " << conn_id << std::flush;
    return 0; // Error
  }

  auto addr = remote_contexts[conn_id];
  return remote_addrs[addr.key].sid;
}

TransportConnId
UDPTransport::start()
{

  if (isServerMode) {
    return connect_server();
  } else {
    return connect_client();
  }

  return 0;
}

void
UDPTransport::deleteDataContext(const TransportConnId& context_id, DataContextId streamId)
{

  if (dequeue_data_map[context_id].count(streamId) > 0) {
    dequeue_data_map[context_id].erase(streamId);
  }
}

bool
UDPTransport::getPeerAddrInfo(const TransportConnId& context_id,
                              sockaddr_storage* addr)
{
  // Locate the given transport context
  auto it = remote_contexts.find(context_id);

  // If not found, return false
  if (it == remote_contexts.end()) return false;

  // Copy the address information
  std::memcpy(addr, &it->second.addr, sizeof(it->second.addr));

  return true;
}

void
UDPTransport::close(const TransportConnId& context_id)
{

  if (isServerMode) {
    addrKey ak;

    addr_to_key(remote_contexts[context_id].addr, ak);

    remote_addrs.erase(ak);
    remote_contexts.erase(context_id);
    dequeue_data_map.erase(context_id);
  }
}

void
UDPTransport::addr_to_key(sockaddr_storage& addr, addrKey& key)
{

  key.port = 0;
  key.ip_lo = 0;
  key.ip_hi = 0;

  switch (addr.ss_family) {
    case AF_INET: {
      sockaddr_in* s = (sockaddr_in*)&addr;

      key.port = s->sin_port;
      key.ip_lo = s->sin_addr.s_addr;
      break;
    }
    default: {
      // IPv6
      sockaddr_in6* s = (sockaddr_in6*)&addr;

      key.port = s->sin6_port;

      key.ip_hi = (uint64_t)&s->sin6_addr;
      key.ip_lo = (uint64_t)&s->sin6_addr + 8;
      break;
    }
  }
}

void
UDPTransport::addr_to_remote(sockaddr_storage& addr, TransportRemote& remote)
{
  char ip[INET6_ADDRSTRLEN];

  remote.proto = TransportProtocol::UDP;

  switch (addr.ss_family) {
    case AF_INET: {
      sockaddr_in* s = (sockaddr_in*)&addr;

      remote.port = s->sin_port;
      inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
      break;
    }
    default: {
      // IPv6
      sockaddr_in6* s = (sockaddr_in6*)&addr;

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
void
UDPTransport::fd_writer()
{

  logger->Log("Starting transport writer thread");

  while (not stop) {
    auto cd = fd_write_queue.block_pop();

    if (cd) {
      if ((dequeue_data_map.count(cd->contextId) == 0 || dequeue_data_map[cd->contextId].count(cd->streamId) == 0)
          || remote_contexts.count(cd->contextId) == 0) {
        // Drop/ignore connection data since the connection or stream no
        // longer exists
        continue;
      }

      auto& r = remote_contexts.at(cd->contextId);

      int numSent = sendto(fd,
                           cd.value().data.data(),
                           cd.value().data.size(),
                           0 /*flags*/,
                           (struct sockaddr*)&r.addr,
                           sizeof(sockaddr_in));

      if (numSent < 0) {
        logger->error << "Error sending on UDP socket: " << strerror(errno)
                      << std::flush;

        break;

      } else if (numSent != (int)cd.value().data.size()) {
        continue;
      }
    }
  }

  logger->Log("Done transport writer thread");
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
 * StreamId
 */
void
UDPTransport::fd_reader()
{
  logger->Log(cantina::LogLevel::Info, "Starting transport reader thread");
#if defined(PLATFORM_ESP)
  // TODO (Suhas): Revisit this once we have basic esp functionality working
  const int dataSize = 2048;
#else
  const int dataSize = 65535; // TODO Add config var to set this value.
                              // larger than actual MTU require IP frags
#endif
  struct sockaddr_storage remoteAddr;
  memset(&remoteAddr, 0, sizeof(remoteAddr));
  socklen_t remoteAddrLen = sizeof(remoteAddr);

  uint8_t data[dataSize];

  while (not stop) {
    int rLen = recvfrom(fd,
                        data,
                        dataSize,
                        0 /*flags*/,
                        (struct sockaddr*)&remoteAddr,
                        &remoteAddrLen);

    if (rLen < 0 || stop) {
      if ((errno == EAGAIN) || (stop)) {
        // timeout on read or stop issued
        continue;

      } else {
        logger->error << "Error reading from UDP socket: " << strerror(errno)
                      << std::flush;
        break;
      }
    }

    if (rLen == 0) {
      continue;
    }

    std::vector<uint8_t> buffer (data, data + rLen);

    connData cd;
    cd.data = buffer;
    cd.streamId = 0;

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
        ++last_stream_id;

        remote_contexts[last_context_id] = r;
        remote_addrs[ra_key] = {
          last_context_id,
            last_stream_id,
        };

        cd.contextId = last_context_id;
        cd.streamId = last_stream_id;

        // Create dequeue
        dequeue_data_map[last_context_id][last_stream_id].set_limit(1000);

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
      cd.streamId = sctx.sid;
    }

    // Add data to caller queue for processing
    auto& dq = dequeue_data_map[cd.contextId][cd.streamId];

    // TODO: Notify caller that packets are being dropped on queue full
    dq.push(cd);

    if (dq.size() < 2) {
      // Notify the caller that there is data to process
      delegate.on_recv_notify(cd.contextId, cd.streamId);
    }
  }

  logger->Log("Done transport reader thread");
}

TransportError
UDPTransport::enqueue(const TransportConnId& context_id,
                      const DataContextId& streamId,
                      std::vector<uint8_t>&& bytes,
                      [[maybe_unused]] const uint8_t priority,
                      [[maybe_unused]] const uint32_t ttl_m,
                      [[maybe_unused]] const EnqueueFlags flags)
{
  if (bytes.empty()) {
    return TransportError::None;
  }

  if (remote_contexts.count(context_id) == 0) {
    // Invalid context id
    return TransportError::InvalidConnContextId;
  }

  if (dequeue_data_map[context_id].count(streamId) == 0) {
    // Invalid stream Id
    return TransportError::InvalidDataContextId;
  }

  connData cd;
  cd.data = bytes;
  cd.contextId = context_id;
  cd.streamId = streamId;

  if (not fd_write_queue.push(cd)) {
    return TransportError::QueueFull;
  }

  return TransportError::None;
}

std::optional<std::vector<uint8_t>>
UDPTransport::dequeue(const TransportConnId& context_id,
                      const DataContextId& streamId)
{

  if (remote_contexts.count(context_id) == 0) {
    logger->warning << "dequeue: invalid context id: " << context_id
                    << std::flush;
    // Invalid context id
    return std::nullopt;
  }

  if (dequeue_data_map[context_id].count(streamId) == 0) {
    logger->error << "dequeue: invalid stream id: " << streamId << std::flush;

    return std::nullopt;
  }

  auto& dq = dequeue_data_map[context_id][streamId];

  if (dq.size() <= 0) {
    return std::nullopt;
  }

  return dq.pop().value().data;
}

TransportConnId
UDPTransport::connect_client()
{
  std::stringstream s_log;

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
#if defined(PLATFORM_ESP)
    // TODO: Suhas: Figure out better API than aborting
    abort();
#else
    throw std::runtime_error("socket() failed");
#endif
  }

int err = 0;
#if not defined(PLATFORM_ESP)
  // TODO: Add config for these values
  size_t snd_rcv_max = 2000000;
  timeval rcv_timeout { .tv_sec = 0, .tv_usec = 10000 };

  err =
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
  if (err != 0) {
    s_log << "client_connect: Unable to set send buffer size: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }

  err =
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
  if (err != 0) {
    s_log << "client_connect: Unable to set receive buffer size: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }
#endif

  err =
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
  if (err != 0) {
    s_log << "client_connect: Unable to set receive timeout: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       
  }


  struct sockaddr_in srvAddr;
  srvAddr.sin_family = AF_INET;
  srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  srvAddr.sin_port = 0;
  err = bind(fd, (struct sockaddr*)&srvAddr, sizeof(srvAddr));
  if (err) {
    s_log << "client_connect: Unable to bind to socket: " << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       
  }

  std::string sPort = std::to_string(htons(serverInfo.port));
  struct addrinfo hints = {}, *address_list = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  err = getaddrinfo(
    serverInfo.host_or_ip.c_str(), sPort.c_str(), &hints, &address_list);
  if (err) {
    strerror(1);
    s_log << "client_connect: Unable to resolve remote ip address: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       
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
    logger->critical << "client_connect: No IP address found" << std::flush;
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       
  }

  struct sockaddr_in* ipv4 = (struct sockaddr_in*)&serverAddr.addr;
  memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
  ipv4->sin_port = htons(serverInfo.port);
  serverAddr.addr_len = sizeof(sockaddr_in);

  freeaddrinfo(address_list);

  addrKey sa_key;
  addr_to_key(serverAddr.addr, sa_key);

  serverAddr.key = sa_key;

  ++last_context_id;
  ++last_stream_id;

  remote_contexts[last_context_id] = serverAddr;
  remote_addrs[sa_key] = { last_context_id, last_stream_id};

  // Create dequeue
  dequeue_data_map[last_context_id][last_stream_id].set_limit(50000);

  // Notify caller that the connection is now ready
  delegate.on_connection_status(last_context_id, TransportStatus::Ready);

#if defined(PLATFORM_ESP)
  auto cfg = create_config("FDReader", 1, 12 * 1024, 5);
  auto esp_err = esp_pthread_set_cfg(&cfg);
  if(esp_err != ESP_OK) {
    s_log << "esp_pthread_set_cfg failed " << esp_err_to_name(esp_err);
    throw std::runtime_error(s_log.str());
  }
#endif

  running_threads.emplace_back(&UDPTransport::fd_reader, this);

#if defined(PLATFORM_ESP)
  cfg = create_config("FDWriter", 1, 12 * 1024, 5);
  esp_err = esp_pthread_set_cfg(&cfg);
  if(esp_err != ESP_OK) {
    s_log << "esp_pthread_set_cfg failed " << esp_err_to_name(esp_err);
    throw std::runtime_error(s_log.str());
  }
#endif
  
  running_threads.emplace_back(&UDPTransport::fd_writer, this);

  return last_context_id;
}

TransportConnId
UDPTransport::connect_server()
{
  std::stringstream s_log;

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    s_log << "connect_server: Unable to create socket: " << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       
  }

  // set for re-use
  int one = 1;
  int err =
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  if (err != 0) {
    s_log << "connect_server: setsockopt error: " << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }

  // TODO: Add config for this value
  size_t snd_rcv_max = 2000000;
  timeval rcv_timeout { .tv_sec = 0, .tv_usec = 10000 };

  err =
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
  if (err != 0) {
    s_log << "client_connect: Unable to set send buffer size: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }

  err =
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
  if (err != 0) {
    s_log << "client_connect: Unable to set receive buffer size: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }

  err =
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
  if (err != 0) {
    s_log << "client_connect: Unable to set receive timeout: "
          << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }


  struct sockaddr_in srv_addr;
  memset((char*)&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_port = htons(serverInfo.port);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  err = bind(fd, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
  if (err < 0) {
    s_log << "connect_server: unable to bind to socket: " << strerror(errno);
    logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)    
    throw std::runtime_error(s_log.str());
 #else
    abort();
#endif       

  }

  logger->info << "connect_server: port: " << serverInfo.port << " fd: " << fd
               << std::flush;

  running_threads.emplace_back(&UDPTransport::fd_reader, this);
  running_threads.emplace_back(&UDPTransport::fd_writer, this);

  return last_context_id;}
