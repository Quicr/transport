#include "transport_picoquic.h"

#include <picoquic_internal.h>
#include <picoquic_utils.h>

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <ctime>
#include <iostream>
#include <netdb.h>
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

using namespace qtransport;

namespace {
    /**
     * @brief Returns the appropriate stream id depending on if the stream is
     *        initiated by the client or the server, and if it bi- or uni- directional.
     *
     * @tparam Int_t The preferred integer type to deal with.
     *
     * @param id  The initial value to adjust
     * @param is_server Flag if the initiating request is from a server or a client
     * @param is_unidirectional Flag if the streams are bi- or uni- directional.
     *
     * @return The correctly adjusted stream id value.
     */
    constexpr StreamId make_stream_id(StreamId id, bool is_server, bool is_unidirectional)
    {
        return (id & (~0x0u << 2)) | (is_server ? 0b01 : 0b00) | (is_unidirectional ? 0b10 : 0b00);
    }

    /**
     * @brief Defines the default/datagram stream depending on if the stream is
     *        initiated by the client or the server, and if it bi- or uni- directional.
     *
     * @param is_server Flag if the initiating request is from a server or a client
     * @param is_unidirectional Flag if the streams are bi- or uni- directional.
     *
     * @return The datagram stream id.
     */
    constexpr StreamId make_datagram_stream_id(bool is_server, bool is_unidirectional)
    {
        return 0;
    }
} // namespace

/*
 * PicoQuic Callbacks
 */
int
pq_event_cb(picoquic_cnx_t* cnx,
            uint64_t stream_id,
            uint8_t* bytes,
            size_t length,
            picoquic_call_back_event_t fin_or_event,
            void* callback_ctx,
            void* v_stream_ctx)
{
    PicoQuicTransport* transport = static_cast<PicoQuicTransport*>(callback_ctx);
    PicoQuicTransport::StreamContext* stream_cnx = static_cast<PicoQuicTransport::StreamContext*>(v_stream_ctx);

    std::ostringstream log_msg;
    bool is_fin = false;

    if (transport == NULL) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (fin_or_event) {

        case picoquic_callback_prepare_datagram: {
            // length is the max allowed data length
            if (stream_cnx == NULL) {
                // picoquic doesn't provide stream context for datagram/stream_id==-0
                stream_cnx = transport->getZeroStreamContext(cnx);
            }
            transport->metrics.dgram_prepare_send++;
            transport->send_next_datagram(stream_cnx, bytes, length);
            break;
        }

        case picoquic_callback_datagram_acked:
            // Called for each datagram - TODO: Revisit how often acked frames are coming in
            //   bytes carries the original packet data
            //      log_msg.str("");
            //      log_msg << "Got datagram ack send_time: " << stream_id
            //              << " bytes length: " << length;
            //      transport->logger.log(LogLevel::info, log_msg.str());
            transport->metrics.dgram_ack++;
            break;

        case picoquic_callback_datagram_spurious:
            // TODO: Add metrics for spurious datagrams
            // delayed ack
            // std::cout << "spurious DGRAM length: " << length << std::endl;
            transport->metrics.dgram_spurious++;
            break;

        case picoquic_callback_datagram_lost:
            // TODO: Add metrics for lost datagrams
            // std::cout << "Lost DGRAM length " << length << std::endl;
            transport->metrics.dgram_lost++;
            break;

        case picoquic_callback_datagram: {
            if (stream_cnx == NULL) {
                // picoquic doesn't provide stream context for datagram/stream_id==-0
                stream_cnx = transport->getZeroStreamContext(cnx);
            }

            transport->metrics.dgram_received++;
            transport->on_recv_datagram(stream_cnx, bytes, length);
            break;
        }

        case picoquic_callback_stream_fin:
            is_fin = true;
            // fall through to picoquic_callback_stream_data
        case picoquic_callback_stream_data: {
            transport->metrics.stream_rx_callbacks++;

            if (stream_cnx == NULL) {
                stream_cnx = transport->createStreamContext(cnx, stream_id);
            }
            // length is the amount of data received
            transport->on_recv_stream_bytes(stream_cnx, bytes, length);

            if (is_fin)
                transport->deleteStreamContext(reinterpret_cast<uint64_t>(cnx), stream_id);
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

            transport->deleteStreamContext(reinterpret_cast<uint64_t>(cnx), stream_id);
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
            transport->deleteStreamContext(reinterpret_cast<uint64_t>(cnx), stream_id);

            if (not transport->_is_server_mode) {
                transport->setStatus(TransportStatus::Disconnected);

                // TODO: Fix picoquic. Apparently picoquic is not processing return values for this callback
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }

            return 0;
        }

        case picoquic_callback_ready: { // Connection callback, not per stream
            if (stream_cnx == NULL) {
                stream_cnx = transport->createStreamContext(cnx, stream_id);
            }

            picoquic_enable_keep_alive(cnx, 3000000);
            (void)picoquic_mark_datagram_ready(cnx, 1);

            if (transport->_is_server_mode) {
                transport->on_new_connection(stream_cnx);
            }

            else {
                // Client
                transport->setStatus(TransportStatus::Ready);
                log_msg << "Connection established to server " << stream_cnx->peer_addr_text
                        << " stream_id: " << stream_id;
                transport->on_connection_status(stream_cnx, TransportStatus::Ready);
                transport->logger.log(LogLevel::info, log_msg.str());
            }

            break;
        }

        case picoquic_callback_prepare_to_send: {
            transport->metrics.stream_prepare_send++;
            transport->send_stream_bytes(stream_cnx, bytes, length);
            break;
        }

        default:
            log_msg.str("");
            log_msg << "Got event " << fin_or_event;
            transport->logger.log(LogLevel::debug, log_msg.str());
            break;
    }

    return 0;
}

