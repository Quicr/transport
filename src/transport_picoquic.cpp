#include <cassert>
#include <cstring> // memcpy
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#if defined(__linux__)
#include <net/ethernet.h>
#include <netpacket/packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#endif

#include <picoquic_utils.h>
#include <picoquic_internal.h>
#include "transport_picoquic.h"

using namespace qtransport;

/*
 * PicoQuic Callbacks
 */
int pq_event_cb(picoquic_cnx_t* cnx,
                uint64_t stream_id, uint8_t* bytes, size_t length,
                picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
  PicoQuicTransport *transport = (PicoQuicTransport *)callback_ctx;
  PicoQuicStreamContext *stream_cnx = (PicoQuicStreamContext *)v_stream_ctx;

  std::ostringstream log_msg;
  bool is_fin = false;

  if (transport == NULL) {
    log_msg.str("");
    std::cout << "Null callback CTX" << std::endl;
    return PICOQUIC_ERROR_UNEXPECTED_ERROR;
  }

  switch (fin_or_event) {
    case picoquic_callback_prepare_datagram:
    {
      // length is the max allowed data length

      /* example using provide datagram buffer
      if (length >= 1000) {
        void *buf = picoquic_provide_datagram_buffer(bytes, 1000);
        memset(buf, 'c', length);
      } */

      break;
    }

    case picoquic_callback_datagram_acked:
      // Called for each datagram - TODO: how efficient is that?
      //   bytes carries the original packet data
      log_msg.str("");
      log_msg << "Got datagram ack send_time: " << stream_id
              << " bytes length: " << length;
      transport->logger.log(LogLevel::info, log_msg.str());

      break;

    case picoquic_callback_datagram:
    {
      if (stream_cnx == NULL) {
        // picoquic doesn't provide stream context for datagram/stream_id==-0
        stream_cnx = transport->getZeroStreamContext(cnx);
      }
      transport->on_recv_data(stream_cnx, bytes, length);
      break;
    }

    case picoquic_callback_stream_fin:
      is_fin = true;
      // fall through to picoquic_callback_stream_data
    case picoquic_callback_stream_data:
    {
      if (stream_cnx == NULL) {
        stream_cnx = transport->createStreamContext(cnx, stream_id);
      }

      // length is the amount of data received
      transport->on_recv_data(stream_cnx, bytes, length);

      if (is_fin) transport->deleteStreamContext((uint64_t)cnx, stream_id);
      break;
    }

    case picoquic_callback_stream_reset: {
      log_msg.str("");
      log_msg << "Closing connection stream " << stream_id;
      transport->logger.log(LogLevel::info, log_msg.str());

      if (stream_id == 0) { // close connection
        picoquic_close(cnx, 0);

      } else {
        picoquic_set_callback(cnx, NULL, NULL);
      }

      transport->deleteStreamContext((uint64_t)cnx, stream_id);


      return 0;
    }

    case picoquic_callback_application_close:
    case picoquic_callback_close: {
      log_msg.str("");
      log_msg << "Closing connection stream_id: " << stream_id;

      if (stream_cnx != NULL) {
        log_msg << stream_cnx->peer_addr_text;
      }

      transport->logger.log(LogLevel::info, log_msg.str());

      picoquic_set_callback(cnx, NULL, NULL);
      picoquic_close(cnx, 0);
      transport->deleteStreamContext((uint64_t)cnx, stream_id);

      return 0;
    }

    case picoquic_callback_ready: { // Connection callback, not per stream
      if (stream_cnx == NULL) {
        stream_cnx = transport->createStreamContext(cnx, stream_id);
      }

      if (transport->isServerMode) {

        // Create new stream context for new stream/connection
        picoquic_enable_keep_alive(cnx, 2 * 1000000);
        (void)picoquic_mark_datagram_ready(cnx, true);

        transport->on_new_connection(stream_cnx);
      }

      else {
        // Client
        transport->setStatus(TransportStatus::Ready);
        log_msg << "Connection established to server " << stream_cnx->peer_addr_text
                << " stream_id: " << stream_id;
        transport->logger.log(LogLevel::info, log_msg.str());
      }

      break;
    }

    case picoquic_callback_prepare_to_send:
    {
      uint8_t* buf;
      log_msg.str("");
      log_msg << stream_cnx->peer_addr_text << " Ready to send"
              << " stream: " << stream_id << " (" << stream_cnx->stream_id
              << ")"
              << " max_len: " << length;
      transport->logger.log(LogLevel::debug, log_msg.str());

      if (bytes == NULL) {
        transport->logger.log(LogLevel::info, "bytes is null");
      }

      break;
    }

    default:
      log_msg.str("");
      log_msg << "Got event " << fin_or_event;
      transport->logger.log(LogLevel::debug, log_msg.str());
      break;
  }

  /*
  if (stream_cnx != NULL) {
    log_msg.str("");
    log_msg << stream_cnx->peer_addr_text << " Got event: " << fin_or_event
            << " stream: " << stream_id
            << " bytes_len: " << length;
    transport->logger.log(LogLevel::info, log_msg.str());
  } */

  return 0;
}

