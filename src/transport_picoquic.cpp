#include "transport_picoquic.h"

#include <picoquic_internal.h>
#include <picoquic_utils.h>

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <ctime>
#include <iostream>
#include <netdb.h>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/select.h>
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

    bool is_fin = false;

    if (transport == NULL) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    transport->pq_runner();

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
            transport->metrics.dgram_spurious++;
            break;

        case picoquic_callback_datagram_lost:
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
                picoquic_set_app_stream_ctx(cnx, stream_id, stream_cnx);
            }

            // length is the amount of data received
            if (length) {
                transport->on_recv_stream_bytes(stream_cnx, bytes, length);
            }

            if (is_fin) {
                transport->logger->info << "Received FIN for stream " << stream_id << std::flush;
                transport->deleteStreamContext(reinterpret_cast<uint64_t>(cnx), stream_id);
            }
            break;
        }

        case picoquic_callback_stream_reset: {
            transport->logger->info << "Reset stream " << stream_id
                                    << std::flush;

            if (stream_id == 0) { // close connection
                picoquic_close(cnx, 0);

            } else {
                picoquic_set_callback(cnx, NULL, NULL);
            }

            transport->deleteStreamContext(reinterpret_cast<uint64_t>(cnx), stream_id);
            return 0;
        }

        case picoquic_callback_almost_ready:
            break;

        case picoquic_callback_path_suspended:
            break;

        case picoquic_callback_path_deleted:
            break;

        case picoquic_callback_path_available:
            break;

        case picoquic_callback_path_quality_changed:
            break;

        case picoquic_callback_pacing_changed: {
            const auto cwin_bytes = picoquic_get_cwin(cnx);
            const auto rtt_us = picoquic_get_rtt(cnx);

            transport->logger->info << "Pacing rate changed; context_id: " << reinterpret_cast<uint64_t>(cnx)
                                    << " rate bps: " << stream_id * 8
                                    << " cwin_bytes: " << cwin_bytes
                                    << " rtt_us: " << rtt_us
                                    << std::flush;
            break;
        }

        case picoquic_callback_application_close:
        case picoquic_callback_close: {
            transport->logger->info << "Closing connection stream_id: "
                                    << " local_error: " << picoquic_get_local_error(cnx)
                                    << " remote_error: " << picoquic_get_remote_error(cnx)
                                    << " app_error: " << picoquic_get_application_error(cnx)
                                    << stream_id;

            if (stream_cnx != NULL) {
                auto conn_ctx = transport->getConnContext(reinterpret_cast<uint64_t>(cnx));

                transport->logger->info << " " << conn_ctx.peer_addr_text;
            }

            transport->logger->info << std::flush;

            picoquic_set_callback(cnx, NULL, NULL);


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
                transport->on_new_connection(cnx);
            }

            else {
                // Client
                transport->setStatus(TransportStatus::Ready);
                transport->on_connection_status(stream_cnx, TransportStatus::Ready);
            }

            break;
        }

        case picoquic_callback_prepare_to_send: {
            transport->metrics.stream_prepare_send++;
            transport->send_stream_bytes(stream_cnx, bytes, length);
            break;
        }

        default:
            LOGGER_DEBUG(transport->logger, "Got event " << fin_or_event);
            break;
    }

    return 0;
}

