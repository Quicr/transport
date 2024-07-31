
#pragma once

#include <atomic>
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
#include "transport/transport_metrics.h"
#include "transport/stream_buffer.h"

namespace qtransport {

    constexpr int PQ_LOOP_MAX_DELAY_US = 500;           /// The max microseconds that pq_loop will be ran again
    constexpr int PQ_REST_WAIT_MIN_PRIORITY = 4;        /// Minimum priority value to consider for RESET and WAIT
    constexpr int PQ_CC_LOW_CWIN = 4000;                /// Bytes less than this value are considered a low/congested CWIN

class PicoQuicTransport : public ITransport
{
  public:
    const char* QUICR_ALPN = "moq-00";

    using bytes_t = std::vector<uint8_t>;
    using timeQueue = TimeQueue<bytes_t, std::chrono::milliseconds>;
    using DataContextId = uint64_t;

    /**
     * Data context information
     *      Data context is intended to be a container for metrics and other state that is related to a flow of
     *      data that may use datagram or one or more stream QUIC frames
     */
    struct DataContext {
        bool is_bidir { false };                             /// Indicates if the stream is bidir (true) or unidir (false)
        bool mark_stream_active { false };                   /// Instructs the stream to be marked active

        bool uses_reset_wait { false };                      /// Indicates if data context can/uses reset wait strategy
        bool tx_reset_wait_discard { false };                /// Instructs TX objects to be discarded on POP instead

        DataContextId data_ctx_id {0};                       /// The ID of this context
        TransportConnId conn_id {0};                         /// The connection ID this context is under

        enum class StreamAction : uint8_t {                  /// Stream action that should be done by send/receive processing
            NO_ACTION=0,
            REPLACE_STREAM_USE_RESET,
            REPLACE_STREAM_USE_FIN,
        } stream_action {StreamAction::NO_ACTION};

        std::optional<uint64_t> current_stream_id;           /// Current active stream if the value is >= 4

        uint8_t priority {0};

        uint64_t in_data_cb_skip_count {0};                  /// Number of times callback was skipped due to size

        std::unique_ptr<PriorityQueue<ConnData>> tx_data;    /// Pending objects to be written to the network

        uint8_t* stream_tx_object {nullptr};                 /// Current object that is being sent as a byte stream
        size_t stream_tx_object_size {0};                    /// Size of the tx object
        size_t stream_tx_object_offset{0};                   /// Pointer offset to next byte to send

        // The last ticks when TX callback was run
        uint64_t last_tx_tick { 0 };

        QuicDataContextMetrics metrics;

        DataContext() = default;
        DataContext(DataContext&&) = default;

        DataContext& operator=(const DataContext&) = delete;
        DataContext& operator=(DataContext&&) = delete;

        ~DataContext() {
            // Free the TX object
            if (stream_tx_object != nullptr) {
                delete[] stream_tx_object;
                stream_tx_object = nullptr;
            }
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
        uint64_t last_stream_id {0};                          /// last stream Id

        bool mark_dgram_ready { false };                     /// Instructs datagram to be marked ready/active

        DataContextId next_data_ctx_id {1};                   /// Next data context ID; zero is reserved for default context


        std::unique_ptr<PriorityQueue<ConnData>> dgram_tx_data;  /// Datagram pending objects to be written to the network
        SafeQueue<bytes_t> dgram_rx_data;                        /// Buffered datagrams received from the network

        /**
         * Active stream buffers for received unidirectional streams
         */
         struct RxStreamBuffer {
             std::shared_ptr<StreamBuffer<uint8_t>> buf;
             bool closed { false };                                          /// Indicates if stream is active or in closed state
             bool checked_once { false };                                    /// True if closed and checked once to close

             RxStreamBuffer() {
                 buf = std::make_shared<StreamBuffer<uint8_t>>();
             }
         };
        std::map<uint64_t, RxStreamBuffer> rx_stream_buffer;        /// Map of stream receive buffers, key is stream_id

        /**
         * Active data contexts (streams bidir/unidir and datagram)
         */
        std::map<qtransport::DataContextId, DataContext> active_data_contexts;

        char peer_addr_text[45] {0};
        uint16_t peer_port {0};
        sockaddr_storage peer_addr;

        // States
        bool is_congested { false };
        uint16_t not_congested_gauge { 0 };                  /// Interval gauge count of consecutive not congested checks

        // Metrics
        QuicConnectionMetrics metrics;

        ConnectionContext() {
        }

        ConnectionContext(picoquic_cnx_t *cnx) : ConnectionContext() {
            pq_cnx = cnx;
        }
    };

