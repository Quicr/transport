
#pragma once

#include <cassert>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <transport/transport.h>
#include <picoquic.h>
#include <picoquic_config.h>
#include <picoquic_packet_loop.h>

#include "safe_queue.h"

namespace qtransport {

struct PicoQuicStreamContext {
  uint64_t stream_id;
  TransportContextId context_id;
  picoquic_cnx_t *cnx;
  char peer_addr_text[45];
  uint16_t peer_port;
  safeQueue<std::vector<uint8_t>> dequeue;      /// Pending messages to dequeue
};

class PicoQuicTransport : public ITransport
{
public:
  const char* QUICR_ALPN = "quicr-v1";

  struct connData
  {
    TransportContextId contextId;
    StreamId streamId;
    std::vector<uint8_t> data;
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

public:
  PicoQuicTransport(const TransportRemote& server,
                    const TransportConfig& tcfg,
                    TransportDelegate& delegate,
                    bool isServerMode,
                    LogHandler& logger);

  virtual ~PicoQuicTransport();

  TransportStatus status() override;

  TransportContextId start() override;

  void close(const TransportContextId& context_id) override;
  void closeStream(const TransportContextId& context_id,
                   StreamId stream_id) override;

  StreamId createStream(const TransportContextId& context_id,
                        bool use_reliable_transport) override;

  TransportError enqueue(const TransportContextId& context_id,
                         const StreamId & stream_id,
                         std::vector<uint8_t>&& bytes) override;


  std::optional<std::vector<uint8_t>> dequeue(
    const TransportContextId& context_id,
    const StreamId & stream_id) override;

  /*
   * Internal public methods
   */
  void setStatus(TransportStatus status);

  PicoQuicStreamContext* getZeroStreamContext(picoquic_cnx_t* cnx);

  PicoQuicStreamContext* createStreamContext(picoquic_cnx_t* cnx,
                                             uint64_t stream_id);
  void deleteStreamContext(const TransportContextId& context_id,
                           const StreamId& stream_id);

  void on_connection_status(const TransportContextId &context_id,
                            const TransportStatus status);
  void on_new_connection(PicoQuicStreamContext *stream_cnx);
  void on_recv_data(PicoQuicStreamContext *stream_cnx,
                    uint8_t* bytes, size_t length);


  /*
   * Internal Public Variables
   */
  LogHandler& logger;
  bool isServerMode;


private:
  const TransportContextId createClient();
  void shutdown();

  void server();
  void client(const TransportContextId tcid);
  void cbNotifier();

  /*
   * Variables
   */
  picoquic_quic_config_t config;
  picoquic_quic_t* quic_ctx;
  picoquic_tp_t local_tp_options;
  safeQueue<std::function<void()>> cbNotifyQueue;

  std::mutex mutex;
  std::atomic_bool stop;
  std::atomic<TransportStatus> transportStatus;
  std::thread picoQuicThread;
  std::thread cbNotifyThread;

  TransportRemote serverInfo;
  TransportDelegate& delegate;

  /*
   * RFC9000 Section 2.1 defines the stream id max value and types.
   *   Type is encoded in the stream id as the first 2 least significant
   *   bits. Stream ID is therefore incremented by 4.
   */
  std::atomic<StreamId> next_stream_id{ 4 };
  std::map<TransportContextId, std::map<StreamId, PicoQuicStreamContext>> active_streams;
};

} // namespace qtransport
