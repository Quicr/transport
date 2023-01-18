#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <optional>

namespace qtransport {

using TransportContextId = uint64_t;

enum class TransportStatus: uint8_t
{
    Ready = 0,
    Disconnected
};

enum class Error: uint8_t
{
    None = 0,
    SocketNotOpened,
    UnknownError,
    InvalidHostname,
    NotConnected,
    ConnectionError,
    InvalidFlowId,
    InvalidDestCid,
    ConnectionFailed,
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

      virtual void on_send_status() {};
      virtual void on_connnection_status(TransportStatus& result) {};
      /*
       * @brief Event reporting transport has some data over
       * the network for the application to consume
       * Applications must invoke ITransport::recv to obtain
       * the data
       */
      virtual void on_recv_notify(TransportContextId& tcid) {};
  };

  static std::shared_ptr<ITransport> make_client_transport(const std::string& server, uint16_t port, TransportDelegate& delegate);

  static std::shared_ptr<ITransport> make_server_transport(uint16_t port, TransportDelegate& delegate);

public:

    virtual ~ITransport() = default;

    virtual TransportStatus status() = 0;

    virtual TransportContextId connect() = 0;

    virtual void close() = 0;

    virtual  Error enqueue(TransportContextId& tcid, std::vector<uint8_t>& bytes) = 0;

    virtual std::optional<std::vector<uint8_t>> dequeue(TransportContextId& tcid) = 0;
  
};

}