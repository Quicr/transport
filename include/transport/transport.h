#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace qtransport {

using TransportContextId = uint64_t;
using MediaStreamId = uint64_t;

enum class TransportStatus : uint8_t { Ready = 0, Disconnected };

enum class Error : uint8_t {
  None = 0,
  UnknownError,
  PeerDisconnected,
  PeerUnreachable,
};

/*
 * A single threaded, async transport interface.
 * The transport implementations own the queues
 * on which the applications can enqueue the messages
 * for transmitting and dequeue for consumption
 *
 * Applications using this transport interface
 * MUST treat it as thread-unsafe and the same
 * is ensured by the transport owing the lock and
 * access to the queues.
 *
 * Note: Some implementations may cho/ose to
 * have enqueue/dequeue being blocking. However
 * in such cases applications needs to
 * take the burden of non-blocking flows.
 */
class ITransport {
public:
  /*
   * @brief Async Callback API on the transport
   */
  struct TransportDelegate {
    virtual ~TransportDelegate() = default;

    /*
     * @brief Report status of the underlying transport connection
     * @param status : Transport Status
     */
    virtual void on_connection_status(const TransportStatus status) = 0;

    /*
     * @brief Report arrival of a new connection
     * @param context_id : Transport context identifier mapped to the connection
     */
    virtual void on_new_connection(const TransportContextId &context_id) = 0;

    /*
     * @brief Event reporting transport has some data over
     * the network for the application to consume
     * Applications must invoke ITransport::deqeue to obtain
     * the data by passing the transport context id
     *
     * param context_id : Transport context identifier mapped to the connection
     */
    virtual void on_recv_notify(TransportContextId &tcid) = 0;
  };

  /* Factory APIs */
  static std::shared_ptr<ITransport>
  make_client_transport(const std::string &server, uint16_t port,
                        TransportDelegate &delegate);
  static std::shared_ptr<ITransport>
  make_server_transport(uint16_t port, TransportDelegate &delegate);

public:
  virtual ~ITransport() = default;

  /*
   * @brief API to obtain latest know status of the transport connection
   */
  virtual TransportStatus status() const = 0;

  /*
   * @brief Setup the underlying transport connection
   * @return TransportContextId: identifying the connection
   */
  virtual TransportContextId start() = 0;

  /*
   * @brief Setup the underlying transport connection
   * @param TransportContextId: identifying the connection
   * @param use_reliable_transport : indicates a reliable stream is
   *                                 preferred for transporting data
   * @return MediaStreamId : o identifying the connection
   */
  virtual MediaStreamId createMediaStream(const TransportContextId &tcid,
                                          bool use_reliable_transport) = 0;

  virtual void close() = 0;

  /*
   * @brief Enqueue application data with the transport
   * The data will be moved from the application and enqueud
   * to the transport queue. The Transport sends the data
   * whenever the credits to transmit data is available
   * by the specific transport implementation
   */
  virtual Error enqueue(const TransportContextId &tcid,
                        const MediaStreamId &msid,
                        std::vector<uint8_t> &&bytes) = 0;

  /*
   * @brief Dequeue any queued application data, may be empty
   */
  virtual std::optional<std::vector<uint8_t>>
  dequeue(const TransportContextId &tcid, const MediaStreamId &msid) = 0;
};

} // namespace qtransport