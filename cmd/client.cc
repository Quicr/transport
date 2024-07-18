#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <transport/transport.h>

#include <cantina/logger.h>
#include "object.h"

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

struct Delegate : public ITransport::TransportDelegate
{
  private:
    std::shared_ptr<ITransport> client;
    TransportConnId conn_id;
    cantina::LoggerPointer logger;
    Object _rx_object { logger };

  public:
    Delegate(const cantina::LoggerPointer& logger)
      : logger(std::make_shared<cantina::Logger>("CLIENT", logger))
    {
        conn_id = 0;
    }

    void stop() {
        client.reset();
    }

    void setClientTransport(std::shared_ptr<ITransport> client) { this->client = client; }

    TransportConnId getContextId() const { return conn_id; }

    void on_connection_status(const TransportConnId& conn_id, const TransportStatus status)
    {
        logger->info << "Connection state change conn_id: " << conn_id << ", " << int(status) << std::flush;
    }

    void on_new_connection(const TransportConnId& , const TransportRemote&) {}

    void on_recv_stream(const TransportConnId& conn_id,
                        uint64_t stream_id,
                        std::optional<DataContextId> data_ctx_id,
                        [[maybe_unused]] const bool is_bidir)
    {
        auto stream_buf = client->getStreamBuffer(conn_id, stream_id);

        while (true) {
            if (stream_buf->available(4)) {
                auto msg_len_b = stream_buf->front(4);
                auto* msg_len = reinterpret_cast<uint32_t*>(msg_len_b.data());

                if (stream_buf->available(*msg_len)) {
                    auto obj = stream_buf->front(*msg_len);
                    stream_buf->pop(*msg_len);

                    _rx_object.process(conn_id, data_ctx_id, obj);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    void on_recv_dgram(const TransportConnId& conn_id,
                       std::optional<DataContextId> data_ctx_id)
    {
        for (int i=0; i < 50; i++) {
            auto data = client->dequeue(conn_id, data_ctx_id);

            if (data) {
                _rx_object.process(conn_id, data_ctx_id, *data);
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

    logger->SetLogLevel("DEBUG");

    TransportRemote server = TransportRemote{ "127.0.0.1", 1234, TransportProtocol::QUIC };

    TransportConfig tconfig{ .tls_cert_filename = "",
                             .tls_key_filename = "",
                             .time_queue_init_queue_size = 1000,
                             .time_queue_max_duration = 1000,
                             .time_queue_bucket_interval = 1,
                             .debug = true };

    if ((envVar = getenv("RELAY_HOST")))
        server.host_or_ip = envVar;

    if ((envVar = getenv("RELAY_PORT")))
        server.port = atoi(envVar);

    bool bidir = false;
    if (getenv("RELAY_UNIDIR"))
        bidir = false;

    auto client = ITransport::make_client_transport(server, tconfig, d, logger);

    logger->info << "bidir is " << (bidir ? "True" : "False") << std::flush;
    logger->info << "client use_count: " << client.use_count() << std::flush;

    d.setClientTransport(client);
    logger->info << "after set client transport client use_count: " << client.use_count() << std::flush;

    auto metrics_conn_samples = std::make_shared<safe_queue<MetricsConnSample>>(10);
    auto metrics_data_samples = std::make_shared<safe_queue<MetricsDataSample>>(10);

    auto conn_id = client->start(metrics_conn_samples, metrics_data_samples);

    while (client->status() != TransportStatus::Ready) {
        logger->Log("Waiting for client to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    bool use_reliable = true;
    DataContextId data_ctx_id = client->createDataContext(conn_id, use_reliable, 1, bidir);

    int period_count = 0;

    ITransport::EnqueueFlags encode_flags { .use_reliable = use_reliable, .new_stream = true, .clear_tx_queue = true, .use_reset = true};

    auto tx_object = Object(logger);

    while (client->status() != TransportStatus::Shutdown && client->status() != TransportStatus::Disconnected) {
        period_count++;
        for (int i = 0; i < 10; i++) {
            auto obj = tx_object.encode();

            std::vector<MethodTraceItem> trace;
            const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());
            trace.push_back({"client:publish", start_time});

            if (encode_flags.use_reliable) {
                if (period_count > 2000) {
                    period_count = 0;
                    encode_flags.new_stream = true;
                    encode_flags.clear_tx_queue = true;
                    encode_flags.use_reset = true;
                } else {
                    encode_flags.new_stream = false;
                    encode_flags.clear_tx_queue = false;
                    encode_flags.use_reset = false;
                }
            }

            client->enqueue(conn_id,
                            data_ctx_id,
                            std::move(obj),
                            std::move(trace),
                            1,
                            350,
                            0,
                            encode_flags);
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
