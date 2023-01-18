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

};

int main()
{
    Delegate d;
    auto server = ITransport::make_server_transport(1234, d);
    uint64_t tcid =  server->connect();
    while (1)
    {

        auto data = server->dequeue(tcid);
        if (!data.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::clog << "Received " << to_hex(data.value()) << "\n";
        server->enqueue(tcid, data.value());
    }

    return 0;
}
