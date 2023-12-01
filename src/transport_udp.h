
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
  TransportConnId contextId;
  DataContextId streamId;
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

  TransportConnId start() override;

  void close(const TransportConnId& context_id) override;

  virtual bool getPeerAddrInfo(const TransportConnId& context_id,
                               sockaddr_storage* addr) override;

  DataContextId createDataContext(const TransportConnId conn_id,
                                  bool use_reliable_transport,
                                  uint8_t priority, bool bidir) override;

  void deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) override;

  TransportError enqueue(const TransportConnId& conn_id,
                         const DataContextId& data_ctx_id,
                         std::vector<uint8_t>&& bytes,
                         const uint8_t priority,
                         const uint32_t ttl_ms,
                         const bool new_stream,
                         const bool buffer_reset) override;

  std::optional<std::vector<uint8_t>> dequeue(
    const TransportConnId& context_id,
    const DataContextId&streamId) override;

private:
  TransportConnId connect_client();
  TransportConnId connect_server();

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
    TransportConnId tcid;
    DataContextId sid;
  };

  cantina::LoggerPointer logger;
  int fd; // UDP socket
  bool isServerMode;

  TransportRemote serverInfo;
  Addr serverAddr;
  safe_queue<connData> fd_write_queue;

  // NOTE: this is a map supporting multiple streams, but UDP does not have that
  // right now.
  std::map<TransportConnId, std::map<DataContextId, safe_queue<connData>>>
    dequeue_data_map;

  TransportDelegate& delegate;

  TransportConnId last_context_id{ 0 };
  DataContextId last_stream_id{ 0 };
  std::map<TransportConnId, Addr> remote_contexts = {};
  std::map<addrKey, AddrStream> remote_addrs = {};
};

} // namespace qtransport
