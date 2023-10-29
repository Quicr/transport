
#pragma once

#include <cassert>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <transport/transport.h>

#include "transport/safe_queue.h"

namespace qtransport {

struct addrKey
{
  uint64_t ip_hi;
  uint64_t ip_lo;
  uint16_t port;

  addrKey()
  {
    ip_hi = 0;
    ip_lo = 0;
    port = 0;
  }

  bool operator==(const addrKey& o) const
  {
    return ip_hi == o.ip_hi && ip_lo == o.ip_lo && port == o.port;
  }

  bool operator<(const addrKey& o) const
  {
    return std::tie(ip_hi, ip_lo, port) < std::tie(o.ip_hi, o.ip_lo, o.port);
  }
};

struct connData
{
  TransportContextId contextId;
  StreamId streamId;
  std::vector<uint8_t> data;
};

class UDPTransport : public ITransport
{
public:
  UDPTransport(const TransportRemote& server,
               TransportDelegate& delegate,
               bool isServerMode,
               const cantina::LoggerPointer& logger);

  virtual ~UDPTransport();

  TransportStatus status() const override;

  TransportContextId start() override;

  void close(const TransportContextId& context_id) override;
  void closeStream(const TransportContextId& context_id,
                   StreamId streamId) override;

  StreamId create_unidirectional_stream(const TransportContextId& context_id,
                                        uint8_t priority) override {
      throw std::runtime_error("not supported");
  }

  void close_unidirectional_stream(const TransportContextId& context_id,
                                   uint64_t stream_id, bool is_sender) override {
      throw std::runtime_error("not supported");
  }

  virtual bool getPeerAddrInfo(const TransportContextId& context_id,
                               sockaddr_storage* addr) override;

  StreamId createStream(const TransportContextId& context_id,
                        bool use_reliable_transport,
                        uint8_t priority) override;


  TransportError enqueue(const TransportContextId& context_id,
                         const StreamId & stream_id,
                         std::vector<uint8_t>&& bytes,
                         const uint8_t priority = 1,
                         const uint32_t ttl_ms = 300) override;



    std::optional<std::vector<uint8_t>> dequeue(
    const TransportContextId& context_id,
    const StreamId &streamId) override;

private:
  TransportContextId connect_client();
  TransportContextId connect_server();

  void addr_to_remote(sockaddr_storage& addr, TransportRemote& remote);
  void addr_to_key(sockaddr_storage& addr, addrKey& key);

  void fd_reader();
  void fd_writer();

  bool stop;
  std::vector<std::thread> running_threads;

  struct Addr
  {
    socklen_t addr_len;
    struct sockaddr_storage addr;
    addrKey key;
  };

  struct AddrStream
  {
    TransportContextId tcid;
    StreamId sid;
  };

  cantina::LoggerPointer logger;
  int fd; // UDP socket
  bool isServerMode;

  TransportRemote serverInfo;
  Addr serverAddr;
  safe_queue<connData> fd_write_queue;

  // NOTE: this is a map supporting multiple streams, but UDP does not have that
  // right now.
  std::map<TransportContextId, std::map<StreamId, safe_queue<connData>>>
    dequeue_data_map;

  TransportDelegate& delegate;

  TransportContextId last_context_id{ 0 };
  StreamId last_stream_id{ 0 };
  std::map<TransportContextId, Addr> remote_contexts = {};
  std::map<addrKey, AddrStream> remote_addrs = {};
};

} // namespace qtransport
