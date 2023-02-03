#include <iostream>
#include <thread>
#include <sstream>
#include <iomanip>

#include <transport/transport.h>
#include "cmdLogger.h"

using namespace qtransport;

static std::string
to_hex(const std::vector<uint8_t>& data)
{
    std::stringstream hex(std::ios_base::out);
    hex.flags(std::ios::hex);
    for (const auto& byte : data) {
        hex << std::setw(2) << std::setfill('0') << int(byte);
    }
    return hex.str();
}

struct Delegate : public ITransport::TransportDelegate {
private:
	std::shared_ptr<ITransport> server;
	uint64_t msgcount;
	cmdLogger &logger;

public:
	Delegate(cmdLogger &logger) : logger(logger) {
		msgcount = 0;
	}

	void setServerTransport(std::shared_ptr<ITransport> server) {
		this->server = server;
	}

	void on_connection_status(const TransportContextId &context_id, const TransportStatus status) {
		std::stringstream s_log;
		s_log << "Connection state change context: " << context_id << ", " << int(status);
		logger.log(LogLevel::info, s_log.str());
	}

	void on_new_connection(const TransportContextId &context_id, const TransportRemote &remote) {
		std::stringstream s_log;
		s_log << "New connection cid: " << context_id
				<< " from " << remote.host_or_ip << ":" << remote.port;
		logger.log(LogLevel::info, s_log.str());
	}

	void on_recv_notify(const TransportContextId &context_id, const MediaStreamId &mStreamId) {
		std::stringstream s_log;

		while (true) {
			auto data = server->dequeue(context_id, mStreamId);

			if (data.has_value()) {
				msgcount++;

				s_log.str(std::string());
				s_log << "cid: " << context_id << " msid: " << mStreamId
					 << "  RecvMsg (" << msgcount << ") : " << to_hex(data.value());
				logger.log(LogLevel::info, s_log.str());

				server->enqueue(context_id, mStreamId, std::move(data.value()));
			} else {
				break;
			}
		}
	}

	void on_new_media_stream(const TransportContextId &context_id, const MediaStreamId &mStreamId) {}
};

int main()
{
	cmdLogger logger;
	Delegate d(logger);
	TransportRemote serverIp = TransportRemote{"127.0.0.1", 1234, TransportProtocol::UDP};
	auto server = ITransport::make_server_transport(serverIp, d, logger);
    uint64_t tcid = server->start();
    uint64_t msid = 0; /* unused */

		d.setServerTransport(server);

    while (1)
    {
      std::this_thread::sleep_for(std::chrono::seconds (5));
    }

    return 0;
}