int pq_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode,
               void* callback_ctx, void* callback_arg)
{

  PicoQuicTransport *transport = (PicoQuicTransport *)callback_ctx;
  int ret = 0;
  std::ostringstream log_msg;

  if (transport == NULL) {
    return PICOQUIC_ERROR_UNEXPECTED_ERROR;
  }
  else {
    log_msg << "Loop got cb_mode: ";

    switch (cb_mode) {
      case picoquic_packet_loop_ready: {
        log_msg << "packet_loop_ready, waiting for packets";
        transport->logger.log(LogLevel::info, log_msg.str());

        if (transport->isServerMode)
          transport->setStatus(TransportStatus::Ready);

        if (callback_arg != nullptr) {
          auto *options = (picoquic_packet_loop_options_t *) callback_arg;
          options->do_time_check = 1;
        }

        break;
      }

      case picoquic_packet_loop_after_receive:
//        log_msg << "packet_loop_after_receive";
//        transport->logger.log(LogLevel::debug, log_msg.str());
        break;

      case picoquic_packet_loop_after_send:
//        log_msg << "packet_loop_after_send";
//        transport->logger.log(LogLevel::debug, log_msg.str());
        break;

      case picoquic_packet_loop_port_update:
        log_msg << "packet_loop_port_update";
        transport->logger.log(LogLevel::debug, log_msg.str());
        break;

      case picoquic_packet_loop_time_check: {
        /*
        packet_loop_time_check_arg_t* targ = (packet_loop_time_check_arg_t*)callback_arg;
        log_msg << "packet_loop_time_check time: "
            << targ->current_time << " delta: " << targ->delta_t;
        transport->logger.log(LogLevel::info, log_msg.str());
        */

        // TODO: Add config to set this value. This will change the loop select
        //   wait time to delta value in microseconds. Default is <= 10 seconds
        //if (targ->delta_t > 50000)
        //  targ->delta_t = 100000;


        // Stop loop if shutting down
        if (transport->status() == TransportStatus::Shutdown) {
          transport->logger.log(LogLevel::info, "picoquic is shutting down");

          picoquic_cnx_t *close_cnx = picoquic_get_first_cnx(quic);
          while (close_cnx != NULL) {
            log_msg.str("");
            log_msg << "Closing connection id " << (uint64_t)close_cnx;
            transport->logger.log(LogLevel::info, log_msg.str());
            picoquic_close(close_cnx, 0);
            close_cnx = picoquic_get_next_cnx(close_cnx);
          }

          return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
        }

        break;
      }

      default:
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
        break;
    }
  }

  return ret;
}

void PicoQuicTransport::deleteStreamContext(const TransportContextId &context_id,
                                            const StreamId &stream_id)
{
  const auto &iter = active_streams.find(context_id);

  if (iter != active_streams.end()) {
    if (stream_id == 0) // Delete context if stream ID is zero
      (void)active_streams.erase(iter);
    else
      (void)iter->second.erase(stream_id);
  }
}

PicoQuicStreamContext* PicoQuicTransport::getZeroStreamContext(picoquic_cnx_t* cnx) {
  if (cnx == NULL)
    return NULL;

  return &active_streams[(uint64_t)cnx][0];
}

