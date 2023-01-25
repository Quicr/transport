#include <iostream>
#include <thread>
#include <sstream>
#include <iomanip>

#include <transport/transport.h>

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

public:
	Delegate() {
		msgcount = 0;
	}

	void setServerTransport(std::shared_ptr<ITransport> server) {
		this->server = server;
	}

	void on_connection_status(const TransportContextId &context_id, const TransportStatus status) {
		std::cout << "Connection state change context: " << context_id << ", " << int(status) << std::endl;
	}

	void on_new_connection(const TransportContextId &context_id, const TransportRemote &remote) {
		std::cout << "New connection cid: " << context_id
				<< " from " << remote.host_or_ip << ":" << remote.port << std::endl;
	}

	void on_recv_notify(const TransportContextId &context_id, const MediaStreamId &mStreamId) {
		std::cout << "cid: " << context_id << " msid: " << mStreamId
						  << " : Data available" << std::endl;

		while (true) {
			auto data = server->dequeue(context_id, mStreamId);

			if (data.has_value()) {
				msgcount++;
				std::cout << "  RecvMsg (" << msgcount << ") : " << to_hex(data.value()) << std::endl;
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
	Delegate d;
	TransportRemote serverIp = TransportRemote{"127.0.0.1", 1234, TransportProtocol::UDP};
	LogHandler logger;
	auto server = ITransport::make_server_transport(serverIp, d, logger);
    uint64_t tcid = server->start();
    uint64_t msid = 0; /* unused */

		d.setServerTransport(server);

    while (1)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
