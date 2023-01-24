
#pragma once

#include <cassert>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <sys/types.h>
#if defined(__linux__) || defined(__APPLE__)
#include <netinet/in.h>
#include <sys/socket.h>
#elif defined(_WIN32)
#include <WinSock2.h>
#include <ws2tcpip.h>
#endif

#include <transport/transport.h>

namespace qtransport {

class UDPTransport : public ITransport {
public:
  UDPTransport(const TransportRemote &server, TransportDelegate &delegate,
               bool isServerMode, LogHandler &logger);

  virtual ~UDPTransport();

  TransportStatus status() const override;

  TransportContextId start() override;

  void close(const TransportContextId &context_id) override;
  void closeMediaStream(const TransportContextId &context_id,
                        MediaStreamId mStreamId) override;

  MediaStreamId createMediaStream(const TransportContextId &context_id,
                                  bool use_reliable_transport) override;

  TransportError enqueue(const TransportContextId &context_id,
                         const MediaStreamId &mStreamId,
                         std::vector<uint8_t> &&bytes) override;

  std::optional<std::vector<uint8_t>>
  dequeue(const TransportContextId &context_id,
          const MediaStreamId &mStreamId) override;

private:
  TransportContextId connect_client();
  TransportContextId connect_server();

  struct Addr {
    socklen_t addr_len;
    struct sockaddr_storage addr;
  };

  int fd; // UDP socket
  bool isServerMode;

  TransportRemote serverInfo;
  Addr serverAddr;

  TransportDelegate &delegate;

  TransportContextId transport_context_id{0};
  std::map<TransportContextId, Addr> remote_contexts = {};
};

} // namespace qtransport
