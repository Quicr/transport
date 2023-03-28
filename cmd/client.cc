#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <transport/transport.h>

#include "cmdLogger.h"

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

static std::string to_hex(const std::vector<uint8_t> &data) {
  std::stringstream hex(std::ios_base::out);
  hex.flags(std::ios::hex);
  for (const auto &byte : data) {
    hex << std::setw(2) << std::setfill('0') << int(byte);
  }
  return hex.str();
}

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
        s_log.str(std::string());
        s_log << "cid: " << context_id << " msid: " << streamId
              << "  RecvMsg (" << msgcount << ") : " << to_hex(data.value());
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
    TransportRemote{"127.0.0.1", 1234, TransportProtocol::QUIC};

TransportConfig tconfig { .tls_key_filename = NULL, .tls_cert_filename = NULL };
auto client = ITransport::make_client_transport(server, tconfig, d, logger);

int main() {
  d.setClientTransport(client);
  const uint8_t forty_bytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3,
                                 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7,
                                 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

  auto tcid = client->start();

  while (client->status() != TransportStatus::Ready) {
    logger.log(LogLevel::info, "Waiting for client to be ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  StreamId stream_id = client->createStream(tcid, true);

  std::stringstream s_log;


  for (int i =0; i < 500; i++) {
    auto data = bytes(forty_bytes, forty_bytes + sizeof(forty_bytes));

    s_log.str("");

    s_log << "sending STREAM: " << to_hex(data);
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

  for (int i =0; i < 1000; i++) {
    auto data = bytes(forty_bytes, forty_bytes + sizeof(forty_bytes));

    s_log.str("");

    s_log << "sending DGRAM: " << to_hex(data);
    logger.log(LogLevel::info, s_log.str());

    client->enqueue(tcid, 0, std::move(data));
    //std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::this_thread::sleep_for(std::chrono::seconds(20));

  client->closeStream(tcid, stream_id);

  client.reset();

}
