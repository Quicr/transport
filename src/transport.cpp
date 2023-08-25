#include <transport/transport.h>
#include <transport_udp.h>
#include <transport_picoquic.h>

namespace qtransport {
/*=======================================================================*/
// JSON Conversion Functions
/*=======================================================================*/

void to_json(json& j, const TransportRemote& config)
{
    j = {
        { "host_or_ip", config.host_or_ip },
        { "port", config.port },
        { "proto", config.proto },
    };
}

void from_json(const json& j, TransportRemote& config)
{
    j.at("host_or_ip").get_to(config.host_or_ip);
    j.at("port").get_to(config.port);
    j.at("proto").get_to(config.proto);
}

void to_json(json& j, const TransportConfig& config)
{
    j = {
        { "tls_cert_filename", config.tls_cert_filename },
        { "tls_key_filename", config.tls_key_filename },
        { "time_queue_init_queue_size", config.time_queue_init_queue_size },
        { "time_queue_max_duration", config.time_queue_max_duration },
        { "time_queue_bucket_interval", config.time_queue_bucket_interval },
        { "time_queue_rx_ttl", config.time_queue_rx_ttl },
        { "debug", config.debug },
    };
}

void from_json(const json& j, TransportConfig& config)
{
    j.at("tls_cert_filename").get_to(config.tls_cert_filename);
    j.at("tls_key_filename").get_to(config.tls_key_filename);

    if (j.contains("time_queue_init_queue_size"))
        j.at("time_queue_init_queue_size").get_to(config.time_queue_init_queue_size);
    if (j.contains("time_queue_max_duration"))
        j.at("time_queue_max_duration").get_to(config.time_queue_max_duration);
    if (j.contains("time_queue_bucket_interval"))
        j.at("time_queue_bucket_interval").get_to(config.time_queue_bucket_interval);
    if (j.contains("time_queue_rx_ttl"))
        j.at("time_queue_rx_ttl").get_to(config.time_queue_rx_ttl);
    if (j.contains("debug"))
        j.at("debug").get_to(config.debug);
}

/*=======================================================================*/
// Creation Functions
/*=======================================================================*/

std::shared_ptr<ITransport>
ITransport::make_client_transport(const json& server_config,
                                  const json& transport_config,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{
  TransportProtocol protocol = server_config["protocol"];
  switch (protocol) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server_config, delegate, false, logger);

    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server_config,
                                                 transport_config,
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
ITransport::make_server_transport(const json& server_config,
                                  const json& transport_config,
                                  TransportDelegate& delegate,
                                  LogHandler& logger)
{
  TransportProtocol protocol = server_config["protocol"];
  switch (protocol) {
    case TransportProtocol::UDP:
      return std::make_shared<UDPTransport>(server_config, delegate, true, logger);

    case TransportProtocol::QUIC:
      return std::make_shared<PicoQuicTransport>(server_config,
                                                 transport_config,
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