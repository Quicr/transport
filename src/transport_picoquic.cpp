#include "transport_picoquic.h"

#include <picoquic_internal.h>
#include <autoqlog.h>
#include <picoquic_utils.h>

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <ctime>
#include <iostream>
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
     * @brief Returns the next stream id depending on if the stream is
     *        initiated by the client or the server, and if it bi- or uni- directional.
     *
     * @param id                    The initial value to adjust
     * @param is_server             Flag if the initiating request is from a server or a client
     * @param is_unidirectional     Flag if the streams are bi- or uni- directional.
     *
     * @return The correctly adjusted stream id value.
     */
    constexpr uint64_t get_next_stream_id(uint64_t last_stream_id, bool is_server, bool is_unidirectional)
    {
        return ((last_stream_id + 4) & (~0x0u << 2)) | (is_server ? 0b01 : 0b00) | (is_unidirectional ? 0b10 : 0b00);
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
    constexpr DataContextId make_datagram_stream_id(bool is_server, bool is_unidirectional)
    {
        return 0;
    }
} // namespace

/* ============================================================================
 * PicoQuic Callbacks
 * ============================================================================
 */

int pq_event_cb(picoquic_cnx_t* pq_cnx,
            uint64_t stream_id,
            uint8_t* bytes,
            size_t length,
            picoquic_call_back_event_t fin_or_event,
            void* callback_ctx,
            void* v_stream_ctx)
{
    PicoQuicTransport* transport = static_cast<PicoQuicTransport*>(callback_ctx);
    PicoQuicTransport::DataContext* data_ctx = static_cast<PicoQuicTransport::DataContext*>(v_stream_ctx);
    const auto conn_id = reinterpret_cast<uint64_t>(pq_cnx);

    bool is_fin = false;

    if (transport == NULL) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (fin_or_event) {

        case picoquic_callback_prepare_datagram: {
            // length is the max allowed data length
            if (picoquic_get_cwin(pq_cnx) < PQ_CC_LOW_CWIN) {        // Congested if less than 8K or near jumbo MTU size
                if (auto conn_ctx = transport->getConnContext(conn_id)) {
                    conn_ctx->metrics.cwin_congested++;
                } else {
                    break;
                }
            }

            data_ctx = transport->getDefaultDataContext(conn_id);
            data_ctx->metrics.dgram_prepare_send++;
            transport->send_next_datagram(data_ctx, bytes, length);
            break;
        }

        case picoquic_callback_datagram_acked:
            //   bytes carries the original packet data
            data_ctx = transport->getDefaultDataContext(conn_id);
//            transport->logger->info << "Got datagram ack send_time: " << stream_id
//                                    << " bytes length: " << length << std::flush;
            data_ctx->metrics.dgram_ack++;
            break;

        case picoquic_callback_datagram_spurious:
            data_ctx = transport->getDefaultDataContext(conn_id);
            data_ctx->metrics.dgram_spurious++;
            break;

        case picoquic_callback_datagram_lost:
            data_ctx = transport->getDefaultDataContext(conn_id);
            data_ctx->metrics.dgram_lost++;
            break;

        case picoquic_callback_datagram: {
            data_ctx = transport->getDefaultDataContext(conn_id);
            data_ctx->metrics.dgram_received++;
            transport->on_recv_datagram(data_ctx, bytes, length);
            break;
        }

        case picoquic_callback_prepare_to_send: {
            if (picoquic_get_cwin(pq_cnx) < PQ_CC_LOW_CWIN) {
                // Congested if less than 8K or near jumbo MTU size
                if (auto conn_ctx = transport->getConnContext(conn_id)) {
                    conn_ctx->metrics.cwin_congested++;
                } else {
                    break;
                }
            }

            if (data_ctx == NULL) {
                // picoquic calls this again even after reset/fin, here we ignore it
                transport->logger->info << "conn_id: " << conn_id << " stream_id: " << stream_id
                                        << " context is null" << std::flush;
                break;
            }

            data_ctx->metrics.stream_prepare_send++;
            transport->send_stream_bytes(data_ctx, bytes, length);
            break;
        }

        case picoquic_callback_stream_fin:
            is_fin = true;
            [[fallthrough]];
        case picoquic_callback_stream_data: {
            if (data_ctx == NULL) {
                if (!((stream_id & 0x2) == 2) /* not unidir stream */) {

                    // Do not create bidir data context if it wasn't initiated by this instance
                    if ( ( (stream_id & 0x1) == 1 && !transport->_is_server_mode)
                        || ( (stream_id & 0x0) == 0 && transport->_is_server_mode)) {

                        // Create the data context for new bidir streams created by remote side
                        data_ctx = transport->createDataContextBiDirRecv(conn_id, stream_id);
                        picoquic_set_app_stream_ctx(pq_cnx, stream_id, data_ctx);

                    } else {
                        // No data context, ignore stream that is not valid
                        break;
                    }
                }
                else {
                    data_ctx = transport->getDefaultDataContext(conn_id);
                    picoquic_set_app_stream_ctx(pq_cnx, stream_id, data_ctx);
                }
            }

            data_ctx->metrics.stream_rx_callbacks++;

            // length is the amount of data received
            if (length) {
                transport->on_recv_stream_bytes(data_ctx, stream_id, bytes, length);
            }

            if (is_fin) {
                transport->logger->info << "Received FIN for stream " << stream_id << std::flush;

                data_ctx->current_stream_id = 0;
                picoquic_reset_stream_ctx(pq_cnx, data_ctx->current_stream_id);

                if (data_ctx->is_default_context) {

                    const auto rx_buf_it = data_ctx->stream_rx_buffer.find(stream_id);
                    if (rx_buf_it != data_ctx->stream_rx_buffer.end()) {
                        if (rx_buf_it->second.object != nullptr) {
                            data_ctx->metrics.rx_buffer_drops++;
                            rx_buf_it->second.reset_buffer();
                        }

                        data_ctx->stream_rx_buffer.erase(rx_buf_it);
                    }

                } else {
                    transport->deleteDataContext(conn_id, data_ctx->data_ctx_id);
                }
            }
            break;
        }

        case picoquic_callback_stream_reset: {
            transport->logger->debug << "Received RESET stream conn_id: " << conn_id
                                    << " stream_id: " << stream_id
                                    << std::flush;

            picoquic_reset_stream_ctx(pq_cnx, stream_id);

            if (data_ctx == NULL) {
                data_ctx = transport->getDefaultDataContext(conn_id);
            }

            data_ctx->current_stream_id = 0;

            const auto rx_buf_it = data_ctx->stream_rx_buffer.find(stream_id);
            if (rx_buf_it != data_ctx->stream_rx_buffer.end()) {
                if (rx_buf_it->second.object != nullptr) {
                    data_ctx->metrics.rx_buffer_drops++;
                    rx_buf_it->second.reset_buffer();
                }

                data_ctx->stream_rx_buffer.erase(rx_buf_it);
            }

            transport->logger->debug << "Received RESET stream; conn_id: " << data_ctx->conn_id
                                    << " data_ctx_id: " << data_ctx->data_ctx_id
                                    << " stream_id: " << stream_id
                                    << " RX buf drops: " << data_ctx->metrics.rx_buffer_drops << std::flush;

            if (!data_ctx->is_default_context) {
                transport->deleteDataContext(conn_id, data_ctx->data_ctx_id);
            }

            break;
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
            const auto cwin_bytes = picoquic_get_cwin(pq_cnx);
            const auto rtt_us = picoquic_get_rtt(pq_cnx);
            picoquic_path_quality_t path_quality;
            picoquic_get_path_quality(pq_cnx, pq_cnx->path[0]->unique_path_id, &path_quality);

            transport->logger->info << "Pacing rate changed; conn_id: " << conn_id
                                    << " rate Kbps: " << stream_id * 8 / 1000
                                    << " cwin_bytes: " << cwin_bytes
                                    << " rtt_us: " << rtt_us
                                    << " rate Kbps: " << path_quality.pacing_rate * 8 / 1000
                                    << " cwin_bytes: " << path_quality.cwin
                                    << " rtt_us: " << path_quality.rtt
                                    << " rtt_max: " << path_quality.rtt_max
                                    << " rtt_sample: " << path_quality.rtt_sample
                                    << " lost_pkts: " << path_quality.lost
                                    << " bytes_in_transit: " << path_quality.bytes_in_transit
                                    << " recv_rate_Kbps: " << path_quality.receive_rate_estimate * 8 / 1000
                                    << std::flush;
            break;
        }

        case picoquic_callback_application_close:
        case picoquic_callback_close: {
            transport->logger->info << "Closing connection conn_id: " << conn_id
                                    << " stream_id: " << stream_id;

            switch (picoquic_get_local_error(pq_cnx)) {
                case PICOQUIC_ERROR_IDLE_TIMEOUT:
                    transport->logger->info << " Idle timeout";
                    break;

                default:
                    transport->logger->info << " local_error: " << picoquic_get_local_error(pq_cnx)
                                            << " remote_error: " << picoquic_get_remote_error(pq_cnx)
                                            << " app_error: " << picoquic_get_application_error(pq_cnx);
            }

            picoquic_set_callback(pq_cnx, NULL, NULL);

            data_ctx = transport->getDefaultDataContext(conn_id);

            if (data_ctx == nullptr) {
                transport->logger->info << std::flush;
                transport->logger->error << "Close conn_id: " << conn_id
                                         << " is missing connection context" << std::flush;
                return 0;
            }

            auto conn_ctx = transport->getConnContext(conn_id);
            if (conn_ctx != nullptr) {
                transport->logger->info << " remote: " << conn_ctx->peer_addr_text;
            }

            transport->logger->info << std::flush;

            transport->close(conn_id);

            if (not transport->_is_server_mode) {
                // TODO: Fix picoquic. Apparently picoquic is not processing return values for this callback
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }

            return 0;
        }

        case picoquic_callback_ready: { // Connection callback, not per stream
            if (transport->_is_server_mode) {
                transport->createConnContext(pq_cnx); // !! Important, must call this before getDefaultDataContext()
                transport->on_new_connection(conn_id);
            }
            else {
                // Client
                transport->setStatus(TransportStatus::Ready);
                transport->on_connection_status(conn_id, TransportStatus::Ready);
            }

            (void)picoquic_mark_datagram_ready(pq_cnx, 1);

            break;
        }

        default:
            LOGGER_DEBUG(transport->logger, "Got event " << fin_or_event);
            break;
    }

    return 0;
}

