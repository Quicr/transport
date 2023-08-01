#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "cmdLogger.h"
#include <transport/transport.h>

using namespace qtransport;

struct Delegate : public ITransport::TransportDelegate {
private:
  std::shared_ptr<ITransport> server;
  uint64_t msgcount;
  cmdLogger &logger;

public:
  Delegate(cmdLogger &logger) : logger(logger) { msgcount = 0; }

  void stop() {
      server.reset();
  }

  void setServerTransport(std::shared_ptr<ITransport> server) {
    this->server = server;
  }

  void on_connection_status(const TransportContextId &context_id,
                            const TransportStatus status) {
    std::stringstream s_log;
    s_log << "Connection state change context: " << context_id << ", "
          << int(status);
    logger.log(LogLevel::info, s_log.str());
  }

  void on_new_connection(const TransportContextId &context_id,
                         const TransportRemote &remote) {
    std::stringstream s_log;
    s_log << "New connection cid: " << context_id << " from "
          << remote.host_or_ip << ":" << remote.port;
    logger.log(LogLevel::info, s_log.str());
  }

  void on_recv_notify(const TransportContextId &context_id,
                      const StreamId &streamId) {
    std::stringstream s_log;

    static uint32_t prev_msg_num = 0;

    while (true) {
      auto data = server->dequeue(context_id, streamId);

      if (data.has_value()) {
        msgcount++;

        uint32_t *msg_num = (uint32_t *)data.value().data();

        if (prev_msg_num && (*msg_num - prev_msg_num) > 1) {
            s_log.str(std::string());
            s_log << "cid: " << context_id << " sid: " << streamId << "  length: " << data->size() << "  RecvMsg ("
                  << msgcount << ")"
                  << "  msg_num: " << *msg_num
                  << "  prev_num: " << prev_msg_num << "(" << *msg_num - prev_msg_num << ")";
            logger.log(LogLevel::info, s_log.str());
        }

        prev_msg_num = *msg_num;

        server->enqueue(context_id, streamId, std::move(data.value()));
      } else {
        break;
      }
    }
  }

  void on_new_stream(const TransportContextId & /* context_id */,
                     const StreamId & /* streamId */) {}
};

int main() {
  char *envVar;
  cmdLogger logger;
  Delegate d(logger);
  TransportRemote serverIp =
      TransportRemote{"127.0.0.1", 1234, TransportProtocol::QUIC};
  TransportConfig tconfig{.tls_cert_filename = "./server-cert.pem",
                          .tls_key_filename = "./server-key.pem",
                          .time_queue_max_duration = 300,
                          .time_queue_bucket_interval = 1,
                          .debug = true};


  if ( (envVar = getenv("RELAY_PORT")))
    serverIp.port = atoi(envVar);

  auto server = ITransport::make_server_transport(serverIp, tconfig, d, logger);
  server->start();

  d.setServerTransport(server);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }

  server.reset();
  d.stop();

  return 0;
}

