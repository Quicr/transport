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
                      const MediaStreamId &mStreamId) {
    std::stringstream s_log;

    while (true) {
      auto data = client->dequeue(context_id, mStreamId);

      if (data.has_value()) {
        msgcount++;
        s_log.str(std::string());
        s_log << "cid: " << context_id << " msid: " << mStreamId
              << "  RecvMsg (" << msgcount << ") : " << to_hex(data.value());
        logger.log(LogLevel::info, s_log.str());
      } else {
        break;
      }
    }
  }
  void on_new_media_stream(const TransportContextId & /* context_id */,
                           const MediaStreamId & /* mStreamId */) {}
};

cmdLogger logger;
Delegate d(logger);
TransportRemote server =
    TransportRemote{"127.0.0.1", 1234, TransportProtocol::UDP};

auto client = ITransport::make_client_transport(server, d, logger);
auto tcid = client->start();

int main() {
  d.setClientTransport(client);
  const uint8_t forty_bytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3,
                                 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7,
                                 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  // while(!transportManager.transport_ready()) {
  //	std::this_thread::sleep_for(std::chrono::seconds (2));
  // }
  logger.log(LogLevel::info, "Transport is ready");

  // Send forty_bytes packet 10 seconds with 50 ms apart
  while (true) {
    if ((tcid = d.getContextId()) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    } else
      break;
  }

  std::stringstream s_log;
  while (true) {
    auto data = bytes(forty_bytes, forty_bytes + sizeof(forty_bytes));

    s_log.str("");

    s_log << "sending: " << to_hex(data);
    logger.log(LogLevel::info, s_log.str());

    client->enqueue(tcid, 1, std::move(data));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}
