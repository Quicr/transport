#include <transport/transport.h>
#include <transport_udp.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote &server,
                                  TransportDelegate &delegate,
																	LogHandler &logger) {

  switch (server.proto) {
  case TransportProtocol::UDP:
    return std::make_shared<UDPTransport>(server, delegate, false, logger);

  case TransportProtocol::QUIC:
    assert(0); // TODO  - add handling of protocol not yet implemented
    break;
  default:
    assert(0); // TODO  - add handling of invalid protocol
    break;
  }

  return NULL;
}

std::shared_ptr<ITransport>
ITransport::make_server_transport(const TransportRemote &server,
                                  TransportDelegate &delegate,
                                  LogHandler &logger) {
  switch (server.proto) {
  case TransportProtocol::UDP:
    return std::make_shared<UDPTransport>(server, delegate, true, logger);

  case TransportProtocol::QUIC:
    assert(0); // TODO  - add handling of protocol not yet implemented
    break;
  default:
    assert(0); // TODO  - add handling of invalid protocol
    break;
  }

  return NULL;
}

} // namespace qtransport