int pq_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, void* callback_ctx, void* callback_arg)
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

                if (targ->delta_t > PQ_LOOP_MAX_DELAY_US) {
                    targ->delta_t = PQ_LOOP_MAX_DELAY_US;
                }

                if (!transport->pq_loop_prev_time) {
                    transport->pq_loop_prev_time = targ->current_time;
                }

                if (targ->current_time - transport->pq_loop_prev_time > 100000) {

                    transport->check_conns_for_congestion();

                    transport->pq_loop_prev_time = targ->current_time;
                }

                // Stop loop if shutting down
                if (transport->status() == TransportStatus::Shutdown) {
                    transport->logger->Log("picoquic is shutting down");

                    picoquic_cnx_t* close_cnx = picoquic_get_first_cnx(quic);

                    if (close_cnx == NULL) {
                        return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
                    }

                    while (close_cnx != NULL) {
                        transport->logger->info << "Closing connection id " << reinterpret_cast<uint64_t>(close_cnx)
                                                << std::flush;
                        picoquic_close(close_cnx, 0);
                        close_cnx = picoquic_get_next_cnx(close_cnx);
                    }

                    return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
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

/* ============================================================================
 * Public API methods
 * ============================================================================
 */

TransportStatus PicoQuicTransport::status() const
{
    return transportStatus;
}

TransportConnId
PicoQuicTransport::start()
{
    uint64_t current_time = picoquic_current_time();

    if (debug) {
        picoquic_logger = std::make_shared<cantina::Logger>("PQIC", logger);
        debug_set_callback(&PicoQuicTransport::PicoQuicLogging, this);
    }

    if (tconfig.use_reset_wait_strategy) {
        logger->info << "Using Reset and Wait congestion control strategy" << std::flush;
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
    local_tp_options.max_idle_timeout = tconfig.idle_timeout_ms;
    local_tp_options.max_ack_delay = 100000;
    local_tp_options.min_ack_delay = 1000;

    picoquic_set_default_handshake_timeout(quic_ctx, (tconfig.idle_timeout_ms * 1000) / 2);
    picoquic_set_default_tp(quic_ctx, &local_tp_options);
    picoquic_set_default_idle_timeout(quic_ctx, tconfig.idle_timeout_ms);
    picoquic_set_default_priority(quic_ctx, 2);
    picoquic_set_default_datagram_priority(quic_ctx, 1);


    logger->info << "Setting idle timeout to " << tconfig.idle_timeout_ms << "ms" << std::flush;
    //picoquic_set_default_wifi_shadow_rtt(quic_ctx, tconfig.quic_wifi_shadow_rtt_us);
    //logger->info << "Setting wifi shadow RTT to " << tconfig.quic_wifi_shadow_rtt_us << "us" << std::flush;

    picoquic_runner_queue.set_limit(2000);

    cbNotifyQueue.set_limit(2000);
    cbNotifyThread = std::thread(&PicoQuicTransport::cbNotifier, this);

    TransportConnId cid = 0;
    std::ostringstream log_msg;

    if (_is_server_mode) {

        logger->info << "Starting server, listening on " << serverInfo.host_or_ip << ':' << serverInfo.port
                     << std::flush;

        picoQuicThread = std::thread(&PicoQuicTransport::server, this);

    } else {
        logger->info << "Connecting to server " << serverInfo.host_or_ip << ':' << serverInfo.port
                     << std::flush;

        if ((cid = createClient())) {
            picoQuicThread = std::thread(&PicoQuicTransport::client, this, cid);
        }
    }

    return cid;
}

bool PicoQuicTransport::getPeerAddrInfo(const TransportConnId& conn_id,
                                   sockaddr_storage* addr)
{
    std::lock_guard<std::mutex> _(_state_mutex);

    // Locate the specified transport connection context
    auto it = conn_context.find(conn_id);

    // If not found, return false
    if (it == conn_context.end()) return false;

    // Copy the address
    std::memcpy(addr, &it->second.peer_addr, sizeof(sockaddr_storage));

    return true;
}

TransportError
PicoQuicTransport::enqueue(const TransportConnId& conn_id,
                           const DataContextId& data_ctx_id,
                           std::vector<uint8_t>&& bytes,
                           std::vector<qtransport::MethodTraceItem> &&trace,
                           const uint8_t priority,
                           const uint32_t ttl_ms,
                           [[maybe_unused]] const uint32_t delay_ms,
                           const EnqueueFlags flags)
{
    if (bytes.empty()) {
        logger->error << "enqueue dropped due bytes empty, conn_id: " << conn_id
                      << " data_ctx_id: " << data_ctx_id
                      << std::flush;
        return TransportError::None;
    }

    trace.push_back({"transport_quic:enqueue", trace.front().start_time});

    std::lock_guard<std::mutex> _(_state_mutex);

    trace.push_back({"transport_quic:enqueue:afterLock", trace.front().start_time});

    const auto conn_ctx_it = conn_context.find(conn_id);
    if (conn_ctx_it == conn_context.end()) {
        return TransportError::InvalidConnContextId;
    }

    if (!data_ctx_id) { // Default data context (using datagram)
        conn_ctx_it->second.default_data_context.metrics.enqueued_objs++;

        if (flags.clear_tx_queue) {
            conn_ctx_it->second.default_data_context.tx_data->clear();
        }

        ConnData cd { conn_id,
                      data_ctx_id,
                      priority,
                      std::move(bytes),
                      std::move(trace)};

        conn_ctx_it->second.default_data_context.tx_data->push(std::move(cd), ttl_ms, priority, 0);

        if (!conn_ctx_it->second.default_data_context.mark_dgram_ready) {
            conn_ctx_it->second.default_data_context.mark_dgram_ready = true;

            picoquic_runner_queue.push([=]() {
                mark_dgram_ready(conn_id);
            });
        }

    }
    else { // is stream
        const auto data_ctx_it = conn_ctx_it->second.active_data_contexts.find(data_ctx_id);
        if (data_ctx_it == conn_ctx_it->second.active_data_contexts.end()) {
            return TransportError::InvalidDataContextId;
        }

        data_ctx_it->second.metrics.enqueued_objs++;

        if (flags.new_stream) {
            if (flags.use_reset) {
                data_ctx_it->second.stream_action = DataContext::StreamAction::REPLACE_STREAM_USE_RESET;
            } else {
                data_ctx_it->second.stream_action = DataContext::StreamAction::REPLACE_STREAM_USE_FIN;
            }
        }

        if (flags.clear_tx_queue) {
            data_ctx_it->second.metrics.tx_queue_discards += data_ctx_it->second.tx_data->size();
            data_ctx_it->second.tx_data->clear();
        }

        ConnData cd { conn_id,
                      data_ctx_id,
                      priority,
                      std::move(bytes),
                      std::move(trace)};

        data_ctx_it->second.tx_data->push(std::move(cd), ttl_ms, priority, 0);

        if (! data_ctx_it->second.mark_stream_active) {
            data_ctx_it->second.mark_stream_active = true;

            picoquic_runner_queue.push([=]() {
                mark_stream_active(conn_id, data_ctx_id);
            });
        }
    }

    return TransportError::None;
}

std::optional<std::vector<uint8_t>>
PicoQuicTransport::dequeue(const TransportConnId& conn_id, const DataContextId& data_ctx_id)
{
    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_ctx_it = conn_context.find(conn_id);
    if (conn_ctx_it == conn_context.end()) {
        return std::nullopt;
    }

    if (!data_ctx_id) { // Default data context
        if (auto cd = conn_ctx_it->second.default_data_context.rx_data->pop()) {
            return std::move(cd->data);
        }
        return std::nullopt;
    }

    const auto data_ctx_it = conn_ctx_it->second.active_data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_ctx_it->second.active_data_contexts.end()) {
        return std::nullopt;
    }

    if (auto cd = data_ctx_it->second.rx_data->pop()) {
        return std::move(cd->data);
    }
    return std::nullopt;
}

DataContextId
PicoQuicTransport::createDataContext(const TransportConnId conn_id,
                                     bool use_reliable_transport,
                                     uint8_t priority, bool bidir)
{
    std::lock_guard<std::mutex> _(_state_mutex);

    if (priority > 127) {
        /*
         * Picoquic most significant bit of priority indicates to use round-robin. We don't want
         *      to use round-robin of same priorities right now.
         */
        throw std::runtime_error("Create stream priority cannot be greater than 127, range is 0 - 127");
    }

    const auto conn_it = conn_context.find(conn_id);
    if (conn_it == conn_context.end()) {
        logger->error << "Invalid conn_id: " << conn_id << ", cannot create data context" << std::flush;
        return 0;
    }

    if (not use_reliable_transport) {
        return 0; // Default data context, which is datagram
    }

    const auto [data_ctx_it, is_new] = conn_it->second.active_data_contexts.emplace(conn_it->second.next_data_ctx_id,
                                                                                    DataContext{});

    if (is_new) {
        // Init context
        data_ctx_it->second.conn_id = conn_id;
        data_ctx_it->second.is_bidir = bidir;
        data_ctx_it->second.data_ctx_id = conn_it->second.next_data_ctx_id++; // Set and bump next data_ctx_id

        data_ctx_it->second.priority = priority;

        data_ctx_it->second.rx_data = std::make_unique<safe_queue<ConnData>>(tconfig.time_queue_rx_size);
        data_ctx_it->second.tx_data = std::make_unique<priority_queue<ConnData>>(tconfig.time_queue_max_duration,
                                                                                tconfig.time_queue_bucket_interval,
                                                                                _tick_service,
                                                                                tconfig.time_queue_init_queue_size);

        // Create stream
        if (use_reliable_transport) {
            create_stream(conn_it->second, &data_ctx_it->second);
        }
    }

    return data_ctx_it->second.data_ctx_id;
}

void
PicoQuicTransport::close(const TransportConnId& conn_id)
{
    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_it = conn_context.find(conn_id);

    if (conn_it == conn_context.end())
        return;

    // Only one datagram context is per connection, if it's deleted, then the connection is to be terminated
    on_connection_status(conn_id, TransportStatus::Disconnected);

    // Remove pointer references in picoquic for active streams
    for (const auto& [d_id, d_ctx]: conn_it->second.active_data_contexts) {
        picoquic_mark_active_stream(conn_it->second.pq_cnx, d_ctx.current_stream_id, 0, NULL);
        picoquic_reset_stream(conn_it->second.pq_cnx, d_ctx.current_stream_id, 0);
    }

    picoquic_close(conn_it->second.pq_cnx, 0);

    if (not _is_server_mode) {
        setStatus(TransportStatus::Shutdown);
    }

    conn_context.erase(conn_it);
}

/* ============================================================================
 * Public internal methods used by picoquic
 * ============================================================================
 */

PicoQuicTransport::DataContext*
PicoQuicTransport::getDefaultDataContext(TransportConnId conn_id)
{
    // Locate the specified transport connection context
    auto it = conn_context.find(conn_id);

    if (it == conn_context.end()) {
        return nullptr;
    }

    return &it->second.default_data_context;
}

PicoQuicTransport::ConnectionContext* PicoQuicTransport::getConnContext(const TransportConnId& conn_id)
{
    // Locate the specified transport connection context
    auto it = conn_context.find(conn_id);

    // If not found, return empty context
    if (it == conn_context.end()) return nullptr;

    return &it->second;
}


PicoQuicTransport::ConnectionContext& PicoQuicTransport::createConnContext(picoquic_cnx_t * pq_cnx)
{

    auto [conn_it, is_new] = conn_context.emplace(reinterpret_cast<TransportConnId>(pq_cnx), pq_cnx);

    sockaddr* addr;

    auto &conn_ctx = conn_it->second;
    conn_ctx.conn_id = reinterpret_cast<TransportConnId>(pq_cnx);
    conn_ctx.pq_cnx = pq_cnx;

    picoquic_get_peer_addr(pq_cnx, &addr);
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

    if (is_new || !conn_ctx.default_data_context.is_default_context) {
        logger->info << "Created new connection context for conn_id: " << conn_ctx.conn_id << std::flush;

        // Setup default data context
        conn_ctx.default_data_context.is_default_context = true;
        conn_ctx.default_data_context.conn_id = conn_ctx.conn_id;
        conn_ctx.default_data_context.priority = 1;
        conn_ctx.default_data_context.rx_data = std::make_unique<safe_queue<ConnData>>(tconfig.time_queue_rx_size);

        conn_ctx.default_data_context.tx_data = std::make_unique<priority_queue<ConnData>>(
          tconfig.time_queue_max_duration, tconfig.time_queue_bucket_interval, _tick_service,
          tconfig.time_queue_init_queue_size);
    }

    return conn_ctx;
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
PicoQuicTransport::setStatus(TransportStatus status)
{
    transportStatus = status;
}

PicoQuicTransport::DataContext* PicoQuicTransport::createDataContextBiDirRecv(TransportConnId conn_id, uint64_t stream_id)
{
    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_it = conn_context.find(conn_id);
    if (conn_it == conn_context.end()) {
        logger->error << "Invalid conn_id: " << conn_id << ", cannot create data context" << std::flush;
        return nullptr;
    }

    const auto [data_ctx_it, is_new] = conn_it->second.active_data_contexts.emplace(conn_it->second.next_data_ctx_id,
                                                                                    DataContext{});

    if (is_new) {
        // Init context
        data_ctx_it->second.conn_id = conn_id;
        data_ctx_it->second.is_bidir = true;
        data_ctx_it->second.data_ctx_id = conn_it->second.next_data_ctx_id++; // Set and bump next data_ctx_id

        data_ctx_it->second.priority = 10; // TODO: Need to get priority from remote

        data_ctx_it->second.rx_data = std::make_unique<safe_queue<ConnData>>(tconfig.time_queue_rx_size);
        data_ctx_it->second.tx_data = std::make_unique<priority_queue<ConnData>>(tconfig.time_queue_max_duration,
                                                                                tconfig.time_queue_bucket_interval,
                                                                                _tick_service,
                                                                                tconfig.time_queue_init_queue_size);

        data_ctx_it->second.current_stream_id = stream_id;

        cbNotifyQueue.push([=, data_ctx_id = data_ctx_it->second.data_ctx_id, this]() {
            delegate.on_new_data_context(conn_id, data_ctx_id); });

        logger->info << "Created new bidir data context conn_id: " << conn_id
                     << " data_ctx_id: " << data_ctx_it->second.data_ctx_id
                     << " stream_id: " << stream_id
                     << std::flush;

        return &data_ctx_it->second;
    }

    return nullptr;
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

void PicoQuicTransport::deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id)
{
    if (data_ctx_id == 0) {
        return; // use close() instead of deleting default/datagram context
    }

    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_it = conn_context.find(conn_id);

    if (conn_it == conn_context.end())
        return;

    logger->info << "Delete data context " << data_ctx_id
                 << " in conn_id: " << conn_id
                 << std::flush;

    const auto data_ctx_it = conn_it->second.active_data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_it->second.active_data_contexts.end())
        return;

    close_stream(conn_it->second, &data_ctx_it->second, false);

    conn_it->second.active_data_contexts.erase(data_ctx_it);
}

void
PicoQuicTransport::send_next_datagram(DataContext* data_ctx, uint8_t* bytes_ctx, size_t max_len)
{
    if (bytes_ctx == NULL) {
        return;
    }

    if (data_ctx->current_stream_id != 0) {
        // Only for datagrams
        return;
    }

    check_callback_delta(data_ctx);

    auto out_data = data_ctx->tx_data->front();
    if (out_data.has_value) {
        if (out_data.value.data.size() == 0) {
            logger->error << "conn_id: " << data_ctx->conn_id
                          << " data_ctx_id: " << data_ctx->data_ctx_id
                          << " priority: " << static_cast<int>(data_ctx->priority)
                          << " has ZERO data size"
                          << std::flush;

            data_ctx->tx_data->pop();
            return;
        }

        data_ctx->metrics.tx_queue_expired += out_data.expired_count;

        if (max_len >= out_data.value.data.size()) {
            data_ctx->tx_data->pop();

            out_data.value.trace.push_back({"transport_quic:send_dgram", out_data.value.trace.front().start_time});

            if (out_data.value.trace.back().delta > 60000) {
                logger->info << "MethodTrace conn_id: " << data_ctx->conn_id
                             << " data_ctx_id: " << data_ctx->data_ctx_id
                             << " priority: " << static_cast<int>(out_data.value.priority);
                for (const auto &ti: out_data.value.trace) {
                    logger->info << " " << ti.method << ": " << ti.delta << " ";
                }

                logger->info << " total_duration: " << out_data.value.trace.back().delta << std::flush;
            }

            data_ctx->metrics.dgram_sent++;

            uint8_t* buf = NULL;

            buf = picoquic_provide_datagram_buffer_ex(bytes_ctx,
                                                      out_data.value.data.size(),
                                                      data_ctx->tx_data->empty() ? picoquic_datagram_not_active : picoquic_datagram_active_any_path);

            if (buf != NULL) {
                std::memcpy(buf, out_data.value.data.data(), out_data.value.data.size());
            }
        }
        else {
            picoquic_runner_queue.push([=]() {
                mark_dgram_ready(data_ctx->conn_id);
            });

            /* TODO(tievens): picoquic_prepare_stream_and_datagrams() appears to ignore the
             *     below unless data was sent/provided
             */
            picoquic_provide_datagram_buffer_ex(bytes_ctx, 0, picoquic_datagram_active_any_path);
        }
    }
    else {
        picoquic_provide_datagram_buffer_ex(bytes_ctx, 0, picoquic_datagram_not_active);
    }
}

void
PicoQuicTransport::send_stream_bytes(DataContext* data_ctx, uint8_t* bytes_ctx, size_t max_len)
{
    if (bytes_ctx == NULL) {
        return;
    }

    if (data_ctx->is_default_context) {
        return; // Only for steams
    }

    check_callback_delta(data_ctx);

    uint32_t data_len = 0;                                  /// Length of data to follow the 4 byte length
    size_t offset = 0;
    int is_still_active = 0;

    switch (data_ctx->stream_action) {
        case DataContext::StreamAction::NO_ACTION:
            [[fallthrough]];
        default:
            if (data_ctx->current_stream_id == 0) {
                logger->info << "Creating unset stream in conn_id: " << data_ctx->conn_id
                             << std::flush;
                const auto conn_ctx = getConnContext(data_ctx->conn_id);
                create_stream(*conn_ctx, data_ctx);

                return;
            }
            break;

        case DataContext::StreamAction::REPLACE_STREAM_USE_RESET: {
            data_ctx->uses_reset_wait = true;

            std::lock_guard<std::mutex> _(_state_mutex);
            const auto conn_ctx = getConnContext(data_ctx->conn_id);

            // Keep stream in discard mode if still congested
            if (conn_ctx->is_congested && data_ctx->tx_reset_wait_discard) {
                break;
            }

            if (data_ctx->stream_tx_object != nullptr) {
                data_ctx->metrics.tx_buffer_drops++;
            }

            const auto existing_stream_id = data_ctx->current_stream_id;

            close_stream(*conn_ctx, data_ctx, true);

            logger->debug << "Replacing stream using RESET; conn_id: " << data_ctx->conn_id
                         << " data_ctx_id: " << data_ctx->data_ctx_id
                         << " existing_stream: " << existing_stream_id
                         << " write buf drops: " << data_ctx->metrics.tx_buffer_drops
                         << " tx_queue_discards: " << data_ctx->metrics.tx_queue_discards
                         << std::flush;

            create_stream(*conn_ctx, data_ctx);
            data_ctx->stream_action = DataContext::StreamAction::NO_ACTION;

            if (!conn_ctx->is_congested) { // Only clear reset wait if not congested
                data_ctx->tx_reset_wait_discard = false;                // Allow new object to be sent
            }

            data_ctx->mark_stream_active = false;
            return; // New stream requires PQ to callback again using that stream
        }

        case DataContext::StreamAction::REPLACE_STREAM_USE_FIN: {
            data_ctx->uses_reset_wait = true;
            if (data_ctx->stream_tx_object == nullptr) {
                logger->info << "Replacing stream using FIN; conn_id: " << data_ctx->conn_id
                             << " existing_stream: " << data_ctx->current_stream_id << std::flush;

                std::lock_guard<std::mutex> _(_state_mutex);

                const auto conn_ctx = getConnContext(data_ctx->conn_id);
                create_stream(*conn_ctx, data_ctx);

                data_ctx->stream_action = DataContext::StreamAction::NO_ACTION;
            }

            break; // Continue with this object since this is a FIN
        }
    }

    if (data_ctx->tx_reset_wait_discard) {      // Drop TX objects till next reset/new stream
        auto obj = data_ctx->tx_data->pop_front();
        if (obj.has_value) {
            data_ctx->metrics.tx_queue_discards++;

            picoquic_runner_queue.push([=]() {
                mark_stream_active(data_ctx->conn_id, data_ctx->data_ctx_id);
            });
        }

        data_ctx->mark_stream_active = false;
        return;
    }

    if (data_ctx->stream_tx_object == nullptr) {

        if (max_len < 5) {
            // Not enough bytes to send
            logger->debug << "Not enough bytes to send stream size header, waiting for next callback. "
                          << " conn_id: " << data_ctx->conn_id
                          << " data_ctx_id: " << data_ctx->data_ctx_id
                          << " stream_id: " << data_ctx->current_stream_id
                          << " priority: " << static_cast<int>(data_ctx->priority)
                         << data_ctx->current_stream_id << std::flush;

            if (!data_ctx->tx_data->empty()) {
                data_ctx->mark_stream_active = true;
                picoquic_runner_queue.push([=]() {
                    mark_stream_active(data_ctx->conn_id, data_ctx->data_ctx_id);
                });
            }

            return;
        }

        auto obj = data_ctx->tx_data->pop_front();

        if (obj.has_value) {
            if (obj.value.data.size() == 0) {
                logger->error << "conn_id: " << data_ctx->conn_id
                              << " data_ctx_id: " << data_ctx->data_ctx_id
                              << " priority: " << static_cast<int>(data_ctx->priority)
                              << " stream has ZERO data size"
                              << std::flush;
                return;
            }


            data_ctx->metrics.stream_objects_sent++;

            obj.value.trace.push_back({"transport_quic:send_stream", obj.value.trace.front().start_time});

            if (!obj.value.trace.empty() && obj.value.trace.back().delta > 15000) {
                logger->info << "MethodTrace conn_id: " << data_ctx->conn_id
                             << " data_ctx_id: " << data_ctx->data_ctx_id
                             << " priority: " << static_cast<int>(obj.value.priority);
                for (const auto &ti: obj.value.trace) {
                    logger->info << " " << ti.method << ": " << ti.delta << " ";
                }

                logger->info << " total_duration: " << obj.value.trace.back().delta << std::flush;
            }

            max_len -= 4; // Subtract out the length header that will be added

            data_ctx->stream_tx_object = new uint8_t[obj.value.data.size()];
            data_ctx->stream_tx_object_size = obj.value.data.size();
            std::memcpy(data_ctx->stream_tx_object, obj.value.data.data(), obj.value.data.size());

            if (obj.value.data.size() > max_len) {
                data_ctx->stream_tx_object_offset = max_len;
                data_len = max_len;
                is_still_active = 1;

            } else {
                data_len = obj.value.data.size();
                data_ctx->stream_tx_object_offset = 0;
            }

        } else {
            // Queue is empty
            picoquic_provide_stream_data_buffer(bytes_ctx,
                                                0,
                                                0,
                                                is_still_active);

            return;
        }
    }
    else { // Have existing object with remaining bytes to send.
        data_len = data_ctx->stream_tx_object_size - data_ctx->stream_tx_object_offset;
        offset = data_ctx->stream_tx_object_offset;

        if (data_len > max_len) {
            data_ctx->stream_tx_object_offset += max_len;
            data_len = max_len;
            is_still_active = 1;

        } else {
            data_ctx->stream_tx_object_offset = 0;
        }
    }

    data_ctx->metrics.stream_bytes_sent += data_len;

    if (!is_still_active && !data_ctx->tx_data->empty())
        is_still_active = 1;

    uint8_t *buf = nullptr;

    if (offset == 0) {
        // New object being written, add length (4 bytes), little endian for now
        uint32_t obj_length = data_ctx->stream_tx_object_size;

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
    std::memcpy(buf, data_ctx->stream_tx_object + offset, data_len);

    if (data_ctx->stream_tx_object_offset == 0 && data_ctx->stream_tx_object != nullptr) {
        // Zero offset at this point means the object was fully sent
        data_ctx->reset_tx_object();
    }
}

void
PicoQuicTransport::on_connection_status(const TransportConnId conn_id, const TransportStatus status)
{
    if (status == TransportStatus::Ready) {
        auto conn_ctx = getConnContext(conn_id);
        logger->info << "Connection established to server "
                     << conn_ctx->peer_addr_text
                     << std::flush;

    }

    cbNotifyQueue.push([=, this]() { delegate.on_connection_status(conn_id, status); });
}

void
PicoQuicTransport::on_new_connection(const TransportConnId conn_id)
{
    auto conn_ctx = getConnContext(conn_id);
    logger->info << "New Connection " << conn_ctx->peer_addr_text << " port: " << conn_ctx->peer_port
                 << " conn_id: " << conn_id
                 << std::flush;

//    picoquic_subscribe_pacing_rate_updates(conn_ctx->pq_cnx, tconfig.pacing_decrease_threshold_Bps,
//                                           tconfig.pacing_increase_threshold_Bps);

    TransportRemote remote{ .host_or_ip = conn_ctx->peer_addr_text,
                            .port = conn_ctx->peer_port,
                            .proto = TransportProtocol::QUIC };

    picoquic_enable_keep_alive(conn_ctx->pq_cnx, tconfig.idle_timeout_ms * 500);

    cbNotifyQueue.push([=, this]() { delegate.on_new_connection(conn_id, remote); });
}

void
PicoQuicTransport::on_recv_datagram(DataContext* data_ctx, uint8_t* bytes, size_t length)
{
    if (data_ctx == NULL || length == 0) {
        logger->Log(cantina::LogLevel::Debug, "On receive datagram has null context");
        return;
    }

    std::vector<uint8_t> data(bytes, bytes + length);

    std::vector<MethodTraceItem> trace;
    const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

    trace.push_back({"transport_quic:recv_dgram", start_time});

    ConnData cd { data_ctx->conn_id, data_ctx->data_ctx_id, data_ctx->priority, std::move(data), std::move(trace)};
    if (!data_ctx->rx_data->push(std::move(cd))) {
        logger->error << "conn_id: " << data_ctx->conn_id
                      << " data_ctx_id: " << data_ctx->data_ctx_id
                      << " RX datagram queue is full" << std::flush;
    }

    if (cbNotifyQueue.size() > 100) {
        logger->info << "on_recv_datagram cbNotifyQueue size"
                     << cbNotifyQueue.size() << std::flush;
    }


    if (data_ctx->rx_data->size() < 10 || data_ctx->in_data_cb_skip_count > 20) {
        data_ctx->in_data_cb_skip_count = 0;

        if (!cbNotifyQueue.push([=, this]() { delegate.on_recv_notify(data_ctx->conn_id, data_ctx->current_stream_id, true); })) {

            logger->error << "conn_id: " << data_ctx->conn_id
                          << " data_ctx_id: " << data_ctx->data_ctx_id
                          << " notify queue is full" << std::flush;
        }
    } else {
        data_ctx->in_data_cb_skip_count++;
    }
}

void PicoQuicTransport::on_recv_stream_bytes(DataContext* data_ctx, uint64_t stream_id, uint8_t* bytes, size_t length)
{
    uint8_t *bytes_p = bytes;

    if (data_ctx == NULL || length == 0) {
        logger->Log(cantina::LogLevel::Debug, "on_recv_stream_bytes has null context");
        return;
    }

    auto rx_buf_it = data_ctx->stream_rx_buffer.find(stream_id);
    if (rx_buf_it == data_ctx->stream_rx_buffer.end()) {
        logger->debug << "Adding received conn_id: " << data_ctx->conn_id
                     << " data_ctx_id: " << data_ctx->data_ctx_id
                     << " stream_id: " << stream_id
                     << " into RX buffer" << std::flush;

        auto [it, _] = data_ctx->stream_rx_buffer.try_emplace(stream_id);
        rx_buf_it = it;
    }

    auto &rx_buf = rx_buf_it->second;

    bool object_complete = false;

    if (rx_buf.object == nullptr) {

        if (rx_buf.object_hdr_size < 4) {

            uint16_t len_to_copy = length > 4 ? 4 - rx_buf.object_hdr_size : length - rx_buf.object_hdr_size;


            std::memcpy(&rx_buf.object_size + rx_buf.object_hdr_size,
                        bytes_p, len_to_copy);

            rx_buf.object_hdr_size += len_to_copy;

            if (rx_buf.object_hdr_size < 4) {
                logger->debug << "Stream header not complete. hdr " << rx_buf.object_hdr_size
                              << " conn_id: " << data_ctx->conn_id
                              << " data_ctx_id: " << data_ctx->data_ctx_id
                              << " len_to_copy: " << len_to_copy
                              << " length: " << length
                              << std::flush;
            }

            length -= len_to_copy;
            bytes_p += len_to_copy;


            if (length == 0 || rx_buf.object_hdr_size < 4) {
                // Either no data left to read or not enough data for the header (length) value
                return;
            }
        }

        if (rx_buf.object_size > 40000000L) { // Safety check
            logger->warning << "on_recv_stream_bytes stream_id: " << stream_id
                            << " conn_id: " << data_ctx->conn_id
                            << " data_ctx_id: " << data_ctx->data_ctx_id
                            << " data length is too large: " << rx_buf.object_size
                            << std::flush;

            rx_buf.reset_buffer();
            // TODO: Should reset stream in this case
            return;
        }

        if (rx_buf.object_size <= length) {
            object_complete = true;

            std::vector<uint8_t> data(bytes_p, bytes_p + rx_buf.object_size);

            std::vector<MethodTraceItem> trace;
            const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

            trace.push_back({"transport_quic:recv_stream", start_time});

            ConnData cd { data_ctx->conn_id, data_ctx->data_ctx_id, data_ctx->priority, std::move(data), std::move(trace)};

            data_ctx->rx_data->push(std::move(cd));

            bytes_p += rx_buf.object_size;
            length -= rx_buf.object_size;

            data_ctx->metrics.stream_bytes_recv += rx_buf.object_size;

            rx_buf.reset_buffer();
            length = 0; // no more data left to process
        }
        else {
            // Need to wait for more data, create new object buffer
            rx_buf.object = new uint8_t[rx_buf.object_size];

            rx_buf.object_offset = length;
            std::memcpy(rx_buf.object, bytes_p, length);
            data_ctx->metrics.stream_bytes_recv += length;
            length = 0; // no more data left to process
        }
    }
    else { // Existing object, append
        size_t remaining_len = rx_buf.object_size - rx_buf.object_offset;

        if (remaining_len > length) {
            remaining_len = length;
            length = 0; // no more data left to process

        } else {
            object_complete = true;
            length -= remaining_len;
        }

        data_ctx->metrics.stream_bytes_recv += remaining_len;

        std::memcpy(rx_buf.object + rx_buf.object_offset, bytes_p, remaining_len);
        bytes_p += remaining_len;

        if (object_complete) {
            std::vector<uint8_t> data(rx_buf.object,
                                      rx_buf.object + rx_buf.object_size);

            std::vector<MethodTraceItem> trace;
            const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

            trace.push_back({"transport_quic:recv_stream", start_time});

            ConnData cd { data_ctx->conn_id, data_ctx->data_ctx_id, data_ctx->priority, std::move(data), std::move(trace)};

            data_ctx->rx_data->push(std::move(cd));

            rx_buf.reset_buffer();
        }
        else {
            rx_buf.object_offset += remaining_len;
        }
    }

    if (object_complete) {
        data_ctx->metrics.stream_objects_recv++;

        if (cbNotifyQueue.size() > 150) {
            logger->warning << "on_recv_stream_bytes "
                            << " conn_id: " << data_ctx->conn_id
                            << " data_ctx_id: " << data_ctx->data_ctx_id
                            << "cbNotifyQueue size" << cbNotifyQueue.size()
                            << std::flush;
        }

        if (data_ctx->rx_data->size() < 4 || data_ctx->in_data_cb_skip_count > 30) {
            data_ctx->in_data_cb_skip_count = 0;

            if (!cbNotifyQueue.push([=, this]() { delegate.on_recv_notify(data_ctx->conn_id,
                                                                          data_ctx->data_ctx_id, data_ctx->is_bidir); })) {
                logger->error << "conn_id: " << data_ctx->conn_id
                              << " data_ctx_id: " << data_ctx->data_ctx_id
                              << " notify queue is full" << std::flush;

            }
        } else {
            data_ctx->in_data_cb_skip_count++;
        }
    }

    if (length > 0) {
        logger->debug << "on_recv_stream_bytes has remaining bytes: " << length
                      << " conn_id: " << data_ctx->conn_id
                      << " data_ctx_id: " << data_ctx->data_ctx_id
                      << " stream_id: " << data_ctx->current_stream_id
                      << " rx_hdr_sz: " << rx_buf.object_hdr_size
                      << " rx_obj_offset: " << rx_buf.object_offset
                      << " rx_obj_sz: " << rx_buf.object_size
                      << std::flush;

        on_recv_stream_bytes(data_ctx, stream_id, bytes_p, length);
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

    for (auto& [conn_id, conn_ctx] : conn_context) {
        int congested_count { 0 };
        uint16_t cwin_congested_count = conn_ctx.metrics.cwin_congested - conn_ctx.metrics.prev_cwin_congested;

        picoquic_path_quality_t path_quality;
        picoquic_get_path_quality(conn_ctx.pq_cnx, conn_ctx.pq_cnx->path[0]->unique_path_id, &path_quality);

        // Is CWIN congested
        if (cwin_congested_count > 5 || (path_quality.cwin < PQ_CC_LOW_CWIN && path_quality.bytes_in_transit > 1)) {

            logger->info << "CC: CWIN congested (fyi only)"
                         << " conn_id: " << conn_id
                         << " cwin_congested_count: " << cwin_congested_count
                         << " rate Kbps: " << path_quality.pacing_rate * 8 / 1000
                         << " cwin_bytes: " << path_quality.cwin
                         << " rtt_us: " << path_quality.rtt
                         << " rtt_max: " << path_quality.rtt_max
                         << " rtt_sample: " << path_quality.rtt_sample
                         << " lost_pkts: " << path_quality.lost
                         << " bytes_in_transit: " << path_quality.bytes_in_transit
                         << " recv_rate_Kbps: " << path_quality.receive_rate_estimate * 8 / 1000
                         << std::flush;

            //congested_count++; /* TODO(tievens): DO NOT react to this right now, causing issue with low latency wired networks */
        }
        conn_ctx.metrics.prev_cwin_congested = conn_ctx.metrics.cwin_congested;

        conn_ctx.default_data_context.metrics.prev_tx_delayed_callback = conn_ctx.default_data_context.metrics.tx_delayed_callback;

        // All other data flows (streams)
        uint64_t reset_wait_data_ctx_id {0};       // Positive value indicates the data_ctx_id that can be set to reset_wait

        for (auto& [data_ctx_id, data_ctx] : conn_ctx.active_data_contexts) {

            // Skip context that is in reset and wait
            if (data_ctx.tx_reset_wait_discard) {
                continue;
            }

            // Don't include control stream in delayed callbacks check. Control stream should be priority 0 or 1
            if (data_ctx.priority >= 2
                    && data_ctx.metrics.tx_delayed_callback - data_ctx.metrics.prev_tx_delayed_callback > 1) {
                logger->info << "CC: Stream congested,  callback count greater than 1"
                             << " conn_id: " << data_ctx.conn_id
                             << " data_ctx_id: " << data_ctx.data_ctx_id
                             << " tx_data_queue: " << data_ctx.tx_data->size()
                             << " congested_callbacks: " << data_ctx.metrics.tx_delayed_callback - data_ctx.metrics.prev_tx_delayed_callback
                             << std::flush;
                congested_count++;
            }
            data_ctx.metrics.prev_tx_delayed_callback = data_ctx.metrics.tx_delayed_callback;

            if (data_ctx.tx_data->size() >= 10) {
                logger->info << "CC: Stream congested, queue backlog"
                             << " conn_id: " << data_ctx.conn_id
                             << " data_ctx_id: " << data_ctx.data_ctx_id
                             << " tx_data_queue: " << data_ctx.tx_data->size()
                             << std::flush;
                congested_count++;
            }

            if (data_ctx.priority >= PQ_REST_WAIT_MIN_PRIORITY
                && data_ctx.uses_reset_wait
                && reset_wait_data_ctx_id == 0
                && !data_ctx.tx_reset_wait_discard) {

                reset_wait_data_ctx_id = data_ctx_id;
            }
        }

        if (cwin_congested_count && conn_ctx.pq_cnx->nb_retransmission_total - conn_ctx.metrics.total_retransmits  > 2) {
            logger->info << "CC: remote: " << conn_ctx.peer_addr_text << " port: " << conn_ctx.peer_port
                         << " conn_id: " << conn_id << " retransmits increased, delta: "
                         << (conn_ctx.pq_cnx->nb_retransmission_total - conn_ctx.metrics.total_retransmits)
                         << " total: " << conn_ctx.pq_cnx->nb_retransmission_total << std::flush;

            conn_ctx.metrics.total_retransmits = conn_ctx.pq_cnx->nb_retransmission_total;
            congested_count++;
        }

        // Act on congested
        if (congested_count) {
            conn_ctx.is_congested = true;
            logger->info << "CC: conn_id: " << conn_id << " has streams congested."
                         << " congested_count: " << congested_count
                         << " retrans: " << conn_ctx.metrics.total_retransmits
                         << " cwin_congested: " << conn_ctx.metrics.cwin_congested
                         << std::flush;

            if (tconfig.use_reset_wait_strategy && reset_wait_data_ctx_id > 0) {
                auto& data_ctx = conn_ctx.active_data_contexts[reset_wait_data_ctx_id];
                logger->info << "CC: conn_id: " << conn_id << " setting reset and wait to "
                             << " data_ctx_id: " << reset_wait_data_ctx_id
                             << " priority: " << static_cast<int>(data_ctx.priority)
                             << std::flush;

                data_ctx.tx_reset_wait_discard = true;
                data_ctx.metrics.tx_reset_wait++;

                /*
                 * TODO(tievens) Submit an issue with picoquic to add an API to flush the stream of any
                 *      data stuck in retransmission or waiting for acks
                 */
                //close_stream(conn_ctx, &data_ctx, true);
            }


        } else if (conn_ctx.is_congested) {

            if (conn_ctx.not_congested_gauge > 4) {
                // No longer congested
                conn_ctx.is_congested = false;
                conn_ctx.not_congested_gauge = 0;
                logger->info << "CC: conn_id: " << conn_id << " congested_count: " << congested_count << " is no longer congested."
                             << std::flush;
            } else {
                conn_ctx.not_congested_gauge++;
            }
        }

    }
}

/* ============================================================================
 * Private methods
 * ============================================================================
 */
void PicoQuicTransport::server()
{
    int ret = picoquic_packet_loop(quic_ctx, serverInfo.port, PF_UNSPEC, 0, 2000000, 0, pq_loop_cb, this);

    if (quic_ctx != NULL) {
        picoquic_free(quic_ctx);
        quic_ctx = NULL;
    }

    logger->info << "picoquic packet loop ended with " << ret << std::flush;

    setStatus(TransportStatus::Shutdown);
}

TransportConnId PicoQuicTransport::createClient()
{
    struct sockaddr_storage server_address;
    char const* sni = "cisco.webex.com";
    int ret;

    int is_name = 0;

    ret = picoquic_get_server_address(serverInfo.host_or_ip.c_str(), serverInfo.port, &server_address, &is_name);
    if (ret != 0 || server_address.ss_family == 0) {
        logger->error << "Failed to get server: " << serverInfo.host_or_ip << " port: " << serverInfo.port
                      << std::flush;
        setStatus(TransportStatus::Disconnected);
        return 0;
    } else if (is_name) {
        sni = serverInfo.host_or_ip.c_str();
    }

    picoquic_set_default_congestion_algorithm(quic_ctx, picoquic_bbr_algorithm);

    uint64_t current_time = picoquic_current_time();

    /* TODO: Instead of using debug, change to client/server config of the directory
    if (debug) {
        picoquic_set_qlog(quic_ctx, ".");
    }
    */

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

//    picoquic_subscribe_pacing_rate_updates(cnx, tconfig.pacing_decrease_threshold_Bps,
//                                           tconfig.pacing_increase_threshold_Bps);

    auto &_ = createConnContext(cnx);

    return reinterpret_cast<uint64_t>(cnx);
}

void PicoQuicTransport::client(const TransportConnId conn_id)
{
    int ret;

    auto conn_ctx = getConnContext(conn_id);

    if (conn_ctx == nullptr) {
        logger->error << "Client connection does not exist, check connection settings." << std::flush;
        setStatus(TransportStatus::Disconnected);
        return;
    }

    logger->info << "Thread client packet loop for client conn_id: " << conn_id << std::flush;

    if (conn_ctx->pq_cnx == NULL) {
        logger->Log(cantina::LogLevel::Error, "Could not create picoquic connection client context");
    } else {
        picoquic_set_callback(conn_ctx->pq_cnx, pq_event_cb, this);

        picoquic_enable_keep_alive(conn_ctx->pq_cnx, tconfig.idle_timeout_ms * 500);
        ret = picoquic_start_client_cnx(conn_ctx->pq_cnx);
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

void PicoQuicTransport::shutdown()
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

void PicoQuicTransport::check_callback_delta(DataContext* data_ctx, bool tx) {
    if (!tx) return;

    const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

    if (data_ctx->last_tx_tick == 0) {
        data_ctx->last_tx_tick = current_tick;
        return;
    }

    const auto delta_ms = current_tick - data_ctx->last_tx_tick;
    data_ctx->last_tx_tick = current_tick;


    if (data_ctx->priority > 0 && delta_ms > 50 && data_ctx->tx_data->size() >= 3) {
        data_ctx->metrics.tx_delayed_callback++;

        picoquic_path_quality_t path_quality;

        if (const auto conn_it = getConnContext(data_ctx->conn_id)) {
            picoquic_get_path_quality(conn_it->pq_cnx, conn_it->pq_cnx->path[0]->unique_path_id, &path_quality);
        }

        logger->info << "conn_id: " << data_ctx->conn_id
                      << " data_ctx_id: " << data_ctx->data_ctx_id
                      << " stream_id: " << data_ctx->current_stream_id
                      << " pri: " << static_cast<int>(data_ctx->priority)
                      << " CB TX delta " << delta_ms << " ms"
                      << " cb_tx_count: " << data_ctx->metrics.tx_delayed_callback
                      << " tx_queue_size: " << data_ctx->tx_data->size()
                      << " expired_count: " << data_ctx->metrics.tx_queue_expired
                      << " dgram_cb_count: " << data_ctx->metrics.dgram_prepare_send
                      << " stream_cb_count: " << data_ctx->metrics.stream_prepare_send
                      << " tx_reset_wait: " << data_ctx->metrics.tx_reset_wait
                      << " tx_queue_discards: " << data_ctx->metrics.tx_queue_discards
                      << " rate Kbps: " << path_quality.pacing_rate * 8 / 1000
                      << " cwin_bytes: " << path_quality.cwin
                      << " rtt_us: " << path_quality.rtt
                      << " rtt_max: " << path_quality.rtt_max
                      << " rtt_sample: " << path_quality.rtt_sample
                      << " lost_pkts: " << path_quality.lost
                      << " bytes_in_transit: " << path_quality.bytes_in_transit
                      << " recv_rate_Kbps: " << path_quality.receive_rate_estimate * 8 / 1000
                      << std::flush;
    }
}

void PicoQuicTransport::cbNotifier()
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

void PicoQuicTransport::create_stream(ConnectionContext& conn_ctx, DataContext *data_ctx)
{
    conn_ctx.last_stream_id = ::get_next_stream_id(conn_ctx.last_stream_id ,
                                                   _is_server_mode, !data_ctx->is_bidir);

    logger->debug << "conn_id: " << conn_ctx.conn_id << " data_ctx_id: " << data_ctx->data_ctx_id
                 << " create new stream with stream_id: " << conn_ctx.last_stream_id
                 << std::flush;

    if (data_ctx->current_stream_id) {
        close_stream(conn_ctx, data_ctx, false);
    }

    data_ctx->current_stream_id = conn_ctx.last_stream_id;

    /*
     * Low order bit set indicates FIFO handling of same priorities, unset is round-robin
     */
    picoquic_set_stream_priority(conn_ctx.pq_cnx, data_ctx->current_stream_id, (data_ctx->priority << 1));
    picoquic_mark_active_stream(conn_ctx.pq_cnx, data_ctx->current_stream_id, 1, data_ctx);

    data_ctx->mark_stream_active = true;

    picoquic_runner_queue.push([=, conn_id = conn_ctx.conn_id, data_ctx_id = data_ctx->data_ctx_id]() {
        mark_stream_active(conn_id, data_ctx_id);
    });


}

void PicoQuicTransport::close_stream(const ConnectionContext& conn_ctx, DataContext* data_ctx, const bool send_reset)
{
    if (data_ctx->current_stream_id == 0) {
        return; // stream already closed
    }

    logger->debug << "conn_id: " << conn_ctx.conn_id << " data_ctx_id: " << data_ctx->data_ctx_id
                 << " closing stream stream_id: " << data_ctx->current_stream_id
                 << std::flush;

    if (send_reset) {
        logger->debug << "Reset stream_id: " << data_ctx->current_stream_id << " conn_id: " << conn_ctx.conn_id
                     << std::flush;

        picoquic_set_app_stream_ctx(conn_ctx.pq_cnx, data_ctx->current_stream_id , NULL);
        picoquic_reset_stream(conn_ctx.pq_cnx, data_ctx->current_stream_id, 0);

    } else {
        logger->info << "Sending FIN for stream_id: " << data_ctx->current_stream_id << " conn_id: " << conn_ctx.conn_id
                     << std::flush;

        picoquic_add_to_stream(conn_ctx.pq_cnx, data_ctx->current_stream_id, NULL, 0, 1);
    }

    data_ctx->reset_tx_object();
    data_ctx->stream_rx_buffer.erase(data_ctx->current_stream_id);

    data_ctx->current_stream_id = 0;
}

void PicoQuicTransport::mark_stream_active(const TransportConnId conn_id, const DataContextId data_ctx_id) {
    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_it = conn_context.find(conn_id);
    if (conn_it == conn_context.end()) {
        return;
    }

    const auto data_ctx_it = conn_it->second.active_data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_it->second.active_data_contexts.end()) {
        return;
    }

    data_ctx_it->second.mark_stream_active = false;

    if (data_ctx_it->second.current_stream_id == 0) {
        return;
    }

    picoquic_mark_active_stream(conn_it->second.pq_cnx,
                                data_ctx_it->second.current_stream_id,
                                1,
                                &data_ctx_it->second);
}

void PicoQuicTransport::mark_dgram_ready(const TransportConnId conn_id) {
    std::lock_guard<std::mutex> _(_state_mutex);

    const auto conn_it = conn_context.find(conn_id);
    if (conn_it == conn_context.end()) {
        return;
    }

    picoquic_mark_datagram_ready(conn_it->second.pq_cnx, 1);

    conn_it->second.default_data_context.mark_dgram_ready = false;
}