PicoQuicStreamContext* PicoQuicTransport::createStreamContext(
    picoquic_cnx_t* cnx, uint64_t stream_id)
{
  if (cnx == NULL)
    return NULL;

  std::ostringstream log_msg;
  log_msg << "Creating stream context for id: " << stream_id;
  logger.log(LogLevel::info, log_msg.str());

  std::lock_guard<std::mutex> lock(mutex);

  PicoQuicStreamContext* stream_cnx = &active_streams[(uint64_t)cnx][stream_id];
  stream_cnx->stream_id = stream_id;
  stream_cnx->context_id = (uint64_t)cnx;
  stream_cnx->cnx = cnx;

  sockaddr* addr;
  picoquic_get_peer_addr(cnx, &addr);
  std::memset(stream_cnx->peer_addr_text, 0, sizeof(stream_cnx->peer_addr_text));

  switch (addr->sa_family) {
    case AF_INET:
      (void)inet_ntop(AF_INET,
                      (const void*)(&((struct sockaddr_in*)addr)->sin_addr),
                      stream_cnx->peer_addr_text,
                      sizeof(stream_cnx->peer_addr_text));
      stream_cnx->peer_port = ntohs(((struct sockaddr_in*) addr)->sin_port);
      break;

    case AF_INET6:
      (void)inet_ntop(AF_INET6,
                      (const void*)(&((struct sockaddr_in6*)addr)->sin6_addr),
                      stream_cnx->peer_addr_text, sizeof(stream_cnx->peer_addr_text));
      stream_cnx->peer_port = ntohs(((struct sockaddr_in6*) addr)->sin6_port);
      break;
  }

  (void)picoquic_set_app_stream_ctx(cnx, stream_id, stream_cnx);

  return stream_cnx;
}

PicoQuicTransport::PicoQuicTransport(const TransportRemote& server,
                                     const TransportConfig& tcfg,
                                     TransportDelegate& delegate,
                                     bool isServerMode,
                                     LogHandler& logger)
    : stop(false)
      , logger(logger)
      , isServerMode(isServerMode)
      , serverInfo(server)
      , transportStatus(TransportStatus::Connecting)
      , delegate(delegate)
{
  if (isServerMode && tcfg.tls_cert_filename == NULL) {
    throw InvalidConfigException("Missing cert filename");
  }
  else if (tcfg.tls_cert_filename != NULL) {
    (void)picoquic_config_set_option(&config, picoquic_option_CERT,
                               tcfg.tls_cert_filename);

    if (tcfg.tls_key_filename != NULL) {
      (void)picoquic_config_set_option(&config, picoquic_option_KEY,
                                 tcfg.tls_key_filename);
    } else {
      throw InvalidConfigException("Missing cert key filename");
    }
  }


}

PicoQuicTransport::~PicoQuicTransport()
{
  setStatus(TransportStatus::Shutdown);
  shutdown();
}

void PicoQuicTransport::shutdown()
{

  if (stop) // Already stopped
    return;

  stop = true;

  cbNotifyQueue.stopWaiting();

  if (picoQuicThread.joinable()) {
    logger.log(LogLevel::info, "Closing transport pico thread");
    picoQuicThread.join();
  }

  if (cbNotifyThread.joinable()) {
    logger.log(LogLevel::info, "Closing transport callback notifier thread");
    cbNotifyThread.join();
  }

  logger.log(LogLevel::info, "done closing transport threads");

}


TransportStatus
PicoQuicTransport::status()
{
  return transportStatus;
}

void PicoQuicTransport::setStatus(TransportStatus status) {
  transportStatus = status;
}

StreamId PicoQuicTransport::createStream(
  const TransportContextId& context_id,
  bool use_reliable_transport)
{
  logger.log(LogLevel::debug, "App requests to create new stream");

  const auto &iter = active_streams.find(context_id);
  if (iter == active_streams.end()) {
    logger.log(LogLevel::warn, "Invalid context id, cannot create stream");
    return 0;
  }

  const auto &cnx_stream_iter = iter->second.find(0);
  if (cnx_stream_iter == iter->second.end()) {
    logger.log(LogLevel::warn,
               "Missing primary connection zero stream, cannot create streams");
    return 0;
  }

  if (isServerMode) // Server does not create streams, clients must create these
    return 0;
  else if (not use_reliable_transport)
    return 0;

  // Create stream will add stream to active_streams
  PicoQuicStreamContext* stream_cnx = createStreamContext(cnx_stream_iter->second.cnx,
                                                          next_stream_id);

  TransportContextId tcid = context_id;
  StreamId stream_id = next_stream_id;

  cbNotifyQueue.push([&] () {
    delegate.on_new_stream(tcid, stream_id);
  });

  // Set next stream ID
  next_stream_id += 4; // Increment by 4, stream type is first 2 bits

  return stream_cnx->stream_id;
}