int
pq_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, void* callback_ctx, void* callback_arg)
{
    PicoQuicTransport* transport = static_cast<PicoQuicTransport*>(callback_ctx);
    int ret = 0;
    std::ostringstream log_msg;

    if (transport == NULL) {
        std::cerr << "picoquic transport was called with NULL transport" << std::endl;
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;

    } else if (transport->status() == TransportStatus::Disconnected) {
        return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;

    } else {
        transport->pq_runner();

        log_msg << "Loop got cb_mode: ";

        switch (cb_mode) {
            case picoquic_packet_loop_ready: {
                log_msg << "packet_loop_ready, waiting for packets";
                transport->logger.log(LogLevel::info, log_msg.str());

                if (transport->_is_server_mode)
                    transport->setStatus(TransportStatus::Ready);

                if (callback_arg != nullptr) {
                    auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
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
                static uint64_t prev_time = 0;
                static qtransport::PicoQuicTransport::Metrics prev_metrics;

                packet_loop_time_check_arg_t* targ = static_cast<packet_loop_time_check_arg_t*>(callback_arg);

                // TODO: Add config to set this value. This will change the loop select
                //   wait time to delta value in microseconds. Default is <= 10 seconds
                if (targ->delta_t > 1000) {
                    targ->delta_t = 1000;
                }

                if (!prev_time) {
                    prev_time = targ->current_time;
                    prev_metrics = transport->metrics;
                }

                transport->metrics.time_checks++;

                if (transport->debug && targ->current_time - prev_time > 500000) {

                    if (transport->metrics != prev_metrics) {
                        std::ostringstream log_msg;
                        log_msg << "Metrics: " << std::endl
                                << "   time checks        : " << transport->metrics.time_checks << std::endl
                                << "   enqueued_objs      : " << transport->metrics.enqueued_objs << std::endl
                                << "   send with null ctx : " << transport->metrics.send_null_bytes_ctx << std::endl
                                << "   ----[ Datagrams ] --------------" << std::endl
                                << "   dgram_recv         : " << transport->metrics.dgram_received << std::endl
                                << "   dgram_sent         : " << transport->metrics.dgram_sent << std::endl
                                << "   dgram_prepare_send : " << transport->metrics.dgram_prepare_send << " ("
                                << transport->metrics.dgram_prepare_send - prev_metrics.dgram_prepare_send << ")"
                                << std::endl
                                << "   dgram_lost         : " << transport->metrics.dgram_lost << std::endl
                                << "   dgram_ack          : " << transport->metrics.dgram_ack << std::endl
                                << "   dgram_spurious     : " << transport->metrics.dgram_spurious << " ("
                                << transport->metrics.dgram_spurious + transport->metrics.dgram_ack << ")" << std::endl
                                << "   ----[ Streams ] --------------" << std::endl
                                << "   stream_prepare_send: " << transport->metrics.stream_prepare_send << std::endl
                                << "   stream_objects_sent: " << transport->metrics.stream_objects_sent << std::endl
                                << "   stream_bytes_sent  : " << transport->metrics.stream_bytes_sent << std::endl
                                << "   stream_rx_callbacks: " << transport->metrics.stream_rx_callbacks << std::endl
                                << "   stream_objects_recv: " << transport->metrics.stream_objects_recv << std::endl
                                << "   stream_bytes_recv  : " << transport->metrics.stream_bytes_recv << std::endl;

                        transport->logger.log(LogLevel::debug, log_msg.str());
                        prev_metrics = transport->metrics;
                    }

                    prev_time = targ->current_time;
                }

                // Stop loop if shutting down
                if (transport->status() == TransportStatus::Shutdown) {
                    transport->logger.log(LogLevel::info, "picoquic is shutting down");

                    picoquic_cnx_t* close_cnx = picoquic_get_first_cnx(quic);
                    while (close_cnx != NULL) {
                        log_msg.str("");
                        log_msg << "Closing connection id " << reinterpret_cast<uint64_t>(close_cnx);
                        transport->logger.log(LogLevel::info, log_msg.str());
                        picoquic_close(close_cnx, 0);
                        close_cnx = picoquic_get_next_cnx(close_cnx);
                    }

                    return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
                }

                break;
            }

            default:
                //ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
                transport->logger.log(LogLevel::warn, "pq_loop_cb() does not implement " + std::to_string(cb_mode));
                break;
        }
    }

    return ret;
}

void
PicoQuicTransport::deleteStreamContext(const TransportContextId& context_id, const StreamId& stream_id)
{
    std::ostringstream log_msg;
    log_msg << "Delete stream context for id: " << stream_id;
    logger.log(LogLevel::info, log_msg.str());

    std::lock_guard<std::mutex> lock(_state_mutex);

    const auto& active_stream_it = active_streams.find(context_id);
    if (active_stream_it == active_streams.end())
        return;

    if (stream_id == 0) {
        StreamContext* s_cnx = &active_streams[context_id][stream_id];

        on_connection_status(s_cnx, TransportStatus::Disconnected);
        picoquic_close_immediate(s_cnx->cnx);

        active_streams.erase(active_stream_it);
        return;
    }

    auto& [ctx_id, stream_contexts] = *active_stream_it;
    const auto& stream_iter = stream_contexts.find(stream_id);
    if (stream_iter == stream_contexts.end())
        return;

    const auto& [_, ctx] = *stream_iter;
    picoquic_runner_queue.push([=, cnx = ctx.cnx]() {
        picoquic_mark_active_stream(cnx, stream_id, 0, NULL);
        picoquic_add_to_stream(cnx, stream_id, NULL, 0, 1);
        // TODO: Is this needed if we already set the stream FIN?
        // picoquic_discard_stream(stream_iter->second.cnx, stream_id, 0);
    });

    (void)stream_contexts.erase(stream_id);
}

PicoQuicTransport::StreamContext*
PicoQuicTransport::getZeroStreamContext(picoquic_cnx_t* cnx)
{
    if (cnx == NULL)
        return NULL;

    // This should be safe since it's not removed unless the connection is removed. Zero is reserved for datagram
    return &active_streams[reinterpret_cast<uint64_t>(cnx)][0];
}

PicoQuicTransport::StreamContext*
PicoQuicTransport::createStreamContext(picoquic_cnx_t* cnx, uint64_t stream_id)
{
    if (cnx == NULL)
        return NULL;

    std::lock_guard<std::mutex> lock(_state_mutex);

    StreamContext* stream_cnx = &active_streams[reinterpret_cast<uint64_t>(cnx)][stream_id];
    stream_cnx->stream_id = stream_id;
    stream_cnx->context_id = reinterpret_cast<uint64_t>(cnx);
    stream_cnx->cnx = cnx;

    stream_cnx->rx_data = std::make_unique<safeQueue<bytes_t>>(tconfig.time_queue_size_rx);

    stream_cnx->tx_data = std::make_unique<priority_queue<bytes_t>>(
      tconfig.time_queue_max_duration, tconfig.time_queue_bucket_interval, _timer,
      tconfig.time_queue_init_queue_size,
      tconfig.pq_queue_spike_period_ms, tconfig.pq_queue_spike_duration_ms);

    sockaddr* addr;

    picoquic_get_peer_addr(cnx, &addr);
    std::memset(stream_cnx->peer_addr_text, 0, sizeof(stream_cnx->peer_addr_text));
    std::memcpy(&stream_cnx->peer_addr, addr, sizeof(stream_cnx->peer_addr));

    switch (addr->sa_family) {
        case AF_INET:
            (void)inet_ntop(AF_INET,
                            &reinterpret_cast<struct sockaddr_in*>(addr)->sin_addr,
                            /*(const void*)(&((struct sockaddr_in*)addr)->sin_addr),*/
                            stream_cnx->peer_addr_text,
                            sizeof(stream_cnx->peer_addr_text));
            stream_cnx->peer_port = ntohs(((struct sockaddr_in*)addr)->sin_port);
            break;

        case AF_INET6:
            (void)inet_ntop(AF_INET6,
                            &reinterpret_cast<struct sockaddr_in6*>(addr)->sin6_addr,
                            /*(const void*)(&((struct sockaddr_in6*)addr)->sin6_addr), */
                            stream_cnx->peer_addr_text,
                            sizeof(stream_cnx->peer_addr_text));
            stream_cnx->peer_port = ntohs(((struct sockaddr_in6*)addr)->sin6_port);
            break;
    }

    picoquic_runner_queue.push([=]() {
        picoquic_set_app_stream_ctx(cnx, stream_id, stream_cnx);
    });

    if (stream_id) {
        picoquic_runner_queue.push([=, this]() {
                picoquic_mark_active_stream(cnx, stream_id, 1, stream_cnx);
        });
    }

    return stream_cnx;
}

PicoQuicTransport::PicoQuicTransport(const TransportRemote& server,
                                     const TransportConfig& tcfg,
                                     TransportDelegate& delegate,
                                     bool _is_server_mode,
                                     LogHandler& logger)
  : logger(logger)
  , _is_server_mode(_is_server_mode)
  , stop(false)
  , transportStatus(TransportStatus::Connecting)
  , serverInfo(server)
  , delegate(delegate)
  , tconfig(tcfg)
  , next_stream_id{ ::make_datagram_stream_id(_is_server_mode, _is_unidirectional) }
{
    debug = tcfg.debug;

    picoquic_config_init(&config);

    if (_is_server_mode && tcfg.tls_cert_filename == NULL) {
        throw InvalidConfigException("Missing cert filename");
    } else if (tcfg.tls_cert_filename != NULL) {
        (void)picoquic_config_set_option(&config, picoquic_option_CERT, tcfg.tls_cert_filename);

        if (tcfg.tls_key_filename != NULL) {
            (void)picoquic_config_set_option(&config, picoquic_option_KEY, tcfg.tls_key_filename);
        } else {
            throw InvalidConfigException("Missing cert key filename");
        }
    }

    _timer = std::make_shared<queue_timer_thread>();
}

PicoQuicTransport::~PicoQuicTransport()
{
    setStatus(TransportStatus::Shutdown);
    shutdown();
}

void
PicoQuicTransport::shutdown()
{
    if (stop) // Already stopped
        return;

    stop = true;

    if (picoQuicThread.joinable()) {
        logger.log(LogLevel::info, "Closing transport pico thread");
        picoQuicThread.join();
    }

    picoquic_runner_queue.stopWaiting();
    cbNotifyQueue.stopWaiting();

    if (cbNotifyThread.joinable()) {
        logger.log(LogLevel::info, "Closing transport callback notifier thread");
        cbNotifyThread.join();
    }

    _timer.reset();
    logger.log(LogLevel::info, "done closing transport threads");

    picoquic_config_clear(&config);
}

TransportStatus
PicoQuicTransport::status() const
{
    return transportStatus;
}

void
PicoQuicTransport::setStatus(TransportStatus status)
{
    transportStatus = status;
}

StreamId
PicoQuicTransport::createStream(const TransportContextId& context_id, bool use_reliable_transport)
{
    const auto& iter = active_streams.find(context_id);
    if (iter == active_streams.end()) {
        throw std::invalid_argument("Invalid context id, cannot create stream");
    }

    const auto datagram_stream_id = ::make_datagram_stream_id(_is_server_mode, _is_unidirectional);
    const auto& cnx_stream_iter = iter->second.find(datagram_stream_id);
    if (cnx_stream_iter == iter->second.end()) {
        createStreamContext(cnx_stream_iter->second.cnx, datagram_stream_id);
    }

    if (!use_reliable_transport)
        return datagram_stream_id;

    next_stream_id = ::make_stream_id(next_stream_id + 4, _is_server_mode, _is_unidirectional);

    PicoQuicTransport::StreamContext* stream_cnx = createStreamContext(cnx_stream_iter->second.cnx, next_stream_id);

    cbNotifyQueue.push([&] { delegate.on_new_stream(context_id, next_stream_id); });

    return stream_cnx->stream_id;
}

TransportContextId
PicoQuicTransport::start()
{
    uint64_t current_time = picoquic_current_time();

    if (debug) {
        debug_set_stream(stdout); // Enable picoquic debug
    }

    (void)picoquic_config_set_option(&config, picoquic_option_ALPN, QUICR_ALPN);
    (void)picoquic_config_set_option(&config, picoquic_option_MAX_CONNECTIONS, "100");
    quic_ctx = picoquic_create_and_configure(&config, pq_event_cb, this, current_time, NULL);

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
    //  local_tp_options.max_packet_size = 1450;
    //  local_tp_options.max_ack_delay = 3000000;
    //  local_tp_options.min_ack_delay = 500000;

    picoquic_set_default_tp(quic_ctx, &local_tp_options);

    picoquic_runner_queue.setLimit(2000);

    cbNotifyQueue.setLimit(2000);
    cbNotifyThread = std::thread(&PicoQuicTransport::cbNotifier, this);

    TransportContextId cid = 0;
    std::ostringstream log_msg;

    if (_is_server_mode) {

        log_msg << "Starting server, listening on " << serverInfo.host_or_ip << ':' << serverInfo.port;
        logger.log(LogLevel::info, log_msg.str());

        picoQuicThread = std::thread(&PicoQuicTransport::server, this);

    } else {
        log_msg << "Connecting to server " << serverInfo.host_or_ip << ':' << serverInfo.port;
        logger.log(LogLevel::info, log_msg.str());

        cid = createClient();
        picoQuicThread = std::thread(&PicoQuicTransport::client, this, cid);
    }

    return cid;
}

void PicoQuicTransport::pq_runner() {

    while (auto cb = std::move(picoquic_runner_queue.pop())) {
        (*cb)();
    }
}

void
PicoQuicTransport::cbNotifier()
{
    logger.log(LogLevel::info, "Starting transport callback notifier thread");

    while (not stop) {
        auto cb = std::move(cbNotifyQueue.block_pop());
        if (cb) {
            (*cb)();
        } else {
            logger.log(LogLevel::info, "Notify callback is NULL");
        }
    }

    logger.log(LogLevel::info, "Done with transport callback notifier thread");
}

void
PicoQuicTransport::server()
{
    int ret = picoquic_packet_loop(quic_ctx, serverInfo.port, 0, 0, 2000000, 0, pq_loop_cb, this);

    std::ostringstream log_msg;

    if (quic_ctx != NULL) {
        picoquic_free(quic_ctx);
        quic_ctx = NULL;
    }

    log_msg << "picoquic packet loop ended with " << ret;
    logger.log(LogLevel::info, log_msg.str());

    setStatus(TransportStatus::Shutdown);
}

TransportContextId
PicoQuicTransport::createClient()
{
    struct sockaddr_storage server_address;
    char const* sni = "cisco.webex.com";
    int ret;
    std::ostringstream log_msg;

    int is_name = 0;

    ret = picoquic_get_server_address(serverInfo.host_or_ip.c_str(), serverInfo.port, &server_address, &is_name);
    if (ret != 0) {
        log_msg.str("");
        log_msg << "Failed to get server: " << serverInfo.host_or_ip << " port: " << serverInfo.port;
        logger.log(LogLevel::error, log_msg.str());

    } else if (is_name) {
        sni = serverInfo.host_or_ip.c_str();
    }

    picoquic_set_default_congestion_algorithm(quic_ctx, picoquic_bbr_algorithm);

    uint64_t current_time = picoquic_current_time();

    picoquic_cnx_t* cnx = picoquic_create_cnx(quic_ctx,
                                              picoquic_null_connection_id,
                                              picoquic_null_connection_id,
                                              reinterpret_cast<struct sockaddr*>(&server_address),
                                              current_time,
                                              0,
                                              sni,
                                              config.alpn,
                                              1);

    if (cnx == NULL) {
        logger.log(LogLevel::error, "Could not create picoquic connection client context");
        return 0;
    }

    // Using default TP
    picoquic_set_transport_parameters(cnx, &local_tp_options);

    (void)createStreamContext(cnx, 0);

    return reinterpret_cast<uint64_t>(cnx);
}

void
PicoQuicTransport::client(const TransportContextId tcid)
{
    int ret;
    std::ostringstream log_msg;

    picoquic_cnx_t* cnx = active_streams[tcid][0].cnx;

    log_msg << "Thread client packet loop for client context_id: " << tcid;
    logger.log(LogLevel::info, log_msg.str());

    if (cnx == NULL) {
        logger.log(LogLevel::error, "Could not create picoquic connection client context");
    } else {
        picoquic_set_callback(cnx, pq_event_cb, this);

        picoquic_enable_keep_alive(cnx, 3000000);

        ret = picoquic_start_client_cnx(cnx);
        if (ret < 0) {
            logger.log(LogLevel::error, "Could not activate connection");
            return;
        }

        ret = picoquic_packet_loop(quic_ctx, 0, AF_INET, 0, 2000000, 0, pq_loop_cb, this);

        log_msg.str("");
        log_msg << "picoquic ended with " << ret;
        logger.log(LogLevel::info, log_msg.str());
    }

    if (quic_ctx != NULL) {
        picoquic_free(quic_ctx);
        quic_ctx = NULL;
    }

    setStatus(TransportStatus::Disconnected);
}

void
PicoQuicTransport::closeStream(const TransportContextId& context_id, const StreamId stream_id)
{
    deleteStreamContext(context_id, stream_id);
}

bool PicoQuicTransport::getPeerAddrInfo(const TransportContextId& context_id,
                                        sockaddr_storage* addr)
{
    // Locate the specified transport context
    auto it = active_streams.find(context_id);

    // If not found, return false
    if (it == active_streams.end()) return false;

    // Try to locate stream 0
    auto it_stream = it->second.find(0);

    // If not found, it suggests the connection is actually closed
    if (it_stream == it->second.end()) return false;

    // Copy the address
    std::memcpy(addr, &it_stream->second.peer_addr, sizeof(sockaddr_storage));

    return true;
}

void
PicoQuicTransport::close([[maybe_unused]] const TransportContextId& context_id)
{
}

void
PicoQuicTransport::send_next_datagram(StreamContext* stream_cnx, uint8_t* bytes_ctx, size_t max_len)
{
    if (bytes_ctx == NULL) {
        metrics.send_null_bytes_ctx++;
        return;
    }

    if (stream_cnx->stream_id != 0) {
        // Only for datagrams
        return;
    }

    static auto prev_time = std::chrono::steady_clock::now();
    auto now_time = std::chrono::steady_clock::now();
    auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(now_time - prev_time).count();
    prev_time = now_time;

    const auto& out_data = stream_cnx->tx_data->front();
    if (out_data.has_value()) {
        if (max_len >= out_data->size()) {
            stream_cnx->tx_data->pop();

            metrics.dgram_sent++;

            uint8_t* buf = NULL;

            buf = picoquic_provide_datagram_buffer_ex(bytes_ctx,
                                                      out_data->size(),
                                                      stream_cnx->tx_data->empty() ? picoquic_datagram_not_active : picoquic_datagram_active_any_path);

            if (buf != NULL) {
                std::memcpy(buf, out_data->data(), out_data->size());
            }
        }
        else {
            picoquic_provide_datagram_buffer_ex(bytes_ctx, 0, picoquic_datagram_active_any_path);
        }
    }
    else {
        picoquic_provide_datagram_buffer_ex(bytes_ctx, 0, picoquic_datagram_not_active);
    }
}

void
PicoQuicTransport::send_stream_bytes(StreamContext* stream_cnx, uint8_t* bytes_ctx, size_t max_len)
{
    if (bytes_ctx == NULL) {
        metrics.send_null_bytes_ctx++;
        return;
    }

    if (stream_cnx->stream_id == 0) {
        return; // Only for steams
    }

    uint32_t data_len = 0;                                  /// Length of data to follow the 4 byte length
    size_t offset = 0;
    int is_still_active = 0;

    if (stream_cnx->stream_tx_object == nullptr) {

        if (max_len < 5) {
            // Not enough bytes to send
            logger.log(LogLevel::info, (std::ostringstream()
                                        << "Not enough bytes to send stream size header sid: "
                                        << stream_cnx->stream_id).str());
            return;
        }

        auto obj = stream_cnx->tx_data->pop_front();
        if (obj.has_value()) {
            metrics.stream_objects_sent++;
            max_len -= 4; // Subtract out the length header that will be added

            stream_cnx->stream_tx_object = new uint8_t[obj->size()];
            stream_cnx->stream_tx_object_size = obj->size();
            std::memcpy(stream_cnx->stream_tx_object, obj->data(), obj->size());

            if (obj->size() > max_len) {
                stream_cnx->stream_tx_object_offset = max_len;
                data_len = max_len;
                is_still_active = 1;

            } else {
                data_len = obj->size();
                stream_cnx->stream_tx_object_offset = 0;
            }

        } else {
            // queue is empty during pop
            return;
        }
    }
    else { // Have existing object with remaining bytes to send.
        data_len = stream_cnx->stream_tx_object_size - stream_cnx->stream_tx_object_offset;
        offset = stream_cnx->stream_tx_object_offset;

        if (data_len > max_len) {
            stream_cnx->stream_tx_object_offset += max_len;
            data_len = max_len;
            is_still_active = 1;

        } else {
            stream_cnx->stream_tx_object_offset = 0;
        }
    }

    metrics.stream_bytes_sent += data_len;

    if (!is_still_active && !stream_cnx->tx_data->empty())
        is_still_active = 1;

    uint8_t *buf = nullptr;

    if (offset == 0) {
        // New object being written, add length (4 bytes), little endian for now
        uint32_t obj_length = stream_cnx->stream_tx_object_size;

        buf = picoquic_provide_stream_data_buffer(bytes_ctx,
                                                  data_len + 4,
                                                  0,
                                                  is_still_active);
        if (buf != NULL) {
            std::memcpy(buf, reinterpret_cast<uint8_t *>(&obj_length), 4);
            buf += 4;
        } else {
            // Error allocating memory to write
            return;
        }
    } else {
        buf = picoquic_provide_stream_data_buffer(bytes_ctx,
                                                  data_len,
                                                  0,
                                                  is_still_active);

        if (buf == NULL) {
            // Error allocating memory to write
            return;
        }
    }

    // Write data
    std::memcpy(buf, stream_cnx->stream_tx_object + offset, data_len);

    if (stream_cnx->stream_tx_object_offset == 0 && stream_cnx->stream_tx_object != nullptr) {
        // Zero offset at this point means the object was fully sent
        delete []stream_cnx->stream_tx_object;
        stream_cnx->stream_tx_object = nullptr;
    }
}

TransportError
PicoQuicTransport::enqueue(const TransportContextId& context_id,
                           const StreamId& stream_id,
                           std::vector<uint8_t>&& bytes,
                           const uint8_t priority,
                           const uint32_t ttl_ms)
{
    if (bytes.empty()) {
        std::cerr << "enqueue dropped due bytes empty" << std::endl;
        return TransportError::None;
    }

    std::lock_guard<std::mutex> lock(_state_mutex);

    const auto& ctx = active_streams.find(context_id);

    if (ctx != active_streams.end()) {
        const auto& stream_cnx = ctx->second.find(stream_id);

        if (stream_cnx != ctx->second.end()) {
            metrics.enqueued_objs++;
            stream_cnx->second.tx_data->push(bytes, ttl_ms, priority);

            if (!stream_id) {
                picoquic_runner_queue.push([=]() {
                    if (stream_cnx->second.cnx != NULL)
                        picoquic_mark_datagram_ready(stream_cnx->second.cnx, 1);
                });

            } else {
                picoquic_runner_queue.push([=]() {
                    if (stream_cnx->second.cnx != NULL)
                        picoquic_mark_active_stream(
                          stream_cnx->second.cnx, stream_id, 1, static_cast<StreamContext*>(&stream_cnx->second));
                });
            }
        } else {
            std::cerr << "enqueue dropped due to invalid stream: " << stream_id << std::endl;
            return TransportError::InvalidStreamId;
        }
    } else {
        //std::cerr << "enqueue dropped due to invalid contextId: " << context_id << std::endl;
        //TODO: Add metrics to report invalid context
        return TransportError::InvalidContextId;
    }
    return TransportError::None;
}

std::optional<std::vector<uint8_t>>
PicoQuicTransport::dequeue(const TransportContextId& context_id, const StreamId& stream_id)
{
    std::lock_guard<std::mutex> lock(_state_mutex);

    auto cnx_it = active_streams.find(context_id);

    if (cnx_it != active_streams.end()) {
        auto stream_it = cnx_it->second.find(stream_id);
        if (stream_it != cnx_it->second.end()) {
            return std::move(stream_it->second.rx_data->pop());
        }
    }

    return std::nullopt;
}

void
PicoQuicTransport::on_connection_status(PicoQuicTransport::StreamContext* stream_cnx, const TransportStatus status)
{
    TransportContextId context_id{ stream_cnx->context_id };

    cbNotifyQueue.push([=, this]() { delegate.on_connection_status(context_id, status); });
}

void
PicoQuicTransport::on_new_connection(StreamContext* stream_cnx)
{
    std::ostringstream log_msg;

    log_msg << "New Connection " << stream_cnx->peer_addr_text << ":" << stream_cnx->peer_port
            << " conn_ctx: " << reinterpret_cast<uint64_t>(stream_cnx->cnx) << " stream_id: " << stream_cnx->stream_id;

    logger.log(LogLevel::info, log_msg.str());

    TransportRemote remote{ .host_or_ip = stream_cnx->peer_addr_text,
                            .port = stream_cnx->peer_port,
                            .proto = TransportProtocol::QUIC };

    TransportContextId context_id{ stream_cnx->context_id };

    cbNotifyQueue.push([=, this]() { delegate.on_new_connection(context_id, remote); });
}

void
PicoQuicTransport::on_recv_datagram(StreamContext* stream_cnx, uint8_t* bytes, size_t length)
{
    if (stream_cnx == NULL || length == 0) {
        logger.log(LogLevel::warn, "On receive datagram has null context");
        return;
    }

    std::vector<uint8_t> data(bytes, bytes + length);
    stream_cnx->rx_data->push(std::move(data));

    if (cbNotifyQueue.size() > 200) {
        logger.log(LogLevel::info, (std::ostringstream()
                                    << "on_recv_datagram cbNotifyQueue size"
                                    << cbNotifyQueue.size()).str());
    }

    if (stream_cnx->rx_data->size() < 2 || stream_cnx->in_data_cb_skip_count > 30) {
        stream_cnx->in_data_cb_skip_count = 0;
        TransportContextId context_id = stream_cnx->context_id;
        StreamId stream_id = stream_cnx->stream_id;

        cbNotifyQueue.push([=, this]() { delegate.on_recv_notify(context_id, stream_id); });
    } else {
        stream_cnx->in_data_cb_skip_count++;
    }
}

void PicoQuicTransport::on_recv_stream_bytes(StreamContext* stream_cnx, uint8_t* bytes, size_t length)
{
    if (stream_cnx == NULL || length == 0) {
        logger.log(LogLevel::warn, "on_recv_stream_bytes has null context");
        return;
    }

    bool object_complete = false;

    if (stream_cnx->stream_rx_object == nullptr) {
        if (length < 5) {
            logger.log(LogLevel::warn, (std::ostringstream()
                                        << "on_recv_stream_bytes is less than 5, cannot process sid: "
                                        << std::to_string(stream_cnx->stream_id)).str());

            // TODO: Should reset stream in this case
            return;
        }

        // Start of new object being received is the 4 byte length value
        std::memcpy(&stream_cnx->stream_rx_object_size, bytes, 4);
        bytes += 4;
        length -= 4;

        if (stream_cnx->stream_rx_object_size > 40000000) { // Safety check
            logger.log(LogLevel::warn, (std::ostringstream()
                                        << "on_recv_stream_bytes sid: " << stream_cnx->stream_id
                                        << " data length is too large: "
                                        << std::to_string(stream_cnx->stream_rx_object_size)).str());
            stream_cnx->stream_rx_object_size = 0;

            // TODO: Should reset stream in this case
            return;
        }

        if (stream_cnx->stream_rx_object_size <= length) {
            object_complete = true;

            std::vector<uint8_t> data(bytes, bytes + stream_cnx->stream_rx_object_size);
            stream_cnx->rx_data->push(std::move(data));

            bytes += stream_cnx->stream_rx_object_size;
            length -= stream_cnx->stream_rx_object_size;
            metrics.stream_bytes_recv += stream_cnx->stream_rx_object_size;
        }
        else {
            // Need to wait for more data, create new object buffer
            stream_cnx->stream_rx_object = new uint8_t[stream_cnx->stream_rx_object_size];

            stream_cnx->stream_rx_object_offset = length;
            std::memcpy(stream_cnx->stream_rx_object, bytes, length);
            length = 0;
        }
    }
    else { // Existing object, append
        size_t remaining_len = stream_cnx->stream_rx_object_size - stream_cnx->stream_rx_object_offset;

        if (remaining_len > length) {
            remaining_len = length;
            length = 0;
        } else {
            object_complete = true;
            length -= remaining_len;
        }

        metrics.stream_bytes_recv += remaining_len;

        std::memcpy(stream_cnx->stream_rx_object + stream_cnx->stream_rx_object_offset, bytes, remaining_len);
        bytes += remaining_len;

        if (object_complete) {
            std::vector<uint8_t> data(stream_cnx->stream_rx_object, stream_cnx->stream_rx_object + stream_cnx->stream_rx_object_size);
            stream_cnx->rx_data->push(std::move(data));

            delete []stream_cnx->stream_rx_object;
            stream_cnx->stream_rx_object = nullptr;
            stream_cnx->stream_rx_object_size = 0;
            stream_cnx->stream_rx_object_offset = 0;
        }
        else {
            stream_cnx->stream_rx_object_offset += remaining_len;
        }
    }

    if (object_complete) {
        metrics.stream_objects_recv++;

        bool too_many_in_queue = false;
        if (cbNotifyQueue.size() > 200) {
            logger.log(LogLevel::warn, (std::ostringstream()
                                        << "on_recv_stream_bytes sid: " << stream_cnx->stream_id
                                        << "cbNotifyQueue size" << cbNotifyQueue.size()).str());

        }

        if (too_many_in_queue || stream_cnx->rx_data->size() < 2 || stream_cnx->in_data_cb_skip_count > 30) {
            stream_cnx->in_data_cb_skip_count = 0;
            TransportContextId context_id = stream_cnx->context_id;
            StreamId stream_id = stream_cnx->stream_id;

            cbNotifyQueue.push([=, this]() { delegate.on_recv_notify(context_id, stream_id); });
        } else {
            stream_cnx->in_data_cb_skip_count++;
        }
    }

    if (length > 0) {
        logger.log(LogLevel::info, (std::ostringstream()
                                     << "on_stream_bytes has remaining bytes: " << length).str());
        on_recv_stream_bytes(stream_cnx, bytes, length);
    }
}