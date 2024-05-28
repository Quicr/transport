#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <cantina/logger.h>
#include <transport/transport.h>
#include "object.h"

using namespace qtransport;

struct Delegate : public ITransport::TransportDelegate
{
  private:
    std::shared_ptr<ITransport> server;
    cantina::LoggerPointer logger;

    Object _object { logger };

    uint64_t msgcount{ 0 };
    uint64_t prev_msgcount{ 0 };
    uint32_t prev_msg_num{ 0 };
    DataContextId out_data_ctx{ 0 };

  public:
    Delegate(const cantina::LoggerPointer& logger)
      : logger(std::make_shared<cantina::Logger>("SERVER", logger))
    {
    }

    void stop() { server.reset(); }

    void setServerTransport(std::shared_ptr<ITransport> server) { this->server = server; }

    void on_connection_status(const TransportConnId& conn_id, const TransportStatus status)
    {
        logger->info << "Connection state change conn_id: " << conn_id << ", " << int(status) << std::flush;
    }

    void on_new_connection(const TransportConnId& conn_id, const TransportRemote& remote)
    {
        logger->info << "New connection conn_id: " << conn_id << " from " << remote.host_or_ip << ":" << remote.port
                     << std::flush;

        out_data_ctx = this->server->createDataContext(conn_id, true, 10);
    }


    void on_recv_stream(const TransportConnId& conn_id,
                        uint64_t stream_id,
                        std::optional<DataContextId> data_ctx_id,
                        [[maybe_unused]] const bool is_bidir)
    {
        auto stream_buf = std::move(server->getStreamBuffer(conn_id, stream_id));

        while(true) {
            if (stream_buf->available(4)) {
                auto len_b = stream_buf->front(4);
                if (!len_b.size())
                    return;

                auto* msg_len = reinterpret_cast<uint32_t*>(len_b.data());

                if (stream_buf->available(*msg_len)) {
                    auto obj = stream_buf->front(*msg_len);
                    stream_buf->pop(*msg_len);

                    _object.process(conn_id, data_ctx_id, obj);

                    server->enqueue(conn_id, out_data_ctx, std::move(obj), { MethodTraceItem{} }, 2, 500, 0, { true, false, false, false });
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
        for (int i=0; i < 150; i++) {
            auto data = server->dequeue(conn_id, data_ctx_id);

            if (data) {
                _object.process(conn_id, data_ctx_id, *data);

                server->enqueue(conn_id, out_data_ctx, std::move(*data));
            }
        }
    }

    void on_new_data_context(const TransportConnId& conn_id, const DataContextId& data_ctx_id)
    {
        logger->info << "Callback for new data context conn_id: " << conn_id << " data_ctx_id: " << data_ctx_id
                     << std::flush;
    }
};

int
main()
{
    char* envVar;
    cantina::LoggerPointer logger = std::make_shared<cantina::Logger>("ECHO");
    logger->SetLogLevel("DEBUG");
    Delegate d(logger);
    TransportRemote serverIp = TransportRemote{ "127.0.0.1", 1234, TransportProtocol::QUIC };
    TransportConfig tconfig{ .tls_cert_filename = "./server-cert.pem",
                             .tls_key_filename = "./server-key.pem",
                             .time_queue_max_duration = 1000,
                             .time_queue_bucket_interval = 1,
                             .debug = true };

    if ((envVar = getenv("RELAY_PORT")))
        serverIp.port = atoi(envVar);

    auto server = ITransport::make_server_transport(serverIp, tconfig, d, logger);
    auto metrics_conn_samples = std::make_shared<safe_queue<MetricsConnSample>>(10);
    auto metrics_data_samples = std::make_shared<safe_queue<MetricsDataSample>>(10);
    server->start(metrics_conn_samples, metrics_data_samples);

    d.setServerTransport(server);

    while (server->status() != TransportStatus::Shutdown) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    server.reset();
    d.stop();

    return 0;
}