int
pq_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, void* callback_ctx, void* callback_arg)
{
    PicoQuicTransport* transport = static_cast<PicoQuicTransport*>(callback_ctx);
    int ret = 0;

    if (transport == NULL) {
        std::cerr << "picoquic transport was called with NULL transport" << std::endl;
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;

    } else if (transport->status() == TransportStatus::Disconnected) {
        return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;

    } else {
        transport->pq_runner();


        switch (cb_mode) {
            case picoquic_packet_loop_ready: {
                transport->logger->info << "packet_loop_ready, waiting for packets" << std::flush;

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
                LOGGER_DEBUG(transport->logger, "packet_loop_port_update");
                break;

            case picoquic_packet_loop_time_check: {
                packet_loop_time_check_arg_t* targ = static_cast<packet_loop_time_check_arg_t*>(callback_arg);

                // TODO: Add config to set this value. This will change the loop select
                //   wait time to delta value in microseconds. Default is <= 10 seconds
                if (targ->delta_t > 4000) {
                    targ->delta_t = 4000;
                }

                if (!transport->prev_time) {
                    transport->prev_time = targ->current_time;
                    transport->prev_metrics = transport->metrics;
                }

                transport->metrics.time_checks++;

                if (targ->current_time - transport->prev_time > 1000000) {

                    // TODO: Debug only mode for now. Will remove or change based on findings
                    if (transport->debug) {
                        transport->check_conns_for_congestion();
                    }

                    if (transport->debug && transport->metrics != transport->prev_metrics) {
/*
                        LOGGER_DEBUG(
                          transport->logger,
                          "Metrics: "
                            << std::endl
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
                            << "   stream_bytes_recv  : " << transport->metrics.stream_bytes_recv << std::endl);
*/
                        transport->prev_metrics = transport->metrics;
                    }

                    transport->prev_time = targ->current_time;
                }

                // Stop loop if shutting down
                if (transport->status() == TransportStatus::Shutdown) {
                    transport->logger->Log("picoquic is shutting down");

                    picoquic_cnx_t* close_cnx = picoquic_get_first_cnx(quic);
                    while (close_cnx != NULL) {
                        transport->logger->info << "Closing connection id " << reinterpret_cast<uint64_t>(close_cnx)
                                                << std::flush;
                        picoquic_close(close_cnx, 0);
                        close_cnx = picoquic_get_next_cnx(close_cnx);
                    }

                    return 0; //PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP; picoquic_close() will stop the loop
                }

                break;
            }

            default:
                //ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
                transport->logger->warning << "pq_loop_cb() does not implement " + std::to_string(cb_mode)
                                           << std::flush;
                break;
        }
    }

    return ret;
}

void
PicoQuicTransport::deleteStreamContext(const TransportContextId& context_id, const StreamId& stream_id, bool fin_stream)
{
    std::lock_guard<std::mutex> lock(_state_mutex);

    const auto& active_stream_it = active_streams.find(context_id);
    if (active_stream_it == active_streams.end())
        return;

    logger->info << "Delete stream context for stream " << stream_id << std::flush;

    if (stream_id == 0) {
        StreamContext* s_cnx = &active_streams[context_id][stream_id];

        on_connection_status(s_cnx, TransportStatus::Disconnected);

        // Remove pointer references in picoquic for active streams
        for (const auto& [sid, _]: active_streams[context_id]) {
            picoquic_add_to_stream(s_cnx->cnx, stream_id, NULL, 0, 1);
        }

        picoquic_close(s_cnx->cnx, 0);

        // Remove all streams if closing the root/datagram stream (closed connection)
        active_streams.erase(context_id);

        conn_context.erase(context_id);

        return;
    }

    auto& [ctx_id, stream_contexts] = *active_stream_it;
    const auto& stream_iter = stream_contexts.find(stream_id);
    if (stream_iter == stream_contexts.end())
        return;

    const auto& [_, ctx] = *stream_iter;
    picoquic_runner_queue.push([=, cnx = ctx.cnx]() {
        picoquic_mark_active_stream(cnx, stream_id, 0, NULL);
    });

    if (fin_stream) {
        logger->info << "Sending FIN to stream " << stream_id << std::flush;
        picoquic_runner_queue.push([=, cnx = ctx.cnx]() {
            picoquic_add_to_stream(cnx, stream_id, NULL, 0, 1);
        });
    }

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

PicoQuicTransport::ConnectionContext PicoQuicTransport::getConnContext(const TransportContextId& context_id)
{
    std::lock_guard<std::mutex> lock(_state_mutex);

    ConnectionContext conn_ctx { };

    // Locate the specified transport connection context
    auto it = conn_context.find(context_id);

    // If not found, return false
    if (it == conn_context.end()) return std::move(conn_ctx);

    conn_ctx = it->second;

    return std::move(conn_ctx);
}


PicoQuicTransport::ConnectionContext& PicoQuicTransport::createConnContext(picoquic_cnx_t *cnx)
{
    auto [conn_it, _] = conn_context.emplace(
      reinterpret_cast<uint64_t>(cnx),
      ConnectionContext{
        .cnx = cnx, .peer_addr_text = { 0 }, .peer_port = 0, .is_congested = false, .total_retransmits = 0 });

    sockaddr* addr;

    auto &conn_ctx = conn_it->second;

    picoquic_get_peer_addr(cnx, &addr);
    std::memset(conn_ctx.peer_addr_text, 0, sizeof(conn_ctx.peer_addr_text));
    std::memcpy(&conn_ctx.peer_addr, addr, sizeof(conn_ctx.peer_addr));

    switch (addr->sa_family) {
        case AF_INET:
            (void)inet_ntop(AF_INET,
                            &reinterpret_cast<struct sockaddr_in*>(addr)->sin_addr,
                            /*(const void*)(&((struct sockaddr_in*)addr)->sin_addr),*/
                            conn_ctx.peer_addr_text,
                            sizeof(conn_ctx.peer_addr_text));
            conn_ctx.peer_port = ntohs(((struct sockaddr_in*)addr)->sin_port);
            break;

        case AF_INET6:
            (void)inet_ntop(AF_INET6,
                            &reinterpret_cast<struct sockaddr_in6*>(addr)->sin6_addr,
                            /*(const void*)(&((struct sockaddr_in6*)addr)->sin6_addr), */
                            conn_ctx.peer_addr_text,
                            sizeof(conn_ctx.peer_addr_text));
            conn_ctx.peer_port = ntohs(((struct sockaddr_in6*)addr)->sin6_port);
            break;
    }

    return conn_ctx;
}

PicoQuicTransport::StreamContext*
PicoQuicTransport::createStreamContext(picoquic_cnx_t* cnx, uint64_t stream_id)
{
    if (cnx == NULL)
        return NULL;

    if (stream_id == 0) {
        createConnContext(cnx);
    }

    std::lock_guard<std::mutex> lock(_state_mutex);

    StreamContext* stream_cnx = &active_streams[reinterpret_cast<uint64_t>(cnx)][stream_id];
    stream_cnx->stream_id = stream_id;
    stream_cnx->context_id = reinterpret_cast<uint64_t>(cnx);
    stream_cnx->cnx = cnx;

    stream_cnx->rx_data = std::make_unique<safe_queue<bytes_t>>(tconfig.time_queue_size_rx);

    stream_cnx->tx_data = std::make_unique<priority_queue<bytes_t>>(
      tconfig.time_queue_max_duration, tconfig.time_queue_bucket_interval, _tick_service,
      tconfig.time_queue_init_queue_size);


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
                                     const cantina::LoggerPointer& logger)
  : logger(std::make_shared<cantina::Logger>("QUIC", logger))
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

    _tick_service = std::make_shared<threaded_tick_service>();
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
        logger->Log("Closing transport pico thread");
        picoQuicThread.join();
    }

    picoquic_runner_queue.stop_waiting();
    cbNotifyQueue.stop_waiting();

    if (cbNotifyThread.joinable()) {
        logger->Log("Closing transport callback notifier thread");
        cbNotifyThread.join();
    }

    _tick_service.reset();
    logger->Log("done closing transport threads");

    picoquic_config_clear(&config);

    // If logging picoquic events, stop those
    if (picoquic_logger) {
        debug_set_callback(NULL, NULL);
        picoquic_logger.reset();
    }
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
PicoQuicTransport::createStream(const TransportContextId& context_id,
                                bool use_reliable_transport,
                                uint8_t priority)
{
    if (priority > 127) {
        throw std::runtime_error("Create stream priority cannot be greater than 127, range is 0 - 127");
    }

    const auto& iter = active_streams.find(context_id);
    if (iter == active_streams.end()) {
        logger->info << "Invalid context id, cannot create stream. context_id = " << context_id << std::flush;
        return 0;
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
    stream_cnx->priority = priority;

    /*
     * Low order bit set indicates FIFO handling of same priorities, unset is round-robin
     */
    picoquic_runner_queue.push([=, this]() {
        picoquic_set_app_stream_ctx(cnx_stream_iter->second.cnx, next_stream_id, stream_cnx);
        picoquic_set_stream_priority(cnx_stream_iter->second.cnx, next_stream_id, (priority << 1));
    });

    cbNotifyQueue.push([&] { delegate.on_new_stream(context_id, next_stream_id); });

    return stream_cnx->stream_id;
}

TransportContextId
PicoQuicTransport::start()
{
    uint64_t current_time = picoquic_current_time();

    if (debug) {
        picoquic_logger = std::make_shared<cantina::Logger>("PQIC", logger);
        debug_set_callback(&PicoQuicTransport::PicoQuicLogging, this);
    }

    (void)picoquic_config_set_option(&config, picoquic_option_CC_ALGO, "bbr");
    (void)picoquic_config_set_option(&config, picoquic_option_ALPN, QUICR_ALPN);
    (void)picoquic_config_set_option(&config, picoquic_option_CWIN_MIN,
                                     std::to_string(tconfig.quic_cwin_minimum).c_str());
    (void)picoquic_config_set_option(&config, picoquic_option_MAX_CONNECTIONS, "100");

    quic_ctx = picoquic_create_and_configure(&config, pq_event_cb, this, current_time, NULL);

    if (quic_ctx == NULL) {
        logger->critical << "Unable to create picoquic context, check certificate and key filenames"
                         << std::flush;
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
    local_tp_options.idle_timeout = 30000; // TODO: Remove when we add reconnnect change back to 10 seconds
    local_tp_options.max_ack_delay = 100000;
    local_tp_options.min_ack_delay = 1000;

    picoquic_set_default_tp(quic_ctx, &local_tp_options);

    picoquic_set_default_wifi_shadow_rtt(quic_ctx, tconfig.quic_wifi_shadow_rtt_us);
    logger->info << "Setting wifi shadow RTT to " << tconfig.quic_wifi_shadow_rtt_us << "us" << std::flush;

    picoquic_runner_queue.set_limit(2000);

    cbNotifyQueue.set_limit(2000);
    cbNotifyThread = std::thread(&PicoQuicTransport::cbNotifier, this);

    TransportContextId cid = 0;
    std::ostringstream log_msg;

    if (_is_server_mode) {

        logger->info << "Starting server, listening on " << serverInfo.host_or_ip << ':' << serverInfo.port
                     << std::flush;

        picoQuicThread = std::thread(&PicoQuicTransport::server, this);

    } else {
        logger->info << "Connecting to server " << serverInfo.host_or_ip << ':' << serverInfo.port
                     << std::flush;

        cid = createClient();
        picoQuicThread = std::thread(&PicoQuicTransport::client, this, cid);
    }

    return cid;
}

void PicoQuicTransport::pq_runner() {

    if (picoquic_runner_queue.empty()) {
        return;
    }

    // note: check before running move of optional, which is more CPU taxing when empty
    while (auto cb = picoquic_runner_queue.pop()) {
        (*cb)();
    }
}

void
PicoQuicTransport::cbNotifier()
{
    logger->Log("Starting transport callback notifier thread");

    while (not stop) {
        auto cb = std::move(cbNotifyQueue.block_pop());
        if (cb) {
            (*cb)();
        } else {
            logger->Log("Notify callback is NULL");
        }
    }

    logger->Log("Done with transport callback notifier thread");
}

void
PicoQuicTransport::server()
{
    int ret = picoquic_packet_loop(quic_ctx, serverInfo.port, PF_UNSPEC, 0, 2000000, 0, pq_loop_cb, this);

    if (quic_ctx != NULL) {
        picoquic_free(quic_ctx);
        quic_ctx = NULL;
    }

    logger->info << "picoquic packet loop ended with " << ret << std::flush;

    setStatus(TransportStatus::Shutdown);
}

TransportContextId
PicoQuicTransport::createClient()
{
    struct sockaddr_storage server_address;
    char const* sni = "cisco.webex.com";
    int ret;

    int is_name = 0;

    ret = picoquic_get_server_address(serverInfo.host_or_ip.c_str(), serverInfo.port, &server_address, &is_name);
    if (ret != 0) {
        logger->error << "Failed to get server: " << serverInfo.host_or_ip << " port: " << serverInfo.port
                      << std::flush;
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
        logger->Log(cantina::LogLevel::Error, "Could not create picoquic connection client context");
        return 0;
    }

    // Using default TP
    picoquic_set_transport_parameters(cnx, &local_tp_options);

    picoquic_subscribe_pacing_rate_updates(cnx, tconfig.pacing_decrease_threshold_Bps,
                                           tconfig.pacing_increase_threshold_Bps);

    (void)createStreamContext(cnx, 0);

    return reinterpret_cast<uint64_t>(cnx);
}

void
PicoQuicTransport::client(const TransportContextId tcid)
{
    int ret;

    picoquic_cnx_t* cnx = active_streams[tcid][0].cnx;

    logger->info << "Thread client packet loop for client context_id: " << tcid
                 << std::flush;

    if (cnx == NULL) {
        logger->Log(cantina::LogLevel::Error, "Could not create picoquic connection client context");
    } else {
        picoquic_set_callback(cnx, pq_event_cb, this);

        picoquic_enable_keep_alive(cnx, 3000000);

        ret = picoquic_start_client_cnx(cnx);
        if (ret < 0) {
            logger->Log(cantina::LogLevel::Error, "Could not activate connection");
            return;
        }

        ret = picoquic_packet_loop(quic_ctx, 0, PF_UNSPEC, 0, 2000000, 0, pq_loop_cb, this);

        logger->info << "picoquic ended with " << ret << std::flush;
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
    deleteStreamContext(context_id, stream_id, true);
}

bool PicoQuicTransport::getPeerAddrInfo(const TransportContextId& context_id,
                                        sockaddr_storage* addr)
{
    std::lock_guard<std::mutex> lock(_state_mutex);

    // Locate the specified transport connection context
    auto it = conn_context.find(context_id);

    // If not found, return false
    if (it == conn_context.end()) return false;

    // Copy the address
    std::memcpy(addr, &it->second.peer_addr, sizeof(sockaddr_storage));

    return true;
}

void
PicoQuicTransport::close([[maybe_unused]] const TransportContextId& context_id)
{
}

void
PicoQuicTransport::check_callback_delta(StreamContext* stream_cnx, bool tx) {
    auto now_time = std::chrono::steady_clock::now();

    if (!tx) return;

    const auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now_time - stream_cnx->last_tx_callback_time).count();

    stream_cnx->last_tx_callback_time = std::move(now_time);

    if (delta_ms > 50 && stream_cnx->tx_data->size() > 10) {
        stream_cnx->metrics.tx_delayed_callback++;

        logger->debug << "context_id: " << reinterpret_cast<uint64_t>(stream_cnx->cnx)
                      << " stream_id: " << stream_cnx->stream_id
                      << " pri: " << static_cast<int>(stream_cnx->priority)
                      << " CB TX delta " << delta_ms << " ms"
                      << " count: " << stream_cnx->metrics.tx_delayed_callback
                      << " tx_queue_size: "
                      << stream_cnx->tx_data->size() << std::flush;
    }
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

    check_callback_delta(stream_cnx);

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

    check_callback_delta(stream_cnx);

    uint32_t data_len = 0;                                  /// Length of data to follow the 4 byte length
    size_t offset = 0;
    int is_still_active = 0;

    if (stream_cnx->stream_tx_object == nullptr) {

        if (max_len < 5) {
            // Not enough bytes to send
            logger->debug << "Not enough bytes to send stream size header, waiting for next callback. sid: "
                         << stream_cnx->stream_id << std::flush;
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
        stream_cnx->reset_tx_object();
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

    if (status == TransportStatus::Ready) {
        auto conn_ctx = getConnContext(stream_cnx->context_id);
        logger->info << "Connection established to server "
                     << conn_ctx.peer_addr_text
                     << std::flush;

    }

    cbNotifyQueue.push([=, this]() { delegate.on_connection_status(context_id, status); });
}

void
PicoQuicTransport::on_new_connection(picoquic_cnx_t *cnx)
{
    const auto context_id = reinterpret_cast<uint64_t>(cnx);

    auto conn_ctx = getConnContext(context_id);
    logger->info << "New Connection " << conn_ctx.peer_addr_text << " port: " << conn_ctx.peer_port
                 << " context_id: " << context_id
                 << std::flush;

    picoquic_subscribe_pacing_rate_updates(cnx, tconfig.pacing_decrease_threshold_Bps,
                                           tconfig.pacing_increase_threshold_Bps);

    TransportRemote remote{ .host_or_ip = conn_ctx.peer_addr_text,
                            .port = conn_ctx.peer_port,
                            .proto = TransportProtocol::QUIC };

    cbNotifyQueue.push([=, this]() { delegate.on_new_connection(context_id, remote); });
}

void
PicoQuicTransport::on_recv_datagram(StreamContext* stream_cnx, uint8_t* bytes, size_t length)
{
    if (stream_cnx == NULL || length == 0) {
        logger->Log(cantina::LogLevel::Debug, "On receive datagram has null context");
        return;
    }

    std::vector<uint8_t> data(bytes, bytes + length);
    stream_cnx->rx_data->push(std::move(data));

    if (cbNotifyQueue.size() > 200) {
        logger->info << "on_recv_datagram cbNotifyQueue size"
                     << cbNotifyQueue.size() << std::flush;
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
    uint8_t *bytes_p = bytes;

    if (stream_cnx == NULL || length == 0) {
        logger->Log(cantina::LogLevel::Debug, "on_recv_stream_bytes has null context");
        return;
    }

    bool object_complete = false;

    if (stream_cnx->stream_rx_object == nullptr) {

        if (stream_cnx->stream_rx_object_hdr_size < 4) {

            uint16_t len_to_copy = length >= 4 ? 4 - stream_cnx->stream_rx_object_hdr_size : 4;


            std::memcpy(&stream_cnx->stream_rx_object_size + stream_cnx->stream_rx_object_hdr_size,
                        bytes_p, len_to_copy);

            stream_cnx->stream_rx_object_hdr_size += len_to_copy;

            if (stream_cnx->stream_rx_object_hdr_size < 4) {
                logger->debug << "Stream header not complete. hdr " << stream_cnx->stream_rx_object_hdr_size
                              << " len_to_copy: " << len_to_copy
                              << " length: " << length
                              << std::flush;
            }

            length -= len_to_copy;
            bytes_p += len_to_copy;


            if (length == 0 || stream_cnx->stream_rx_object_hdr_size < 4) {
                // Either no data left to read or not enough data for the header (length) value
                return;
            }
        }

        if (stream_cnx->stream_rx_object_size > 40000000L) { // Safety check
            logger->warning << "on_recv_stream_bytes sid: " << stream_cnx->stream_id
                            << " data length is too large: "
                            << std::to_string(stream_cnx->stream_rx_object_size)
                            << std::flush;

            stream_cnx->reset_rx_object();
            // TODO: Should reset stream in this case
            return;
        }

        if (stream_cnx->stream_rx_object_size <= length) {
            object_complete = true;

            std::vector<uint8_t> data(bytes_p, bytes_p + stream_cnx->stream_rx_object_size);
            stream_cnx->rx_data->push(std::move(data));

            bytes_p += stream_cnx->stream_rx_object_size;
            length -= stream_cnx->stream_rx_object_size;

            metrics.stream_bytes_recv += stream_cnx->stream_rx_object_size;

            stream_cnx->reset_rx_object();
            length = 0; // no more data left to process
        }
        else {
            // Need to wait for more data, create new object buffer
            stream_cnx->stream_rx_object = new uint8_t[stream_cnx->stream_rx_object_size];

            stream_cnx->stream_rx_object_offset = length;
            std::memcpy(stream_cnx->stream_rx_object, bytes_p, length);
            metrics.stream_bytes_recv += length;
            length = 0; // no more data left to process
        }
    }
    else { // Existing object, append
        size_t remaining_len = stream_cnx->stream_rx_object_size - stream_cnx->stream_rx_object_offset;

        if (remaining_len > length) {
            remaining_len = length;
            length = 0; // no more data left to process

        } else {
            object_complete = true;
            length -= remaining_len;
        }

        metrics.stream_bytes_recv += remaining_len;

        std::memcpy(stream_cnx->stream_rx_object + stream_cnx->stream_rx_object_offset, bytes_p, remaining_len);
        bytes_p += remaining_len;

        if (object_complete) {
            std::vector<uint8_t> data(stream_cnx->stream_rx_object, stream_cnx->stream_rx_object + stream_cnx->stream_rx_object_size);
            stream_cnx->rx_data->push(std::move(data));

            stream_cnx->reset_rx_object();
        }
        else {
            stream_cnx->stream_rx_object_offset += remaining_len;
        }
    }

    if (object_complete) {
        metrics.stream_objects_recv++;

        bool too_many_in_queue = false;
        if (cbNotifyQueue.size() > 200) {
            logger->warning << "on_recv_stream_bytes sid: " << stream_cnx->stream_id
                            << "cbNotifyQueue size" << cbNotifyQueue.size()
                            << std::flush;
        }

        if (too_many_in_queue || stream_cnx->rx_data->size() < 4 || stream_cnx->in_data_cb_skip_count > 30) {
            stream_cnx->in_data_cb_skip_count = 0;
            TransportContextId context_id = stream_cnx->context_id;
            StreamId stream_id = stream_cnx->stream_id;

            cbNotifyQueue.push([=, this]() { delegate.on_recv_notify(context_id, stream_id); });
        } else {
            stream_cnx->in_data_cb_skip_count++;
        }
    }

    if (length > 0) {
        logger->debug << "on_stream_bytes has remaining bytes: " << length << std::flush;
        on_recv_stream_bytes(stream_cnx, bytes_p, length);
    }
}

void PicoQuicTransport::check_conns_for_congestion()
{
    std::lock_guard<std::mutex> _(_state_mutex);

    /*
     * A sign of congestion is when transmit queues are not being serviced (e.g., have a backlog).
     * With no congestion, queues will be close to zero in size.
     *
     * Check each queue size to determine if there is possible congestion
     */

    for (auto& [context_id, streams] : active_streams) {
        int congested_count { 0 };

        for (auto& [stream_id, stream_cnx] : streams) {
            if (stream_cnx.metrics.tx_delayed_callback - stream_cnx.metrics.prev_tx_delayed_callback > 1) {
                congested_count++;
            }

            if (stream_cnx.metrics.tx_delayed_callback) {
                stream_cnx.metrics.prev_tx_delayed_callback = stream_cnx.metrics.tx_delayed_callback;
            }
        }

        auto& conn_ctx = conn_context[context_id];
        if (congested_count && not conn_ctx.is_congested) {
            conn_ctx.is_congested = true;
            logger->debug << "context_id: " << context_id << " has " << congested_count << " streams congested." << std::flush;

        } else if (conn_ctx.is_congested) {
            // No longer congested
            conn_ctx.is_congested = false;
            logger->debug << "context_id: " << context_id << " c_count: " << congested_count << " is no longer congested." << std::flush;
        }
    }

    // TODO: Remove and add metrics per stream if possible
    for (auto& [conn_id, conn_ctx]: conn_context) {
        if (conn_ctx.total_retransmits < conn_ctx.cnx->nb_retransmission_total) {
            logger->debug << "remote: " << conn_ctx.peer_addr_text << " port: " << conn_ctx.peer_port
                         << " context_id: " << conn_id << " retransmits increased, delta: "
                         << (conn_ctx.cnx->nb_retransmission_total - conn_ctx.total_retransmits)
                         << " total: " << conn_ctx.cnx->nb_retransmission_total << std::flush;

            conn_ctx.total_retransmits = conn_ctx.cnx->nb_retransmission_total;
        }
    }

}