TransportContextId
PicoQuicTransport::start()
{
  TransportContextId cid = 0;
  uint64_t current_time = picoquic_current_time();
  debug_set_stream(stdout);  // Enable picoquic debug

  (void)picoquic_config_set_option(&config, picoquic_option_ALPN, QUICR_ALPN);
  (void)picoquic_config_set_option(&config, picoquic_option_MAX_CONNECTIONS, "100");
  quic_ctx = picoquic_create_and_configure(&config,
                                           pq_event_cb,
                                           this,
                                           current_time,
                                           NULL);

  if (quic_ctx == NULL) {
    logger.log(LogLevel::fatal, "Unable to create picoquic context, check certificate and key filenames");
    throw PicoQuicException("Unable to create picoquic context");
  }

  /*
   * TODO doc: Apparently need to set some value to send datagrams. If not set,
   *    max datagram size is zero, preventing sending of datagrams. Setting this
   *    also triggers PMTUD to run. This value will be the initial value.
   */
  picoquic_init_transport_parameters(&local_tp_options, 1);
  local_tp_options.max_datagram_frame_size = 1280;
//  local_tp_options.max_ack_delay = 1000000;
//  local_tp_options.min_ack_delay = 20000;

  picoquic_set_default_tp(quic_ctx, &local_tp_options);

  cbNotifyThread = std::thread(&PicoQuicTransport::cbNotifier, this);

  if (isServerMode) {
    picoQuicThread = std::thread(&PicoQuicTransport::server, this);
    return 0;

  } else {
    cid = createClient();
    picoQuicThread = std::thread(&PicoQuicTransport::client, this, cid);
  }

  return cid;
}

void PicoQuicTransport::cbNotifier()
{
  logger.log(LogLevel::info, "Starting transport callback notifier thread");

  while (not stop) {
    auto cb = cbNotifyQueue.block_pop();
    if (cb) {
      (*cb)();
    }
  }
}

void PicoQuicTransport::server()
{
  int ret = picoquic_packet_loop(quic_ctx, serverInfo.port,
                                 0, 0,
                                 2000000,
                                 0,
                                 pq_loop_cb,
                                 this);

  std::ostringstream log_msg;

  if (quic_ctx != NULL) {
    picoquic_free(quic_ctx);
    quic_ctx = NULL;
  }

  log_msg << "picoquic packet loop ended with " << ret;
  logger.log(LogLevel::info, log_msg.str());

  shutdown();
}

const TransportContextId PicoQuicTransport::createClient() {
  struct sockaddr_storage server_address;
  char const* sni = "cisco.webex.com";
  int ret;
  std::ostringstream log_msg;

  int is_name = 0;

  ret = picoquic_get_server_address(serverInfo.host_or_ip.c_str(), serverInfo.port, &server_address, &is_name);
  if (ret != 0) {
    log_msg.str("");
    log_msg << "Failed to get server: "
            << serverInfo.host_or_ip << " port: " << serverInfo.port;
    logger.log(LogLevel::error, log_msg.str());

  } else if (is_name) {
    sni = serverInfo.host_or_ip.c_str();
  }

  picoquic_set_default_congestion_algorithm(quic_ctx, picoquic_bbr_algorithm);

  uint64_t current_time = picoquic_current_time();

  picoquic_cnx_t* cnx = picoquic_create_cnx(quic_ctx, picoquic_null_connection_id, picoquic_null_connection_id,
                                            (struct sockaddr*) & server_address, current_time, 0,
                                            sni, config.alpn, 1);

  if (cnx == NULL) {
    logger.log(LogLevel::error, "Could not create picoquic connection client context");
    return 0;
  }

  // Using default TP
  //picoquic_set_transport_parameters(cnx, &local_tp_options);

  (void)createStreamContext(cnx, 0);

  return (uint64_t)cnx;
}

void PicoQuicTransport::client(const TransportContextId tcid)
{
  int ret;
  std::ostringstream log_msg;

  picoquic_cnx_t* cnx = active_streams[tcid][0].cnx;

  log_msg << "Thread client packet loop for client context_id: " << tcid;
  logger.log(LogLevel::info, log_msg.str());

  if (cnx == NULL) {
    logger.log(LogLevel::error, "Could not create picoquic connection client context");
  }
  else {
    picoquic_enable_keep_alive(cnx, 2 * 1000000);
    picoquic_set_callback(cnx, pq_event_cb, this);

    ret = picoquic_start_client_cnx(cnx);
    if (ret < 0) {
      logger.log(LogLevel::error, "Could not activate connection");
      return;
    }

    (void)picoquic_mark_datagram_ready(cnx, true);

    ret = picoquic_packet_loop(quic_ctx, 0, AF_INET, 0,
                               2000000, 0,
                               pq_loop_cb, this);

    log_msg.str("");
    log_msg << "picoquic ended with " << ret;
    logger.log(LogLevel::info, log_msg.str());
  }

  if (quic_ctx != NULL) {
    picoquic_free(quic_ctx);
    quic_ctx = NULL;
  }

  shutdown();
}

