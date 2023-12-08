#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>
#include <sys/socket.h>

#include <cantina/logger.h>

namespace qtransport {

using TransportConnId = uint64_t;        ///< Connection Id is a 64bit number that is used as a key to maps
using DataContextId = uint64_t;          ///< Data Context 64bit number that identifies a data flow/track/stream
/**
 * Transport status/state values
 */
enum class TransportStatus : uint8_t
{
  Ready = 0,
  Connecting,
  RemoteRequestClose,
  Disconnected,
  Shutdown
};

/**
 * Transport errors
 */
enum class TransportError : uint8_t
{
  None = 0,
  QueueFull,
  UnknownError,
  PeerDisconnected,
  PeerUnreachable,
  CannotResolveHostname,
  InvalidConnContextId,
  InvalidDataContextId,
  InvalidIpv4Address,
  InvalidIpv6Address
};

/**
 * Transport Protocol to use
 */
enum class TransportProtocol
{
  UDP = 0,
  QUIC
};

/**
 * @brief Remote/Destination endpoint address info.
 *
 * @details Remote destination is either a client or server hostname/ip and port
 */
struct TransportRemote
{
  std::string host_or_ip;       /// IPv4/v6 or FQDN (user input)
  uint16_t port;                /// Port (user input)
  TransportProtocol proto;      /// Protocol to use for the transport
};

/**
 * Transport configuration parameters
 */
struct TransportConfig
{
  const char *tls_cert_filename;                        /// QUIC TLS certificate to use
  const char *tls_key_filename;                         /// QUIC TLS private key to use
  const uint32_t time_queue_init_queue_size {1000};     /// Initial queue size to reserve upfront
  const uint32_t time_queue_max_duration {1000};        /// Max duration for the time queue in milliseconds
  const uint32_t time_queue_bucket_interval {1};        /// The bucket interval in milliseconds
  const uint32_t time_queue_size_rx { 1000 };           /// Receive queue size
  bool debug {false};                                   /// Enable debug logging/processing
  const uint64_t quic_cwin_minimum { 131072 };          /// QUIC congestion control minimum size (default is 128k)
  const uint32_t quic_wifi_shadow_rtt_us { 20000 };     /// QUIC wifi shadow RTT in microseconds

  const uint64_t pacing_decrease_threshold_Bps { 16000 };   /// QUIC pacing rate decrease threshold for notification in Bps
  const uint64_t pacing_increase_threshold_Bps { 16000 };   /// QUIC pacing rate increase threshold for notification in Bps

  const uint64_t idle_timeout_ms { 30000 };             /// Idle timeout for transport connection(s) in milliseconds
};

/**
 * @brief ITransport interface
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
class ITransport
{
public:
  /**
   * @brief Async Callback API on the transport
   */
  class TransportDelegate
  {
  public:
    virtual ~TransportDelegate() = default;

    /**
     * @brief Event notification for connection status changes
     *
     * @details Called when the connection changes state/status
     *
     * @param[in] conn_id           Transport context Id
     * @param[in] status 	    Transport Status value
     */
    virtual void on_connection_status(const TransportConnId& conn_id,
                                      const TransportStatus status) = 0;

    /**
     * @brief Report arrival of a new connection
     *
     * @details Called when new connection is received. This is only used in
     * server mode.
     *
     * @param[in] conn_id	Transport context identifier mapped to the connection
     * @param[in] remote	Transport information for the connection
     */
    virtual void on_new_connection(const TransportConnId& conn_id,
                                   const TransportRemote& remote) = 0;

    /**
     * @brief Report a new data context created
     *
     * @details Report that a new data context was created for a new bi-directional
     *  stream that was received. This method is not called for app created
     *  data contexts.
     *
     * @param[in] conn_id	Transport context identifier mapped to the connection
     * @param[in] data_ctx_id	Data context id for a new data context received by the transport
     */
    virtual void on_new_data_context(const TransportConnId& conn_id,
                                     const DataContextId& data_ctx_id) = 0;

    /**
     * @brief Event reporting transport has some data over
     * 		the network for the application to consume
     *
     * @details Applications must invoke ITransport::deqeue() to obtain
     * 		the data by passing the transport context id
     *
     * @param[in] conn_id 	Transport context identifier mapped to the connection
     * @param[in] data_ctx_id	Data context id that the data was received on
     * @param[in] is_bidir      True if the message is from a bidirectional stream
     */
    virtual void on_recv_notify(const TransportConnId& conn_id,
                                const DataContextId& data_ctx_id,
                                const bool is_bidir=false) = 0;
  };

  /* Factory APIs */

