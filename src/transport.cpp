#include <transport/transport.h>
#include <transport_udp.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const std::string &server, uint16_t port,
                                  ITransport::TransportDelegate &delegate,
                                  LogHandler &logger) {
  return std::make_shared<UDPTransport>(server, port, delegate);
}

std::shared_ptr<ITransport>
ITransport::make_server_transport(uint16_t port,
                                  ITransport::TransportDelegate &delegate,
                                  LogHandler &logger) {
  return std::make_shared<UDPTransport>(port, delegate);
}

} // namespace qtransport