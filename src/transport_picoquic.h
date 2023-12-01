
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <picoquic.h>
#include <picoquic_config.h>
#include <picoquic_packet_loop.h>
#include <transport/transport.h>

#include "transport/priority_queue.h"
#include "transport/safe_queue.h"
#include "transport/time_queue.h"

namespace qtransport {

class PicoQuicTransport : public ITransport
{
  public:
    const char* QUICR_ALPN = "quicr-v1";

    struct ConnectionMetrics {
        uint64_t time_checks {0};
        uint64_t total_retransmits {0};

        auto operator<=>(const ConnectionMetrics&) const = default;
    };

    struct DataContextMetrics {
        uint64_t dgram_ack {0};
        uint64_t dgram_spurious {0};
        uint64_t dgram_prepare_send {0};
        uint64_t dgram_lost {0};
        uint64_t dgram_received {0};
        uint64_t dgram_sent {0};

        uint64_t enqueued_objs {0};

        uint64_t stream_prepare_send {0};
        uint64_t stream_rx_callbacks {0};

        uint64_t tx_delayed_callback {0};                   /// Count of times transmit callbacks were delayed
        uint64_t prev_tx_delayed_callback {0};              /// Previous transmit delayed callback value, set each interval
        uint64_t stream_objects_sent {0};
        uint64_t stream_bytes_sent {0};
        uint64_t stream_bytes_recv {0};
        uint64_t stream_objects_recv {0};

        auto operator<=>(const DataContextMetrics&) const = default;

    };


    using bytes_t = std::vector<uint8_t>;
    using timeQueue = time_queue<bytes_t, std::chrono::milliseconds>;
    using DataContextId = uint64_t;

    /**
     * Data context information
     *      Data context is intended to be a container for metrics and other state that is related to a flow of
     *      data that may use datagram or one or more stream QUIC frames
     */
    struct DataContext {
        bool is_default_context { false };                   /// Indicates if the data context is the default context
        bool is_bidir { false };                             /// Indicates if the stream is bidir (true) or unidir (false)

        DataContextId data_ctx_id {0};                       /// The ID of this context
        TransportConnId conn_id {0};                         /// The connection ID this context is under

        uint64_t current_stream_id {0};                      /// Current active stream if the value is >= 4

        uint8_t priority {0};

        uint64_t in_data_cb_skip_count {0};                  /// Number of times callback was skipped due to size
        std::unique_ptr<safe_queue<bytes_t>> rx_data;        /// Pending objects received from the network
        std::unique_ptr<priority_queue<bytes_t>> tx_data;    /// Pending objects to be written to the network

        uint8_t* stream_tx_object {nullptr};                 /// Current object that is being sent as a byte stream
        size_t stream_tx_object_size {0};                    /// Size of the tx object
        size_t stream_tx_object_offset{0};                   /// Pointer offset to next byte to send

        uint8_t* stream_rx_object {nullptr};                 /// Current object that is being received via byte stream
        uint16_t stream_rx_object_hdr_size { 0 };             /// Size of header read in (should be 4 right now)
        uint32_t stream_rx_object_size {0};                  /// Receive object data size to append up to before sending to app
        size_t stream_rx_object_offset{0};                   /// Pointer offset to next byte to append

        // The last time TX callback was run
        std::chrono::time_point<std::chrono::steady_clock> last_tx_callback_time { std::chrono::steady_clock::now() };

        DataContextMetrics metrics;

        DataContext() = default;
        DataContext(DataContext&&) = default;

        DataContext& operator=(const DataContext&) = delete;
        DataContext& operator=(DataContext&&) = delete;

        ~DataContext() {
            /*
             * Free legacy pointers if not null
             */
            if (stream_tx_object != nullptr) {
                delete[] stream_tx_object;
                stream_tx_object = nullptr;
            }

            if (stream_rx_object != nullptr) {
                delete[] stream_rx_object;
                stream_rx_object = nullptr;
            }
        }

        /**
         * Reset the RX object buffer
         */
        void reset_rx_object() {
            if (stream_rx_object != nullptr) {
                delete[] stream_rx_object;
            }

            stream_rx_object = nullptr;
            stream_rx_object_hdr_size = 0;
            stream_rx_object_size = 0;
            stream_rx_object_offset = 0;
        }

        /**
         * Reset the TX object buffer
         */
        void reset_tx_object() {
            if (stream_tx_object != nullptr) {
                delete [] stream_tx_object;
            }

            stream_tx_object = nullptr;
            stream_tx_object_offset = 0;
            stream_tx_object_size = 0;
        }
    };

    /**
     * Connection context information
     */
    struct ConnectionContext {
        TransportConnId conn_id {0};                          /// This connection ID
        picoquic_cnx_t * pq_cnx = nullptr;                    /// Picoquic connection/path context
        uint64_t last_stream_id {0};                          /// last stream Id - Zero means not set/no stream yet. >=4 is the starting stream value

        DataContextId next_data_ctx_id {1};                   /// Next data context ID; zero is reserved for default context

        /**
         * Default data context is used for datagrams and received unidirectional streams only. Transmit
         *  unidirectional MUST be it's own context.
         */
        DataContext default_data_context;


        /**
         * Active data contexts are for transmit unidirectional data flows as well as for bi-directional (received) flows
         */
        std::map<qtransport::DataContextId, DataContext> active_data_contexts;

        char peer_addr_text[45] {0};
        uint16_t peer_port {0};
        sockaddr_storage peer_addr;

        // States
        bool is_congested { false };

        // Metrics
        ConnectionMetrics metrics;

        ConnectionContext() {
            default_data_context.is_default_context = true;
        }

        ConnectionContext(picoquic_cnx_t *cnx) : ConnectionContext() {
            pq_cnx = cnx;
        }
    };

