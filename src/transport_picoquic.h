
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

    struct Metrics {
        uint64_t dgram_ack {0};
        uint64_t dgram_spurious {0};
        uint64_t dgram_prepare_send {0};
        uint64_t dgram_sent {0};
        uint64_t stream_prepare_send {0};
        uint64_t stream_objects_sent {0};
        uint64_t stream_bytes_sent {0};
        uint64_t stream_bytes_recv {0};
        uint64_t stream_objects_recv {0};
        uint64_t stream_rx_callbacks {0};
        uint64_t send_null_bytes_ctx {0};
        uint64_t dgram_lost {0};
        uint64_t dgram_received {0};
        uint64_t time_checks {0};
        uint64_t enqueued_objs {0 };

        auto operator<=>(const Metrics&) const = default;
    } metrics;

    using bytes_t = std::vector<uint8_t>;
    using timeQueue = time_queue<bytes_t, std::chrono::milliseconds>;

    struct StreamContext {
        uint64_t stream_id;
        TransportContextId context_id;
        picoquic_cnx_t *cnx;
        sockaddr_storage peer_addr;
        char peer_addr_text[45];
        uint16_t peer_port;
        uint64_t in_data_cb_skip_count {0};                  /// Number of times callback was skipped due to size
        std::unique_ptr<safe_queue<bytes_t>> rx_data;        /// Pending objects received from the network
        std::unique_ptr<priority_queue<bytes_t>> tx_data;    /// Pending objects to be written to the network

        uint8_t* stream_tx_object {nullptr};                 /// Current object that is being sent as a byte stream
        size_t stream_tx_object_size {0};                    /// Size of the tx object
        size_t stream_tx_object_offset{0};                   /// Pointer offset to next byte to send

        uint8_t* stream_rx_object {nullptr};                 /// Current object that is being received via byte stream
        uint32_t stream_rx_object_size {0};                  /// Receive object data size to append up to before sending to app
        size_t stream_rx_object_offset{0};                   /// Pointer offset to next byte to append
    };


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

    TransportContextId start() override;

    void close(const TransportContextId& context_id) override;
    void closeStream(const TransportContextId& context_id,
                     StreamId stream_id) override;

    virtual bool getPeerAddrInfo(const TransportContextId& context_id,
                                 sockaddr_storage* addr) override;

    StreamId create_unidirectional_stream(const TransportContextId& context_id,
                                          uint8_t priority) override;

    // is_sender == false, send stop_sending, else set fin on the stream
    void close_unidirectional_stream(const TransportContextId& context_id,
                                             uint64_t stream_id, bool is_sender) override;

    StreamId createStream(const TransportContextId& context_id,
                          bool use_reliable_transport,
                          uint8_t priority) override;



    TransportError enqueue(const TransportContextId& context_id,
                           const StreamId&  stream_id,
                           std::vector<uint8_t>&& bytes,
                           const uint8_t priority = 1,
                           const uint32_t ttl_ms = 300) override;


    std::optional<std::vector<uint8_t>> dequeue(
      const TransportContextId& context_id,
      const StreamId & stream_id) override;

    /*
     * Internal public methods
     */
    void setStatus(TransportStatus status);

    StreamContext * getZeroStreamContext(picoquic_cnx_t* cnx);

    StreamContext * createStreamContext(picoquic_cnx_t* cnx,
                                       uint64_t stream_id);
    void deleteStreamContext(const TransportContextId& context_id,
                             const StreamId& stream_id);

    void send_next_datagram(StreamContext *stream_cnx, uint8_t* bytes_ctx, size_t max_len);
    void send_stream_bytes(StreamContext *stream_cnx, uint8_t* bytes_ctx, size_t max_len);

    void on_connection_status(StreamContext *stream_cnx,
                              const TransportStatus status);
    void on_new_connection(StreamContext *stream_cnx);
    void on_recv_datagram(StreamContext *stream_cnx,
                          uint8_t* bytes, size_t length);
    void on_recv_stream_bytes(StreamContext *stream_cnx,
                             uint8_t* bytes, size_t length);

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
    TransportContextId createClient();
    void shutdown();

    void server();
    void client(const TransportContextId tcid);
    void cbNotifier();


    /*
     * Variables
     */
    picoquic_quic_config_t config;
    picoquic_quic_t* quic_ctx;
    picoquic_cnx_t *pq_cnx { nullptr };
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

    /*
   * RFC9000 Section 2.1 defines the stream id max value and types.
   *   Type is encoded in the stream id as the first 2 least significant
   *   bits. Stream ID is therefore incremented by 4.
     */
    std::atomic<StreamId> next_stream_id;
    std::map<TransportContextId, std::map<StreamId, StreamContext>> active_streams;

    std::shared_ptr<tick_service> _tick_service;
};

} // namespace qtransport
