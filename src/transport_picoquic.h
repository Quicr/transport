
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

namespace qtransport {

    constexpr int PQ_LOOP_MAX_DELAY_US = 500;           /// The max microseconds that pq_loop will be ran again
    constexpr int PQ_REST_WAIT_MIN_PRIORITY = 4;        /// Minimum priority value to consider for RESET and WAIT
    constexpr int PQ_CC_LOW_CWIN = 4000;                /// Bytes less than this value are considered a low/congested CWIN

class PicoQuicTransport : public ITransport
{
  public:
    const char* QUICR_ALPN = "quicr-v1";

    /**
     * Data header is transmitted for every object transmitted (stream and datagram)
     */
    struct DataHeader {
        uint8_t  hdr_length {0};                            /// Length of header in bytes, including hdr_length byte
        uintV_t  remote_data_ctx_id_V {0};                  /// Receiver data context ID encoded
        uint64_t remote_data_ctx_id {0};                    /// Receiver data context ID

        uintV_t  length_V {0};                              /// Length of stream object value being transmitted encoded
        uint64_t length {0};                                /// Length of stream object value being transmitted

        /**
         * @brief Size of the data header
         * @return
         */
        uint8_t size() {
            hdr_length = 1 + uintV_size(remote_data_ctx_id_V[0]) + uintV_size(length_V[0]);
            return hdr_length;
        }

        /**
         * @brief Load data header from bytes
         *
         * @param hdr_bytes         Bytes as written from network to load
         *
         * @return true if loaded, false if there was an error
         */
        bool load(std::vector<uint8_t> &&hdr_bytes) {
            if (hdr_bytes.empty()) return false;

            hdr_length = hdr_bytes.front();

            auto it = hdr_bytes.begin() + 1;

            const auto data_ctx_id_len = uintV_size(*it);
            remote_data_ctx_id_V.assign(it, it + data_ctx_id_len);
            it += data_ctx_id_len;

            if (it == hdr_bytes.end()) return false;

            remote_data_ctx_id = to_uint64(remote_data_ctx_id_V);

            const auto length_len = uintV_size(*it);
            length_V.assign(it, it + length_len);

            length = to_uint64(length_V);

            return true;
        }

        /**
         * @brief data for header. Data can be written to network
         *
         * @return
         */
        std::vector<uint8_t> data() {
            std::vector<uint8_t> net_data;

            net_data.push_back(size());
            net_data.insert(net_data.end(), remote_data_ctx_id_V.begin(), remote_data_ctx_id_V.end());
            net_data.insert(net_data.end(), length_V.begin(), length_V.end());

            return net_data;
        }
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

        DataHeader data_header {};                           /// Data header to use for start of stream or for every datagram

        uint64_t current_stream_id {0};                      /// Current active stream if the value is >= 4

        uint8_t priority {0};

        uint64_t in_data_cb_skip_count {0};                  /// Number of times callback was skipped due to size

        std::unique_ptr<safe_queue<ConnData>> rx_data;        /// Pending objects received from the network
        std::unique_ptr<priority_queue<ConnData>> tx_data;    /// Pending objects to be written to the network

        uint8_t* stream_tx_object {nullptr};                 /// Current object that is being sent as a byte stream
        size_t stream_tx_object_size {0};                    /// Size of the tx object
        size_t stream_tx_object_offset{0};                   /// Pointer offset to next byte to send

        struct StreamRxBuffer
        {
            uint8_t* object { nullptr };           /// Current object that is being received via byte stream
            uint32_t object_size { 0 };            /// Receive object data size to append up to before sending to app
            size_t object_offset { 0 };            /// Pointer offset to next byte to append

            ~StreamRxBuffer() {
                if (object != nullptr) {
                    delete[] object;
                }
            }

            void reset_buffer() {
                if(object != nullptr) {
                    delete[] object;
                }

                object = nullptr;
                object_size = 0;
                object_offset = 0;
            }
        };
        std::map<uint64_t, StreamRxBuffer> stream_rx_buffer;        /// Map of stream receive buffers, key is stream_id

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
         * Reset the RX object buffer
         */
        void reset_rx_object(uint64_t stream_id) {

            auto it = stream_rx_buffer.find(stream_id);
            if (it != stream_rx_buffer.end()) {
                it->second.reset_buffer();
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
        uint64_t last_stream_id {0};                          /// last stream Id - Zero means not set/no stream yet. >=4 is the starting stream value

        bool mark_dgram_ready { false };                     /// Instructs datagram to be marked ready/active

        DataContextId next_data_ctx_id {1};                   /// Next data context ID; zero is reserved for default context


        std::unique_ptr<priority_queue<ConnData>> dgram_tx_data;  /// Datagram pending objects to be written to the network

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
                           std::vector<qtransport::MethodTraceItem> &&trace,
                           const uint8_t priority,
                           const uint32_t ttl_ms,
                           const uint32_t delay_ms,
                           const EnqueueFlags flags) override;

    std::optional<std::vector<uint8_t>> dequeue(
      const TransportConnId& conn_id,
      const DataContextId& data_ctx_id) override;

    void setRemoteDataCtxId(const TransportConnId conn_id,
                            const DataContextId data_ctx_id,
                            const DataContextId remote_data_ctx_id) override;

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
    void close_stream(const ConnectionContext& conn_ctx, DataContext *data_ctx, const bool send_reset);


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
