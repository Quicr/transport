#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <transport/transport.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "object.h"

using namespace qtransport;

struct Delegate : public ITransport::TransportDelegate {
   private:
    std::shared_ptr<ITransport> server;
    std::shared_ptr<spdlog::logger> logger;

    Object _object{logger};

    DataContextId out_data_ctx{0};

   public:
    Delegate() : logger(spdlog::stderr_color_mt("SERVER")) {}

    void stop() { server.reset(); }

    void setServerTransport(std::shared_ptr<ITransport> server) { this->server = server; }

    void OnConnectionStatus(const TransportConnId& conn_id, const TransportStatus status) {
        SPDLOG_LOGGER_INFO(logger, "Connection state change conn_id: {0}, {1}", conn_id, int(status));
    }

    void OnNewConnection(const TransportConnId& conn_id, const TransportRemote& remote) {
        SPDLOG_LOGGER_INFO(logger, "New connection conn_id: {0} from {1}:{2}", conn_id, remote.host_or_ip, remote.port);

        out_data_ctx = this->server->CreateDataContext(conn_id, true, 10);
    }

    void OnRecvStream(const TransportConnId& conn_id, uint64_t stream_id, std::optional<DataContextId> data_ctx_id,
                      [[maybe_unused]] const bool is_bidir) {
        auto stream_buf = server->GetStreamBuffer(conn_id, stream_id);

        while (true) {
            if (stream_buf->Available(4)) {
                auto len_b = stream_buf->Front(4);
                if (!len_b.size()) return;

                auto* msg_len = reinterpret_cast<uint32_t*>(len_b.data());

                if (stream_buf->Available(*msg_len)) {
                    auto obj = stream_buf->Front(*msg_len);
                    stream_buf->Pop(*msg_len);

                    _object.process(conn_id, data_ctx_id, obj);

                    server->Enqueue(conn_id, out_data_ctx, std::move(obj), {MethodTraceItem{}}, 2, 500, 0,
                                    {true, false, false, false});
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    void OnRecvDgram(const TransportConnId& conn_id, std::optional<DataContextId> data_ctx_id) {
        for (int i = 0; i < 150; i++) {
            auto data = server->Dequeue(conn_id, data_ctx_id);

            if (data) {
                _object.process(conn_id, data_ctx_id, *data);

                server->Enqueue(conn_id, out_data_ctx, std::move(*data));
            }
        }
    }

    void OnNewDataContext(const TransportConnId& conn_id, const DataContextId& data_ctx_id) {
        SPDLOG_LOGGER_INFO(logger, "Callback for new data context conn_id: {0} data_ctx_id: {1}", conn_id, data_ctx_id);
    }
};

int main() {
    char* envVar;
    auto logger = spdlog::stderr_color_mt("ECHO");
    logger->set_level(spdlog::level::debug);

    Delegate d;
    TransportRemote serverIp = TransportRemote{"127.0.0.1", 1234, TransportProtocol::kQuic};
    TransportConfig tconfig{.tls_cert_filename = "./server-cert.pem",
                            .tls_key_filename = "./server-key.pem",
                            .time_queue_max_duration = 1000,
                            .time_queue_bucket_interval = 1,
                            .debug = true};

    if ((envVar = getenv("RELAY_PORT"))) serverIp.port = atoi(envVar);

    auto server = ITransport::MakeServerTransport(serverIp, tconfig, d, logger);

    auto metrics_conn_samples = std::make_shared<SafeQueue<MetricsConnSample>>(10);
    auto metrics_data_samples = std::make_shared<SafeQueue<MetricsDataSample>>(10);
    server->Start(metrics_conn_samples, metrics_data_samples);

    d.setServerTransport(server);

    while (server->Status() != TransportStatus::kShutdown) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    server.reset();
    d.stop();

    return 0;
}
