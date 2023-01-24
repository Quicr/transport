#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>

#include <transport/transport.h>

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

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

Delegate d;
TransportRemote server = TransportRemote{"127.0.0.1", 1234, TransportProtocol::UDP};
LogHandler logger;
auto client = ITransport::make_client_transport(server, d, logger);
auto tcid = client->start();
void read_loop() {
	std::cout << "Client read loop init\n";
    uint64_t tcid = 0;
    uint64_t msid = 0;
	while(!done) {
		auto data = client->dequeue(tcid, msid);
		if(data.has_value()) {
			std::cout << "Received: " << to_hex(data.value()) << "\n";
		}
	}
}

int main() {
	const uint8_t forty_bytes[] = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9};
	//while(!transportManager.transport_ready()) {
	//	std::this_thread::sleep_for(std::chrono::seconds (2));
	//}
	std::cout << "Transport is ready" << std::endl;
	std::thread reader (read_loop);

	// Send forty_bytes packet 10 seconds with 50 ms apart

  while(true)
	{
  	auto data = bytes(forty_bytes, forty_bytes+ sizeof(forty_bytes));
		std::cout<< "sending: " << to_hex(data) << std::endl;
		client->enqueue(tcid,0, std::move(data));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}
