#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <vector>
#include <sys/socket.h>

namespace qtransport {

using TransportContextId = uint64_t;      ///< Context Id is a 64bit number that is used as a key to maps
using MediaStreamId = uint64_t;           ///< Media stream Id is a 64bit number that is used as a key to maps

/**
 * Transport status/state values
 */
enum class TransportStatus : uint8_t {
	Ready = 0,
	Connecting,
	RemoteRequestClose,
	Disconnected
};

/**
 * Transport errors
 */
enum class TransportError : uint8_t {
  None = 0,
  UnknownError,
  PeerDisconnected,
  PeerUnreachable,
	CannotResolveHostname,
	InvalidIpv4Address,
	InvalidIpv6Address
};

/**
 *
 */
enum class TransportProtocol {
	UDP=0,
	QUIC
};

/**
 * @brief Remote/Destination endpoint address info.
 *
 * @details Remote destination is either a client or server hostname/ip and port
 */
struct TransportRemote {
	std::string       host_or_ip;   // IPv4/v6 or FQDN (user input)
	uint16_t          port;         // Port (user input)
	TransportProtocol proto;        // Protocol to use for the transport

};

/**
 * Transport configuration parameters
 */
struct TransportConfig {
	// Nothing yet
};

/**
 * @brief IP Transport interface
 *
 * @details A single threaded, async transport interface.
 * 	The transport implementations own the queues
 * 	on which the applications can enqueue the messages
 * 	for transmitting and dequeue for consumption
 *
 * 	Applications using this transport interface
 * 	MUST treat it as thread-unsafe and the same
 * 	is ensured by the transport owing the lock and
 * 	access to the queues.
 *
 * @note Some implementations may cho/ose to
 * 	have enqueue/dequeue being blocking. However
 * 	in such cases applications needs to
 * 	take the burden of non-blocking flows.
 */
class ITransport {
public:
  /**
   * @brief Async Callback API on the transport
   */
  class TransportDelegate {
	public:
    virtual ~TransportDelegate() = default;

    /**
     * @brief Event notification for connection status changes
     *
     * @details Called when the connection changes state/status
		 *
     * @param[in] context_id  Transport context Id
     * @param[in] status 			Transport Status value
     */
    virtual void on_connection_status(const TransportContextId &context_id,
																			const TransportStatus status) = 0;

    /**
     * @brief Report arrival of a new connection
     *
     * @param[in] context_id	Transport context identifier mapped to the connection
     * @param[in] remote			Transport information for the connection
     */
    virtual void on_new_connection(const TransportContextId &context_id,
																	 const TransportRemote remote) = 0;

    /**
     * @brief Event reporting transport has some data over
     * 		the network for the application to consume
     *
     * @details Applications must invoke ITransport::deqeue() to obtain
     * 		the data by passing the transport context id
     *
     * @param[in] context_id 	Transport context identifier mapped to the connection
     */
    virtual void on_recv_notify(const TransportContextId &context_id) = 0;
  };

  /* Factory APIs */

	/**
	 * @brief Create a new client transport based on the remote (server) host/ip
	 *
	 * @param[in] server			Transport remote server information
	 * @param[in] delegate		Implemented callback methods
	 *
	 * @return shared_ptr for the under lining transport.
	 */
  static std::shared_ptr<ITransport>
  make_client_transport(TransportRemote &server,
                        TransportDelegate &delegate);

	/**
	 * @brief Create a new server transport based on the remote (server) ip and port
	 *
	 * @param[in] server			Transport remote server information
	 * @param[in] delegate		Implemented callback methods
	 *
	 * @return shared_ptr for the under lining transport.
	 */
  static std::shared_ptr<ITransport>
  make_server_transport(const TransportRemote &server,
												TransportDelegate &delegate);

public:
  virtual ~ITransport() = default;

  /**
   * @brief Status of the transport
   *
   * @details Return the status of the transport. In server mode, the transport will
   *    reflect the status of the listening socket. In client mode it will reflect
   *    the status of the server connection.
   */
  virtual TransportStatus status() const = 0;

  /**
   * @brief Setup the underlying transport connection
   *
   * @details In server mode this will create the listening socket and will
   * 		start
   *
   * @return TransportContextId: identifying the connection
   */
  virtual TransportContextId start() = 0;

  /**
   * @brief Setup the underlying transport connection
   * @param TransportContextId: identifying the connection
   * @param use_reliable_transport : indicates a reliable stream is
   *                                 preferred for transporting data
   * @return MediaStreamId : o identifying the connection
   */
  virtual MediaStreamId createMediaStream(const TransportContextId &tcid,
                                          bool use_reliable_transport) = 0;

  virtual void close() = 0;

  /**
   * @brief Enqueue application data with the transport
   * The data will be moved from the application and enqueud
   * to the transport queue. The Transport sends the data
   * whenever the credits to transmit data is available
   * by the specific transport implementation
   */
  virtual TransportError enqueue(const TransportContextId &tcid,
                                 const MediaStreamId &msid,
                                 std::vector<uint8_t> &&bytes) = 0;

  /**
   * @brief Dequeue any queued application data, may be empty
   */
  virtual std::optional<std::vector<uint8_t>>
  dequeue(const TransportContextId &tcid, const MediaStreamId &msid) = 0;
};

} // namespace qtransport