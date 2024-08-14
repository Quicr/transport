#include <transport/transport.h>
#include <transport_udp.h>
#if not defined(PLATFORM_ESP)
  #include <transport_picoquic.h>
#endif

#include <spdlog/spdlog.h>

namespace qtransport {

std::shared_ptr<ITransport>
ITransport::make_client_transport(const TransportRemote& server,
                                  const TransportConfig& tcfg,
                                  TransportDelegate& delegate)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, tcfg, delegate, false);
#ifndef PLATFORM_ESP
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 false);
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
                                  TransportDelegate& delegate)
{
  switch (server.proto) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server, tcfg, delegate, true);

#ifndef PLATFORM_ESP
    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server,
                                                 tcfg,
                                                 delegate,
                                                 true);
#endif
    default:
      throw std::runtime_error("make_server_transport: Protocol not implemented");
      break;
  }

  return nullptr;
}

} // namespace qtransport
