
#pragma once

#include <cassert>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <map>

#include <sys/types.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#elif defined(_WIN32)
#include <WinSock2.h>
#include <ws2tcpip.h>
#endif

#include <transport/transport.h>

namespace qtransport {

class UDPTransport : public ITransport
{
public:
  // Server Socket
  UDPTransport(const std::string& server_name_in, uint16_t server_port_in, ITransport::TransportDelegate& delegate_in);

  // Client Socket
  UDPTransport(uint16_t sfuPort_in, ITransport::TransportDelegate& delegate_in);

  virtual ~UDPTransport();

  TransportStatus status() override;

  TransportContextId connect() override;

  void close() override;

  Error enqueue(TransportContextId& tcid, std::vector<uint8_t>& bytes) override;

  std::optional<std::vector<uint8_t>> dequeue(TransportContextId& tcid)override;

private:
    TransportContextId connect_client();
    TransportContextId connect_server();

    struct Remote {
        socklen_t addr_len;
        struct sockaddr_storage addr;
    };

  int fd; // UDP socket
  bool m_isServer;
  std::string server_name;
  uint16_t server_port;
  struct sockaddr_storage server_sockaddr;
  socklen_t server_sockaddr_len;
  ITransport::TransportDelegate& delegate;
  TransportContextId transport_context_id {0};
  std::map<TransportContextId, Remote> remote_contexts = {};
};

} // namespace neo_media
