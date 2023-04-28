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

    while (true) {
      auto data = client->dequeue(context_id, streamId);

      if (data.has_value()) {
        msgcount++;
        uint32_t *msg_num = (uint32_t *)data.value().data();

        s_log.str(std::string());
        s_log << "cid: " << context_id << " sid: " << streamId
              << "  length: " << data->size()
              << "  RecvMsg (" << msgcount << ")"
              << "  msg_num: " << *msg_num;
        logger.log(LogLevel::info, s_log.str());
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
TransportRemote server =
    TransportRemote{"relay.us-west-2.quicr.ctgpoc.com", 33439, TransportProtocol::QUIC};
    //TransportRemote{"127.0.0.1", 33439, TransportProtocol::QUIC};
TransportConfig tconfig { .tls_cert_filename = NULL, .tls_key_filename = NULL };
auto client = ITransport::make_client_transport(server, tconfig, d, logger);

int main() {
  d.setClientTransport(client);

  auto tcid = client->start();
  uint8_t data_buf[1200] {0};

  while (client->status() != TransportStatus::Ready) {
    logger.log(LogLevel::info, "Waiting for client to be ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  StreamId stream_id = client->createStream(tcid, true);

  std::stringstream s_log;

/*
  for (int i =0; i < 500; i++) {
    auto data = bytes(data_buf, data_buf + sizeof(data_buf));

    s_log.str("");

    s_log << "sending STREAM length: " << data.size();
    logger.log(LogLevel::info, s_log.str());
    //client->enqueue(tcid, stream_id, std::move(data)) ;

    while (true) {
      if (client->enqueue(tcid,
                          stream_id,
                          std::move(data)) != TransportError::QueueFull) {
        break;
      }

      logger.log(LogLevel::info, "buffer full");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::this_thread::sleep_for(std::chrono::seconds(3));
*/

  uint32_t *msg_num = (uint32_t*)&data_buf;
  while (true) {
    for (int i =0; i < 25; i++) {
      (*msg_num)++;
      auto data = bytes(data_buf, data_buf + sizeof(data_buf));

      /*
      s_log.str("");

      s_log << "sending DGRAM, length: " << data.size();
      s_log << " msg_num: " << *msg_num;
      logger.log(LogLevel::info, s_log.str());
      */
      client->enqueue(tcid, server.proto == TransportProtocol::UDP ? 1 : 0,
                      std::move(data));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds (50));
  }


  client->closeStream(tcid, stream_id);

  client.reset();

}