  /**
   * @brief Create a new client transport based on the remote (server) host/ip
   *
   * @param[in] server			Transport remote server information
   * @param[in] tcfg                    Transport configuration
   * @param[in] delegate		Implemented callback methods
   * @param[in] logger			Shared pointer to logger
   *
   * @return shared_ptr for the under lining transport.
   */
  static std::shared_ptr<ITransport> make_client_transport(
    const TransportRemote& server,
    const TransportConfig &tcfg,
    TransportDelegate& delegate,
    const cantina::LoggerPointer& logger);

  /**
   * @brief Create a new server transport based on the remote (server) ip and
   * port
   *
   * @param[in] server			Transport remote server information
   * @param[in] tcfg                    Transport configuration
   * @param[in] delegate		Implemented callback methods
   * @param[in] logger			Shared pointer to logger
   *
   * @return shared_ptr for the under lining transport.
   */
  static std::shared_ptr<ITransport> make_server_transport(
    const TransportRemote& server,
    const TransportConfig &tcfg,
    TransportDelegate& delegate,
    const cantina::LoggerPointer& logger);

public:
  virtual ~ITransport() = default;

  /**
   * @brief Status of the transport
   *
   * @details Return the status of the transport. In server mode, the transport
   * will reflect the status of the listening socket. In client mode it will
   * reflect the status of the server connection.
   */
  virtual TransportStatus status() const = 0;

  /**
   * @brief Setup the transport connection
   *
   * @details In server mode this will create the listening socket and will
   * 		start listening on the socket for new connections. In client
   * mode this will initiate a connection to the remote/server.
   *
   * @return TransportContextId: identifying the connection
   */
  virtual TransportConnId start() = 0;

  /**
   * @brief Create a data context
   * @details Data context is flow of data (track, namespace). This is similar to a pipe of data to be transmitted.
   *        Metrics, shaping, etc. maintained at the data context level.
   *
   * @param[in] conn_id                 Connection ID to create data context
   * @param[in] use_reliable_transport 	Indicates a reliable stream is
   *                                 	preferred for transporting data
   * @param[in] priority                Priority for stream (default is 1)
   * @param[in] bidir                   Set context to be bi-directional or unidirectional
   *
   * @return DataContextId identifying the data context via the connection
   */
  virtual DataContextId createDataContext(const TransportConnId conn_id,
                                          bool use_reliable_transport,
                                          uint8_t priority = 1,
                                          bool bidir = false) = 0;

  /**
   * @brief Close a transport context
   */
  virtual void close(const TransportConnId& conn_id) = 0;

  /**
   * @brief Delete data context
   * @details Deletes a data context for the given connection id. If reliable, the stream will
   *    be closed by FIN (graceful).
   *
   * @param[in] conn_id                 Connection ID to create data context
   * @param[in] data_ctx_id             Data context ID to delete
   */
  virtual void deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) = 0;

  /**
   * @brief Get the peer IP address and port associated with the stream
   *
   * @param[in]  context_id	Identifying the connection
   * @param[out] addr	Peer address
   *
   * @returns True if the address was successfully returned, false otherwise
   */
  virtual bool getPeerAddrInfo(const TransportConnId& context_id,
                               sockaddr_storage* addr) = 0;

  /**
   * Enqueue flags
   */
  struct EnqueueFlags
  {
    bool new_stream { false };          /// Indicates that a new stream should be created to replace existing one
    bool clear_tx_queue { false };      /// Indicates that the TX queue should be cleared before adding new object
    bool use_reset { false };           /// Indicates new stream created will close the previous using reset/abrupt
  };

  /**
   * @brief Enqueue application data within the transport
   *
   * @details Add data to the transport queue. Data enqueued will be transmitted
   * when available.
   *
   * @param[in] context_id	Identifying the connection
   * @param[in] data_ctx_id	stream Id to send data on
   * @param[in] bytes		Data to send/write
   * @param[in] priority        Priority of the object, range should be 0 - 255
   * @param[in] ttl_ms          The age the object should exist in queue in milliseconds
   *
   * @param[in] new_stream      Indicates that a new stream should replace the existing on
   * @param[in] clear_queue     Clear the TX priority queue before enqueueing this object
   *
   * @returns TransportError is returned indicating status of the operation
   */
  virtual TransportError enqueue(const TransportConnId& context_id,
                                 const DataContextId& data_ctx_id,
                                 std::vector<uint8_t>&& bytes,
                                 const uint8_t priority = 1,
                                 const uint32_t ttl_ms=350,
                                 const EnqueueFlags flags={false, false, false}) = 0;

  /**
   * @brief Dequeue application data from transport queue
   *
   * @details Data received by the transport will be queued and made available
   * to the caller using this method.  An empty return will be
   *
   * @param[in] context_id		Identifying the connection
   * @param[in] data_ctx_id	        Stream Id to receive data from
   *
   * @returns std::nullopt if there is no data
   */
  virtual std::optional<std::vector<uint8_t>> dequeue(
    const TransportConnId& context_id,
    const DataContextId& data_ctx_id) = 0;
};

} // namespace qtransport
