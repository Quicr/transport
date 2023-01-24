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
	void on_connection_status(const TransportContextId &context_id, const TransportStatus status) {}
	void on_new_connection(const TransportContextId &context_id, const TransportRemote &remote) {}
	void on_recv_notify(const TransportContextId &context_id) {}
	void onNewMediaStream(const TransportContextId &context_id, const MediaStreamId &mStreamId) {}
};

int main()
{
	Delegate d;
	TransportRemote serverIp = TransportRemote{"127.0.0.1", 1234, TransportProtocol::UDP};
	LogHandler logger;
	auto server = ITransport::make_server_transport(serverIp, d, logger);
    uint64_t tcid = server->start();
    uint64_t msid = 0; /* unused */
    while (1)
    {

        auto data = server->dequeue(tcid, msid);
        if (!data.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::clog << "Received " << to_hex(data.value()) << "\n";
        server->enqueue(tcid, msid, std::move(data.value()));
    }

    return 0;
}