void
PicoQuicTransport::closeStream(const TransportContextId& context_id,
                               const StreamId stream_id)
{
  std::ostringstream log_msg;

  if (active_streams.count(context_id) > 0) {

    std::lock_guard<std::mutex> lock(mutex);

    if (stream_id == 0) { // Close connection and all streams

      log_msg << "Closing connection id: " << context_id << " with "
              << active_streams[context_id].size() << " streams";
      logger.log(LogLevel::debug, log_msg.str());

      for (const auto& iter : active_streams[context_id]) {
          if (not isServerMode) {
            (void)picoquic_mark_active_stream(iter.second.cnx, stream_id, 0,
                                              NULL);
            (void)picoquic_add_to_stream(iter.second.cnx, stream_id, NULL, 0,
                                         1);
          }
      }

      active_streams.erase(context_id);

    } else {
      const auto& iter = active_streams[context_id].find(stream_id);
      if (iter == active_streams[context_id].end()) {
          log_msg << "Cannot close inactive stream " << context_id << ":"
                  << stream_id;
          logger.log(LogLevel::warn, log_msg.str());
          return;
      }

      log_msg << "Closing stream  " << context_id << ":" << stream_id;
      logger.log(LogLevel::debug, log_msg.str());

      if (iter->second.cnx != NULL) {
          if (not isServerMode) {
            (void)picoquic_mark_active_stream(iter->second.cnx, stream_id, 0,
                                              NULL);
            (void)picoquic_add_to_stream(iter->second.cnx, stream_id, NULL, 0,
                                         1);
          }
      }

      active_streams[context_id].erase(stream_id);
    }
  }
}

void
PicoQuicTransport::close(const TransportContextId& context_id)
{
}


TransportError
PicoQuicTransport::enqueue(const TransportContextId& context_id,
                      const StreamId & stream_id,
                      std::vector<uint8_t>&& bytes)
{
  if (bytes.empty()) {
    return TransportError::None;
  }

  const auto& ctx = active_streams.find(context_id);

  if (ctx != active_streams.end()) {
    const auto& stream_cnx = ctx->second.find(stream_id);
    if (stream_cnx != ctx->second.end()) {
      if (stream_id == 0) {
          (void)picoquic_queue_datagram_frame(stream_cnx->second.cnx,
                                              bytes.size(),
                                              bytes.data());
      } else {
          (void)picoquic_add_to_stream_with_ctx(stream_cnx->second.cnx,
                                                stream_id,
                                                bytes.data(),
                                                bytes.size(),
                                                0, &stream_cnx->second);
      }
    } else {
      return TransportError::InvalidStreamId;
    }
  } else {
    return TransportError::InvalidContextId;
  }

  return TransportError::None;
}

std::optional<std::vector<uint8_t>>
PicoQuicTransport::dequeue(const TransportContextId& context_id,
                           const StreamId & stream_id)
{
  auto cnx_it = active_streams.find(context_id);

  if (cnx_it != active_streams.end()) {
    auto stream_it = cnx_it->second.find(stream_id);
    if (stream_it != cnx_it->second.end()) {
      return stream_it->second.dequeue.pop();
    }
  }

  return std::nullopt;
}

void PicoQuicTransport::on_connection_status(
    const TransportContextId &context_id, const TransportStatus status)
{

}

void PicoQuicTransport::on_new_connection(PicoQuicStreamContext *stream_cnx)
{
  std::ostringstream log_msg;

  log_msg << "New Connection " << stream_cnx->peer_addr_text
          << ":" << stream_cnx->peer_port
          << " conn_ctx: " << (uint64_t)stream_cnx->cnx
          << " stream_id: " << stream_cnx->stream_id;

  logger.log(LogLevel::info, log_msg.str());

  TransportRemote remote {
      .host_or_ip = stream_cnx->peer_addr_text,
      .port = stream_cnx->peer_port,
      .proto = TransportProtocol::QUIC
  };

  TransportContextId context_id { stream_cnx->context_id };

  cbNotifyQueue.push([=] () {
    delegate.on_new_connection(context_id,
                               remote);
    });
}

void PicoQuicTransport::on_recv_data(PicoQuicStreamContext *stream_cnx,
                                     uint8_t* bytes, size_t length)
{
  if (stream_cnx == NULL || length == 0)
    return;

  std::vector<uint8_t> data(bytes, bytes + length);
  stream_cnx->dequeue.push(std::move(data));

  if (stream_cnx->dequeue.size() <= 2) {
    TransportContextId context_id = stream_cnx->context_id;
    StreamId stream_id = stream_cnx->stream_id;

    cbNotifyQueue.push([=] () {
      delegate.on_recv_notify(context_id, stream_id);
      });
  }
}