    /*
     * pq event loop member vars
     */
    uint64_t pq_loop_prev_time = 0;
    uint64_t pq_loop_metrics_prev_time = 0;

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
      if (!instance->_stop && instance->picoquic_logger)
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
    TransportConnId start(std::shared_ptr<SafeQueue<MetricsConnSample>> metrics_conn_samples,
                          std::shared_ptr<SafeQueue<MetricsDataSample>> metrics_data_samples) override;
    void close(const TransportConnId& conn_id, uint64_t app_reason_code=0) override;

    virtual bool getPeerAddrInfo(const TransportConnId& conn_id,
                                 sockaddr_storage* addr) override;

    DataContextId createDataContext(const TransportConnId conn_id,
                                    bool use_reliable_transport,
                                    uint8_t priority, bool bidir) override;

    void deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) override;
    void delete_data_context_internal(TransportConnId conn_id, DataContextId data_ctx_id);

    TransportError enqueue(const TransportConnId& conn_id,
                           const DataContextId& data_ctx_id,
                           std::vector<uint8_t>&& bytes,
                           std::vector<qtransport::MethodTraceItem> &&trace,
                           const uint8_t priority,
                           const uint32_t ttl_ms,
                           const uint32_t delay_ms,
                           const EnqueueFlags flags) override;

    std::optional<std::vector<uint8_t>> dequeue(TransportConnId conn_id,
                                                std::optional<DataContextId> data_ctx_id) override;

    std::shared_ptr<StreamBuffer<uint8_t>> getStreamBuffer(TransportConnId conn_id, uint64_t stream_id) override;

    void setRemoteDataCtxId(const TransportConnId conn_id,
                            const DataContextId data_ctx_id,
                            const DataContextId remote_data_ctx_id) override;

    void setStreamIdDataCtxId(const TransportConnId conn_id, DataContextId data_ctx_id, uint64_t stream_id) override;
    void setDataCtxPriority(const TransportConnId conn_id, DataContextId data_ctx_id, uint8_t priority) override;

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

    void send_next_datagram(ConnectionContext* conn_ctx, uint8_t* bytes_ctx, size_t max_len);
    void send_stream_bytes(DataContext* data_ctx, uint8_t* bytes_ctx, size_t max_len);

    void on_connection_status(const TransportConnId conn_id,
                              const TransportStatus status);

    void on_new_connection(const TransportConnId conn_id);
    void on_recv_datagram(ConnectionContext* conn_ctx,
                          uint8_t* bytes, size_t length);
    void on_recv_stream_bytes(ConnectionContext *conn_ctx, DataContext* data_ctx, uint64_t stream_id,
                             uint8_t* bytes, size_t length);

    void check_conns_for_congestion();
    void emit_metrics();
    void remove_closed_streams();

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
     * @brief Mark a stream active
     * @details This method MUST only be called within the picoquic thread. Enqueue and other
     *      thread methods can call this via the pq_runner.
     */
    void mark_stream_active(const TransportConnId conn_id, const DataContextId data_ctx_id);

    /**
     * @brief Mark datagram ready
     * @details This method MUST only be called within the picoquic thread. Enqueue and other
     *      thread methods can call this via the pq_runner.
     */
    void mark_dgram_ready(const TransportConnId conn_id);

    /**
     * @brief Create a new stream
     *
     * @param conn_ctx      Connection context to create stream under
     * @param data_ctx      Data context in connection context to create streams
     */
    void create_stream(ConnectionContext&conn_ctx, DataContext *data_ctx);

    /**
     * @brief App initiated Close stream
     * @details App initiated close stream. When the app deletes a context or wants to switch streams to a new stream
     *   this function is used to close out the current stream. A FIN will be sent.
     *
     * @param conn_ctx      Connection context for the stream
     * @param data_ctx      Data context for the stream
     * @param send_reset    Indicates if the stream should be closed by RESET, otherwise FIN
     */
    void close_stream(ConnectionContext& conn_ctx, DataContext *data_ctx, const bool send_reset);


    /*
     * Variables
     */
    picoquic_quic_config_t _config;
    picoquic_quic_t* _quic_ctx;
    picoquic_tp_t _local_tp_options;
    SafeQueue<std::function<void()>> _cbNotifyQueue;

    SafeQueue<std::function<void()>> _picoquic_runner_queue;        /// Threads queue functions that picoquic will call via the pq_loop_cb call

    std::atomic<bool> _stop;
    std::mutex _state_mutex;                                        /// Used for stream/context/state updates
    std::atomic<TransportStatus> _transportStatus;
    std::thread _picoQuicThread;
    std::thread _cbNotifyThread;

    TransportRemote _serverInfo;
    TransportDelegate& _delegate;
    TransportConfig _tconfig;

    std::map<TransportConnId, ConnectionContext> _conn_context;
    std::shared_ptr<TickService> _tick_service;
};

} // namespace qtransport
