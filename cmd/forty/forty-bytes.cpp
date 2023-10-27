
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <atomic>
#include <queue>
#include <signal.h>


#include <transport/transport.h>
#include <cantina/logger.h>


using namespace qtransport;
using namespace std::chrono_literals;

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

struct Delegate : public ITransport::TransportDelegate
{
private:
    std::shared_ptr<ITransport> client;
    uint64_t msgcount;
    TransportContextId tcid;
    cantina::LoggerPointer logger;

public:
    Delegate(const cantina::LoggerPointer& logger)
            : logger(std::make_shared<cantina::Logger>("CMD", logger))
    {
        msgcount = 0;
        tcid = 0;
    }

    void stop() {
        client.reset();
    }

    void setClientTransport(std::shared_ptr<ITransport> client) { this->client = client; }

    TransportContextId getContextId() { return tcid; }

    void on_connection_status(const TransportContextId& context_id, const TransportStatus status)
    {
        tcid = context_id;
        logger->info << "Connection state change context: " << context_id << ", " << int(status) << std::flush;
    }
    void on_new_connection(const TransportContextId& /* context_id */, const TransportRemote& /* remote */) {}

    void on_recv_notify(const TransportContextId& context_id, const StreamId& streamId)
    {
        static uint32_t prev_msg_num = 0;
        std::cout << "client-del:Received Some Data on Stream: " << streamId << "\n";
        while (true) {
            auto data = client->dequeue(context_id, streamId);
            if (data.has_value()) {
                msgcount++;
                if(data) {
                    std::cout << "Received: " << to_hex(data.value()) << "\n";
                }
            } else {
                break;
            }
        }
    }

    void on_new_stream(const TransportContextId& /* context_id */, const StreamId& /* streamId */) {}
};

cantina::LoggerPointer logger = std::make_shared<cantina::Logger>();
Delegate delegate(logger);


std::atomic<bool> done{};




void
signal_callback_handler(int /*signum*/)
{
    done = true;
}

void
setup_signal_handlers()
{
    signal(SIGINT, signal_callback_handler);
}

int
main(int argc, char* argv[])
{
    const uint8_t forty_bytes[] = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9};
    char* envVar;

    setup_signal_handlers();

    TransportRemote server = TransportRemote{ "127.0.0.1", 1234, TransportProtocol::QUIC };
    TransportConfig tconfig{ .tls_cert_filename = NULL,
            .tls_key_filename = NULL,
            .time_queue_init_queue_size = 1000,
            .time_queue_max_duration = 1000,
            .time_queue_bucket_interval = 1,
            .debug = true };

    if ((envVar = getenv("RELAY_HOST"))) {
        server.host_or_ip = envVar;
    }

    if ((envVar = getenv("RELAY_PORT"))) {
        server.port = atoi(envVar);
    }

    auto client = ITransport::make_client_transport(server, tconfig, delegate, logger);
    logger->info << "client use_count: " << client.use_count() << std::flush;
    delegate.setClientTransport(client);
    logger->info << "after set client transport client use_count: " << client.use_count() << std::flush;

    auto tcid = client->start();
    uint8_t data_buf[4200]{ 0 };

    while (client->status() != TransportStatus::Ready) {
        logger->Log("Waiting for client to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    StreamId stream_id = client->createStream(tcid, true);

    uint32_t* msg_num = (uint32_t*)&data_buf;

    while (!done) {
        auto data = bytes(forty_bytes, forty_bytes+ sizeof(forty_bytes));
        std::cout<< "sending: " << to_hex(data) << std::endl;
        client->enqueue(tcid, server.proto == TransportProtocol::UDP ? 1 : stream_id, std::move(data));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client->closeStream(tcid, stream_id);

    logger->Log("Done with transport, closing");
    client.reset();
    delegate.stop();
    logger->Log("Program done");

    return 0;
}