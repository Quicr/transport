#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <transport/transport.h>
#include <cantina/logger.h>

using namespace qtransport;

struct Delegate : public ITransport::TransportDelegate {
private:
  std::shared_ptr<ITransport> server;
  cantina::LoggerPointer logger;

  uint64_t msgcount {0};
  uint64_t prev_msgcount {0};
  uint32_t prev_msg_num {0};
  DataContextId out_data_ctx {0};

public:
  Delegate(const cantina::LoggerPointer& logger)
    : logger(std::make_shared<cantina::Logger>("ECHO", logger))
  {
  }

  void stop() {
      server.reset();
  }

  void setServerTransport(std::shared_ptr<ITransport> server) {
    this->server = server;
  }

  void on_connection_status(const TransportConnId &conn_id,
                            const TransportStatus status) {
    logger->info << "Connection state change conn_id: " << conn_id << ", "
                 << int(status) << std::flush;
  }

  void on_new_connection(const TransportConnId &conn_id,
                         const TransportRemote &remote) {
    logger->info << "New connection conn_id: " << conn_id << " from "
                 << remote.host_or_ip << ":" << remote.port << std::flush;

    out_data_ctx = this->server->createDataContext(conn_id, true, 10);
  }

  void on_recv_notify(const TransportConnId &conn_id,
                      const DataContextId &data_ctx_id) {

    while (true) {
      auto data = server->dequeue(conn_id, data_ctx_id);

      if (data.has_value()) {
        msgcount++;

        uint32_t *msg_num = (uint32_t *)data->data();

        if (prev_msg_num && (*msg_num - prev_msg_num) > 1) {
            logger->info << "conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id << "  length: " << data->size()
                         << "  RecvMsg (" << msgcount << ")"
                         << "  msg_num: " << *msg_num << "  prev_num: " << prev_msg_num << "("
                         << *msg_num - prev_msg_num << ")" << std::flush;
        }

        prev_msg_num = *msg_num;

        server->enqueue(conn_id, out_data_ctx, std::move(data.value()));

      } else {
        if (msgcount % 2000 == 0 && prev_msgcount != msgcount) {
            prev_msgcount = msgcount;
            logger->info << "conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id << "  msgcount: " << msgcount << std::flush;
        }

        break;
      }
    }
  }

  void on_new_data_context(const TransportConnId& conn_id, const DataContextId& data_ctx_id)
  {
    logger->info << "Callback for new data context conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id
                 << std::flush;
  }
};

int main() {
  char *envVar;
  cantina::LoggerPointer logger = std::make_shared<cantina::Logger>("ECHO");
  Delegate d(logger);
  TransportRemote serverIp =
      TransportRemote{"127.0.0.1", 1234, TransportProtocol::QUIC};
  TransportConfig tconfig{.tls_cert_filename = "./server-cert.pem",
                          .tls_key_filename = "./server-key.pem",
                          .time_queue_max_duration = 1000,
                          .time_queue_bucket_interval = 1,
                          .debug = true};


  if ( (envVar = getenv("RELAY_PORT")))
    serverIp.port = atoi(envVar);

  auto server = ITransport::make_server_transport(serverIp, tconfig, d, logger);
  server->start();

  d.setServerTransport(server);

  while (server->status() != TransportStatus::Shutdown) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  server.reset();
  d.stop();

  return 0;
}

