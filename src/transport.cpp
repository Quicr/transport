#include <transport/transport.h>
#include <transport_udp.h>
#include <transport_picoquic.h>
#include <cantina/logger.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  const cantina::LoggerPointer& logger)
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
      logger->error << "Protocol not implemented" << std::flush;
      throw std::runtime_error(
        "make_client_transport: Protocol not implemented");
      break;
  }

  return nullptr;
}

std::shared_ptr<ITransport>
ITransport::make_server_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  const cantina::LoggerPointer& logger)
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
      logger->error << "Protocol not implemented" << std::flush;

      throw std::runtime_error(
        "make_server_transport: Protocol not implemented");
      break;
  }

  return nullptr;
}

} // namespace qtransport