    /*
     * pq event loop member vars
     */
    uint64_t pq_loop_prev_time = 0;

    /*
     * Exceptions
     */
    struct Exception : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct InvalidConfigException : public Exception
    {
        using Exception::Exception;
    };

    struct PicoQuicException : public Exception
    {
        using Exception::Exception;
    };

    static void PicoQuicLogging(const char *message, void *argp)
    {
      auto instance = reinterpret_cast<PicoQuicTransport *>(argp);
      if (!instance->stop && instance->picoquic_logger)
      {
        instance->picoquic_logger->Log(message);
      }
    }

  public:
    PicoQuicTransport(const TransportRemote& server,
                      const TransportConfig& tcfg,
                      TransportDelegate& delegate,
                      bool _is_server_mode,
                      const cantina::LoggerPointer& logger);

    virtual ~PicoQuicTransport();

    TransportStatus status() const override;
    TransportConnId start() override;
    void close(const TransportConnId& conn_id) override;

    virtual bool getPeerAddrInfo(const TransportConnId& conn_id,
                                 sockaddr_storage* addr) override;

    DataContextId createDataContext(const TransportConnId conn_id,
                                    bool use_reliable_transport,
                                    uint8_t priority, bool bidir) override;

    void deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) override;

    TransportError enqueue(const TransportConnId& conn_id,
                           const DataContextId& data_ctx_id,
                           std::vector<uint8_t>&& bytes,
                           const uint8_t priority,
                           const uint32_t ttl_ms,
                           const bool new_stream,
                           const bool buffer_reset) override;

    std::optional<std::vector<uint8_t>> dequeue(
      const TransportConnId& conn_id,
      const DataContextId& data_ctx_id) override;

    /*
     * Internal public methods
     */
    ConnectionContext* getConnContext(const TransportConnId& conn_id);
    void setStatus(TransportStatus status);

    /**
     * @brief Create bidirectional data context for received new stream
     *
     * @details Create a bidir data context for received bidir stream. This is only called
     *  for received bidirectional streams.
     *
     * @param conn_id           Connection context ID for the stream
     * @param stream_id         Stream ID of the new received stream
     *
     * @returns DataContext pointer to the created context, nullptr if invalid connection id
     */
    DataContext* createDataContextBiDirRecv(TransportConnId conn_id, uint64_t stream_id);

    ConnectionContext& createConnContext(picoquic_cnx_t * pq_cnx);

    DataContext& getDefaultDataContext(TransportConnId conn_id);

    void send_next_datagram(DataContext* data_ctx, uint8_t* bytes_ctx, size_t max_len);
    void send_stream_bytes(DataContext* data_ctx, uint8_t* bytes_ctx, size_t max_len);

    void on_connection_status(const TransportConnId conn_id,
                              const TransportStatus status);

    void on_new_connection(const TransportConnId conn_id);
    void on_recv_datagram(DataContext* data_ctx,
                          uint8_t* bytes, size_t length);
    void on_recv_stream_bytes(DataContext* data_ctx,
                             uint8_t* bytes, size_t length);

    void check_conns_for_congestion();

    /**
     * @brief Function run the queue functions within the picoquic thread via the pq_loop_cb
     *
     * @details Function runs the picoquic specific functions in the same thread that runs the
     *      the event loop. This allows picoquic to be thread safe.  All picoquic functions that
     *      other threads want to call should queue those in `picoquic_runner_queue`.
     */
    void pq_runner();

    /*
   * Internal Public Variables
     */
    cantina::LoggerPointer logger;
    cantina::LoggerPointer picoquic_logger;
    bool _is_server_mode;
    bool _is_unidirectional{ false };
    bool debug {false};


  private:
    TransportConnId createClient();
    void shutdown();

    void server();
    void client(const TransportConnId conn_id);
    void cbNotifier();

    void check_callback_delta(DataContext* data_ctx, bool tx=true);

    /**
     * @brief Create a new stream
     *
     * @param conn_ctx      Connection context to create stream under
     * @param data_ctx      Data context in connection context to create streams
     * @param bidir         Stream should be created as bi-directional (true) or unidirectional (false)
     */
    void create_stream(ConnectionContext&conn_ctx, DataContext &data_ctx, const bool bidir=false);

    /**
     * @brief App initiated Close stream
     * @details App initiated close stream. When the app deletes a context or wants to switch streams to a new stream
     *   this function is used to close out the current stream. A FIN will be sent.
     *
     * @param conn_ctx      Connection context for the stream
     * @param data_ctx      Data context for the stream
     * @param send_reset    Indicates if the stream should be closed by RESET
     * @param send_fin      Indicates if the stream should be closed by FIN (reset will take priority)
     */
    void close_stream(const ConnectionContext&conn_ctx, DataContext &data_ctx, const bool send_reset, const bool send_fin);


    /*
     * Variables
     */
    picoquic_quic_config_t config;
    picoquic_quic_t* quic_ctx;
    picoquic_tp_t local_tp_options;
    safe_queue<std::function<void()>> cbNotifyQueue;

    safe_queue<std::function<void()>> picoquic_runner_queue;         /// Threads queue functions that picoquic will call via the pq_loop_cb call

    std::atomic<bool> stop;
    std::mutex _state_mutex;                                        /// Used for stream/context/state updates
    std::atomic<TransportStatus> transportStatus;
    std::thread picoQuicThread;
    std::thread cbNotifyThread;

    TransportRemote serverInfo;
    TransportDelegate& delegate;
    TransportConfig tconfig;

    std::map<TransportConnId, ConnectionContext> conn_context;
    std::shared_ptr<tick_service> _tick_service;
};

} // namespace qtransport
