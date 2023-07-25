#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <transport/transport.h>

#include "cmdLogger.h"

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

struct Delegate : public ITransport::TransportDelegate {
private:
  std::shared_ptr<ITransport> client;
  uint64_t msgcount;
  TransportContextId tcid;
  cmdLogger &logger;

public:
  Delegate(cmdLogger &logger) : logger(logger) {
    msgcount = 0;
    tcid = 0;
  }

  void setClientTransport(std::shared_ptr<ITransport> client) {
    this->client = client;
  }

  TransportContextId getContextId() { return tcid; }

  void on_connection_status(const TransportContextId &context_id,
                            const TransportStatus status) {
    tcid = context_id;
    std::stringstream s_log;
    s_log << "Connection state change context: " << context_id << ", "
          << int(status);
    logger.log(LogLevel::info, s_log.str());
  }
  void on_new_connection(const TransportContextId & /* context_id */,
                         const TransportRemote & /* remote */) {}

  void on_recv_notify(const TransportContextId &context_id,
                      const StreamId &streamId) {
    std::stringstream s_log;

    static uint32_t prev_msg_num = 0;

    while (true) {
      auto data = client->dequeue(context_id, streamId);

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

      } else {
        break;
      }
    }
  }
  void on_new_stream(const TransportContextId & /* context_id */,
                     const StreamId & /* streamId */) {}
};

cmdLogger logger;
Delegate d(logger);

int main() {
  char *envVar;

  TransportRemote server =
      TransportRemote{"127.0.0.1", 1234, TransportProtocol::QUIC};

  TransportConfig tconfig{.tls_cert_filename = NULL,
                           .tls_key_filename = NULL,
                           .time_queue_init_queue_size = 1000,
                           .time_queue_max_duration = 1000,
                           .time_queue_bucket_interval = 1,
                           .debug = true};

  if ( (envVar = getenv("RELAY_HOST")))
    server.host_or_ip = envVar;

  if ( (envVar = getenv("RELAY_PORT")))
    server.port = atoi(envVar);

  auto client = ITransport::make_client_transport(server, tconfig, d, logger);

  d.setClientTransport(client);

  auto tcid = client->start();
  uint8_t data_buf[1200]{0};

  while (client->status() != TransportStatus::Ready) {
    logger.log(LogLevel::info, "Waiting for client to be ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  StreamId stream_id = client->createStream(tcid, false);

  std::stringstream s_log;

  uint32_t *msg_num = (uint32_t *)&data_buf;

  while (true) {
    for (int i = 0; i < 10; i++) {
      (*msg_num)++;
      auto data = bytes(data_buf, data_buf + sizeof(data_buf));

      client->enqueue(tcid, server.proto == TransportProtocol::UDP ? 1 : stream_id,
                        std::move(data));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  client->closeStream(tcid, stream_id);

  client.reset();
}

