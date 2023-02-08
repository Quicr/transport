#include <transport/transport.h>
#include <transport_udp.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote& server,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{

  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, delegate, false, logger);

    case TransportProtocol::QUIC:
      logger.log(LogLevel::error, "QUIC not implemented yet");
      throw std::runtime_error("make_client_transport: QUIC not implemented");
      break;

    default:
      logger.log(LogLevel::error, "Protocol not implemented");
      throw std::runtime_error(
        "make_client_transport: Protocol not implemented");
      break;
  }

  return NULL;
}

std::shared_ptr<ITransport>
ITransport::make_server_transport(const TransportRemote& server,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, delegate, true, logger);

    case TransportProtocol::QUIC:
      logger.log(LogLevel::error, "QUIC not implemented yet");
      throw std::runtime_error("make_server_transport: QUIC not implemented");
      break;
    default:
      logger.log(LogLevel::error, "Protocol not implemented");

      throw std::runtime_error(
        "make_server_transport: Protocol not implemented");
      break;
  }

  return NULL;
}

} // namespace qtransport