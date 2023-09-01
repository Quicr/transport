#include <transport/transport.h>
#include <transport_udp.h>
#if defined(ENABLE_QUIC)
  #include <transport_picoquic.h>
#endif

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
#if defined(ENABLE_QUIC)
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 false,
                                                 logger);
#endif
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
#if defined(ENABLE_QUIC)
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 true, logger);
#endif
    default:
      logger.log(LogLevel::error, "Protocol not implemented");

      throw std::runtime_error(
        "make_server_transport: Protocol not implemented");
      break;
  }

  return NULL;
}

} // namespace qtransport