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
    TransportConnId conn_id;
    cantina::LoggerPointer logger;

  public:
    Delegate(const cantina::LoggerPointer& logger)
      : logger(std::make_shared<cantina::Logger>("CMD", logger))
    {
        msgcount = 0;
        conn_id = 0;
    }

    void stop() {
        client.reset();
    }

    void setClientTransport(std::shared_ptr<ITransport> client) { this->client = client; }

    TransportConnId getContextId() { return conn_id; }

    void on_connection_status(const TransportConnId& conn_id, const TransportStatus status)
    {
        logger->info << "Connection state change conn_id: " << conn_id << ", " << int(status) << std::flush;
    }

    void on_new_connection(const TransportConnId& , const TransportRemote&) {}

    void on_recv_notify(const TransportConnId& conn_id, const DataContextId& data_ctx_id, [[maybe_unused]] const bool is_bidir)
    {
        static uint32_t prev_msg_num = 0;
        static uint32_t prev_msgcount = 0;

        while (true) {
            auto data = client->dequeue(conn_id, data_ctx_id);

            if (data.has_value()) {
                msgcount++;

                if (msgcount % 2000 == 0 && prev_msgcount != msgcount) {
                    logger->info << "conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id << "  msgcount: " << msgcount << std::flush;
                }

                uint32_t* msg_num = (uint32_t*)data.value().data();

                if (prev_msg_num && (*msg_num - prev_msg_num) > 1) {
                    logger->info << "conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id << "  length: " << data->size()
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

    void on_new_data_context(const TransportConnId&, const DataContextId&) {}
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

    bool bidir = true;
    if (getenv("RELAY_UNIDIR"))
        bidir = false;

    auto client = ITransport::make_client_transport(server, tconfig, d, logger);

    logger->info << "bidir is " << (bidir ? "True" : "False") << std::flush;
    logger->info << "client use_count: " << client.use_count() << std::flush;

    d.setClientTransport(client);
    logger->info << "after set client transport client use_count: " << client.use_count() << std::flush;

    auto conn_id = client->start();
    uint8_t data_buf[4200]{ 0 };

    while (client->status() != TransportStatus::Ready) {
        logger->Log("Waiting for client to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    DataContextId data_ctx_id = client->createDataContext(conn_id, true, 1, bidir);

    uint32_t* msg_num = (uint32_t*)&data_buf;
    int period_count = 0;

    ITransport::EncodeFlags encode_flags { .new_stream = true, .clear_tx_queue = true, .use_reset = true};

    while (client->status() != TransportStatus::Shutdown && client->status() != TransportStatus::Disconnected) {
        period_count++;
        for (int i = 0; i < 10; i++) {
            (*msg_num)++;
            auto data = bytes(data_buf, data_buf + sizeof(data_buf));

            if (period_count > 2000) {
                period_count = 0;
                client->enqueue(conn_id,
                                server.proto == TransportProtocol::UDP ? 1 : data_ctx_id,
                                std::move(data),
                                1,
                                350,
                                encode_flags);
            }
            else {
                client->enqueue(conn_id, server.proto == TransportProtocol::UDP ? 1 : data_ctx_id, std::move(data));
            }

        }

        // Increase delay if using UDP, need to pace more
        if (server.proto == TransportProtocol::UDP) {
            std::this_thread::sleep_for(std::chrono::milliseconds (10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }


    }

    client->deleteDataContext(conn_id, data_ctx_id);

    logger->Log("Done with transport, closing");
    client.reset();
    d.stop();
    logger->Log("Program done");
}
