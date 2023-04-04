#include <transport/transport.h>
#include <transport_udp.h>
#include <transport_picoquic.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{

  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, delegate, false, logger);

    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 false,
                                                 logger);

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
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, delegate, true, logger);

    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 true, logger);

    default:
      logger.log(LogLevel::error, "Protocol not implemented");

      throw std::runtime_error(
        "make_server_transport: Protocol not implemented");
      break;
  }

  return NULL;
}

} // namespace qtransport