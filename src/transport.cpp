#include <transport/transport.h>
#include <transport_udp.h>
#if not defined(PLATFORM_ESP)
  #include <transport_picoquic.h>
#endif

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  std::shared_ptr<spdlog::logger> logger)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, tcfg, delegate, false, std::move(logger));
#ifndef PLATFORM_ESP
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 false,
                                                 std::move(logger));
#endif
    default:
      throw std::runtime_error("make_client_transport: Protocol not implemented");
      break;
  }

  return nullptr;
}

std::shared_ptr<ITransport>
ITransport::make_server_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate,
                                  std::shared_ptr<spdlog::logger> logger)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, tcfg, delegate, true, std::move(logger));

#ifndef PLATFORM_ESP
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 true,
                                                 std::move(logger));
#endif
    default:
      throw std::runtime_error("make_server_transport: Protocol not implemented");
      break;
  }

  return nullptr;
}

} // namespace qtransport
