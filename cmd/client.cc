#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <transport/transport.h>

#include "object.h"

using namespace qtransport;

bool done = false;
using bytes = std::vector<uint8_t>;

struct Delegate : public ITransport::TransportDelegate
{
  private:
    std::shared_ptr<ITransport> client;
    TransportConnId conn_id;
    std::shared_ptr<spdlog::logger> logger;
    Object _rx_object{ logger };

  public:
    Delegate()
      : logger(spdlog::stderr_color_mt("CDLG"))
    {
        conn_id = 0;
    }

    void stop() { client.reset(); }

    void setClientTransport(std::shared_ptr<ITransport> client) { this->client = client; }

    TransportConnId getContextId() const { return conn_id; }

    void OnConnectionStatus(const TransportConnId& conn_id, const TransportStatus status)
    {
        SPDLOG_LOGGER_INFO(logger, "Connection state change conn_id: {0}, {1}", conn_id, int(status));
    }

    void OnNewConnection(const TransportConnId&, const TransportRemote&) {}

    void OnRecvStream(const TransportConnId& conn_id,
                      uint64_t stream_id,
                      std::optional<DataContextId> data_ctx_id,
                      [[maybe_unused]] const bool is_bidir)
    {
        auto stream_buf = client->GetStreamBuffer(conn_id, stream_id);

        while (true) {
            if (stream_buf->Available(4)) {
                auto msg_len_b = stream_buf->Front(4);
                auto* msg_len = reinterpret_cast<uint32_t*>(msg_len_b.data());

                if (stream_buf->Available(*msg_len)) {
                    auto obj = stream_buf->Front(*msg_len);
                    stream_buf->Pop(*msg_len);

                    _rx_object.process(conn_id, data_ctx_id, obj);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    void OnRecvDgram(const TransportConnId& conn_id, std::optional<DataContextId> data_ctx_id)
    {
        for (int i = 0; i < 50; i++) {
            auto data = client->Dequeue(conn_id, data_ctx_id);

            if (data) {
                _rx_object.process(conn_id, data_ctx_id, *data);
            }
        }
    }

    void OnNewDataContext(const TransportConnId&, const DataContextId&) {}
};

auto logger = spdlog::stderr_color_mt("CLIENT");
Delegate d;

int
main()
{
    char* envVar;

    logger->set_level(spdlog::level::debug);

    TransportRemote server = TransportRemote{ "127.0.0.1", 1234, TransportProtocol::kQuic };

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

    auto client = ITransport::MakeClientTransport(server, tconfig, d, logger);

    SPDLOG_LOGGER_INFO(logger, "bidir is {0}", (bidir ? "True" : "False"));
    SPDLOG_LOGGER_INFO(logger, "client use_count: {0}", client.use_count());

    d.setClientTransport(client);
    SPDLOG_LOGGER_INFO(logger, "after set client transport client use_count: {0}", client.use_count());

    auto metrics_conn_samples = std::make_shared<SafeQueue<MetricsConnSample>>(10);
    auto metrics_data_samples = std::make_shared<SafeQueue<MetricsDataSample>>(10);

    auto conn_id = client->Start(metrics_conn_samples, metrics_data_samples);

    while (client->Status() != TransportStatus::kReady) {
        SPDLOG_LOGGER_INFO(logger, "Waiting for client to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    bool use_reliable = true;
    DataContextId data_ctx_id = client->CreateDataContext(conn_id, use_reliable, 1, bidir);

    int period_count = 0;

    ITransport::EnqueueFlags encode_flags{
        .use_reliable = use_reliable, .new_stream = true, .clear_tx_queue = true, .use_reset = true
    };

    auto tx_object = Object(logger);

    while (client->Status() != TransportStatus::kShutdown && client->Status() != TransportStatus::kDisconnected) {
        period_count++;
        for (int i = 0; i < 10; i++) {
            auto obj = tx_object.encode();

            std::vector<MethodTraceItem> trace;
            const auto start_time =
              std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());
            trace.push_back({ "client:publish", start_time });

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

            client->Enqueue(conn_id, data_ctx_id, std::move(obj), std::move(trace), 1, 350, 0, encode_flags);
        }

        // Increase delay if using UDP, need to pace more
        if (server.proto == TransportProtocol::kUdp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    client->DeleteDataContext(conn_id, data_ctx_id);

    SPDLOG_LOGGER_INFO(logger, "Done with transport, closing");
    client.reset();
    d.stop();
    SPDLOG_LOGGER_INFO(logger, "Program done");
}
