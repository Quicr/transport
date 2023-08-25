#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <transport/transport.h>

#include <cantina/logger.h>

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

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

        while (true) {
            auto data = client->dequeue(context_id, streamId);

            if (data.has_value()) {
                msgcount++;

                uint32_t* msg_num = (uint32_t*)data.value().data();

                if (prev_msg_num && (*msg_num - prev_msg_num) > 1) {
                    logger->info << "cid: " << context_id << " sid: " << streamId << "  length: " << data->size()
                                 << "  RecvMsg (" << msgcount << ")"
                                 << "  msg_num: " << *msg_num << "  prev_num: " << prev_msg_num << "("
                                 << *msg_num - prev_msg_num << ")" << std::flush;
                }

                prev_msg_num = *msg_num;

            } else {
                break;
            }
        }
    }
    void on_new_stream(const TransportContextId& /* context_id */, const StreamId& /* streamId */) {}
};

cantina::LoggerPointer logger = std::make_shared<cantina::Logger>();
Delegate d(logger);

int
main()
{
    char* envVar;

    TransportRemote server = TransportRemote{ "127.0.0.1", 1234, TransportProtocol::QUIC };

    TransportConfig tconfig{ .tls_cert_filename = NULL,
                             .tls_key_filename = NULL,
                             .time_queue_init_queue_size = 1000,
                             .time_queue_max_duration = 1000,
                             .time_queue_bucket_interval = 1,
                             .debug = true };

    if ((envVar = getenv("RELAY_HOST")))
        server.host_or_ip = envVar;

    if ((envVar = getenv("RELAY_PORT")))
        server.port = atoi(envVar);

    auto client = ITransport::make_client_transport(server, tconfig, d, logger);

    logger->info << "client use_count: " << client.use_count() << std::flush;

    d.setClientTransport(client);
    logger->info << "after set client transport client use_count: " << client.use_count() << std::flush;

    auto tcid = client->start();
    uint8_t data_buf[4200]{ 0 };

    while (client->status() != TransportStatus::Ready) {
        logger->Log("Waiting for client to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    StreamId stream_id = client->createStream(tcid, true);

    uint32_t* msg_num = (uint32_t*)&data_buf;

    while (true) {
        for (int i = 0; i < 10; i++) {
            (*msg_num)++;
            auto data = bytes(data_buf, data_buf + sizeof(data_buf));

            client->enqueue(tcid, server.proto == TransportProtocol::UDP ? 1 : stream_id, std::move(data));
        }

        // Increase delay if using UDP, need to pace more
        if (server.proto == TransportProtocol::UDP) {
            std::this_thread::sleep_for(std::chrono::milliseconds (10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    client->closeStream(tcid, stream_id);

    logger->Log("Done with transport, closing");
    client.reset();
    d.stop();
    logger->Log("Program done");
}
