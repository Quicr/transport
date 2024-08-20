#include <cassert>
#include <cstring> // memcpy
#include <iostream>
#include <thread>
#include <sstream>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#if defined(__linux__)
#include <net/ethernet.h>
#include <netpacket/packet.h>
#elif defined(__APPLE__)

#include <net/if_dl.h>

#endif

#include "transport_udp.h"

#if defined(PLATFORM_ESP)
#include <lwip/netdb.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

using namespace qtransport;

#if defined (PLATFORM_ESP)
static esp_pthread_cfg_t create_config(const char *name, int core_id, int stack, int prio)
{
    auto cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.pin_to_core = core_id;
    cfg.stack_size = stack;
    cfg.prio = prio;
    return cfg;
}
#endif

#define LOGGER_TRACE(logger, ...) if (logger) SPDLOG_LOGGER_TRACE(logger, __VA_ARGS__)
#define LOGGER_DEBUG(logger, ...) if (logger) SPDLOG_LOGGER_DEBUG(logger, __VA_ARGS__)
#define LOGGER_INFO(logger, ...) if (logger) SPDLOG_LOGGER_INFO(logger, __VA_ARGS__)
#define LOGGER_WARN(logger, ...) if (logger) SPDLOG_LOGGER_WARN(logger, __VA_ARGS__)
#define LOGGER_ERROR(logger, ...) if (logger) SPDLOG_LOGGER_ERROR(logger, __VA_ARGS__)
#define LOGGER_CRITICAL(logger, ...) if (logger) SPDLOG_LOGGER_CRITICAL(logger, __VA_ARGS__)

UDPTransport::~UDPTransport() {
    // TODO: Close all streams and connections

    // Stop threads
    _stop = true;

    // Close socket fd
    if (_fd >= 0)
        ::close(_fd);

    LOGGER_INFO(_logger, "Closing transport threads");
    for (auto &thread: _running_threads) {
        if (thread.joinable())
            thread.join();
    }
}

UDPTransport::UDPTransport(const TransportRemote &server,
                           const TransportConfig& tcfg,
                           TransportDelegate &delegate,
                           bool isServerMode,
                           std::shared_ptr<spdlog::logger> logger)
        : _stop(false),
    _logger(std::move(logger)),
    _tconfig(tcfg),
    _fd(-1),
    _isServerMode(isServerMode),
    _serverInfo(server), _delegate(delegate) {
    _tick_service = std::make_shared<ThreadedTickService>();
}

TransportStatus UDPTransport::status() const {
    if (_stop) {
        return TransportStatus::Shutdown;
    } else if (_isServerMode && _fd > 0) {
        return TransportStatus::Ready;
    }
    return _clientStatus;
}

DataContextId UDPTransport::createDataContext(const qtransport::TransportConnId conn_id,
                                              [[maybe_unused]] bool use_reliable_transport,
                                              uint8_t priority,
                                              [[maybe_unused]] bool bidir) {

    [[maybe_unused]] const std::lock_guard<std::mutex> wl(_writer_mutex);
    [[maybe_unused]] const std::lock_guard<std::mutex> rl(_reader_mutex);

    const auto conn_it = _conn_contexts.find(conn_id);

    if (conn_it == _conn_contexts.end()) {
        LOGGER_ERROR(_logger, "Failed to create data context, invalid connection id: {0}", conn_id);
        return 0; // Error
    }

    auto& conn = *conn_it->second;

    const auto data_ctx_id = conn.next_data_ctx_id++;

    const auto& [data_ctx_it, is_new] = conn.data_contexts.try_emplace(data_ctx_id);

    if (is_new) {
        LOGGER_INFO(_logger, "Creating data context conn_id: {0} data_ctx_id: {1}", conn_id, data_ctx_id);
        data_ctx_it->second.data_ctx_id = data_ctx_id;
        data_ctx_it->second.priority = priority;
        data_ctx_it->second.rx_data.set_limit(_tconfig.time_queue_rx_size);
    }

    return data_ctx_id;
}

TransportConnId UDPTransport::start(std::shared_ptr<SafeQueue<MetricsConnSample>> metrics_conn_samples,
                                    std::shared_ptr<SafeQueue<MetricsDataSample>> metrics_data_samples) {

    this->metrics_conn_samples = std::move(metrics_conn_samples);
    this->metrics_data_samples = std::move(metrics_data_samples);

    if (_isServerMode) {
        return connect_server();
    }

    return connect_client();
}

void UDPTransport::deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) {
    [[maybe_unused]] const std::lock_guard<std::mutex> wl(_writer_mutex);
    [[maybe_unused]] const std::lock_guard<std::mutex> rl(_reader_mutex);

    auto conn_it = _conn_contexts.find(conn_id);
    if (conn_it != _conn_contexts.end()) {
        LOGGER_INFO(_logger, "Delete data context id: {0} in conn_id: {1}", data_ctx_id, conn_id);
        conn_it->second->data_contexts.erase(data_ctx_id);
    }
}

void UDPTransport::setRemoteDataCtxId(const TransportConnId conn_id,
                                      const DataContextId data_ctx_id,
                                      const DataContextId remote_data_ctx_id) {

    const std::lock_guard<std::mutex> _(_writer_mutex);

    auto conn_it = _conn_contexts.find(conn_id);
    if (conn_it != _conn_contexts.end()) {
        const auto data_ctx_it = conn_it->second->data_contexts.find(data_ctx_id);
        if (data_ctx_it != conn_it->second->data_contexts.end()) {
            LOGGER_DEBUG(_logger,
                                "Setting remote data context id conn_id: {0} data_ctx_id: {1} remote_data_ctx_id: {2}",
                                conn_id,
                                data_ctx_id,
                                remote_data_ctx_id);
            data_ctx_it->second.remote_data_ctx_id = remote_data_ctx_id;
            data_ctx_it->second.remote_data_ctx_id_V = std::move(to_uintV(remote_data_ctx_id));
        }
    }
}

bool UDPTransport::getPeerAddrInfo(const TransportConnId &conn_id,
                                   sockaddr_storage *addr) {
    // Locate the given transport context
    auto it = _conn_contexts.find(conn_id);

    // If not found, return false
    if (it == _conn_contexts.end()) return false;

    // Copy the address information
    std::memcpy(addr, &it->second->addr, sizeof(it->second->addr));

    return true;
}

void UDPTransport::close(const TransportConnId &conn_id, uint64_t app_reason_code) {

    LOGGER_DEBUG(_logger, "Close UDP conn_id: {0}", conn_id);

    std::unique_lock<std::mutex> wlock(_writer_mutex);
    std::unique_lock<std::mutex> rlock(_reader_mutex);

    auto conn_it = _conn_contexts.find(conn_id);
    if (conn_it != _conn_contexts.end()) {

        if (conn_it->second->status == TransportStatus::Ready) {
            send_disconnect(conn_it->second->id, conn_it->second->addr);
        }

        _addr_conn_contexts.erase(conn_it->second->addr.id);
        _conn_contexts.erase(conn_it);

        if (!_isServerMode) {
            _stop = true;
        }

        wlock.unlock(); // Make sure to not lock when calling delegates
        rlock.unlock(); // Make sure to not lock when calling delegates
        _delegate.on_connection_status(conn_id, TransportStatus::Disconnected);
        return;
    }
}

AddrId UDPTransport::create_addr_id(const sockaddr_storage &addr) {
    AddrId id;

    switch (addr.ss_family) {
        case AF_INET: {
            sockaddr_in *s = (sockaddr_in *) &addr;

            id.port = s->sin_port;
            id.ip_lo = s->sin_addr.s_addr;
            break;
        }
        default: {
            // IPv6
            sockaddr_in6 *s = (sockaddr_in6 *) &addr;

            id.port = s->sin6_port;

            id.ip_hi = (uint64_t) &s->sin6_addr;
            id.ip_lo = (uint64_t) &s->sin6_addr + 8;
            break;
        }
    }

    return id;
}

TransportRemote UDPTransport::create_addr_remote(const sockaddr_storage &addr) {
    TransportRemote remote;

    char ip[INET6_ADDRSTRLEN];

    remote.proto = TransportProtocol::UDP;

    switch (addr.ss_family) {
        case AF_INET: {
            sockaddr_in *s = (sockaddr_in *) &addr;

            remote.port = s->sin_port;
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
            break;
        }
        case AF_INET6: {
            // IPv6
            sockaddr_in6 *s = (sockaddr_in6 *) &addr;

            remote.port = s->sin6_port;
            inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
            break;
        }
        default:
            LOGGER_ERROR(_logger, "Unknown AFI: {0}", static_cast<int>(addr.ss_family));
            break;
    }

    return std::move(remote);
}

bool UDPTransport::send_connect(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::ConnectMsg chdr {};

    chdr.idle_timeout = 20;

    int numSent = sendto(_fd,
                         (uint8_t *)&chdr,
                         sizeof(chdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending CONNECT to UDP socket: {1}", conn_id, strerror(errno));

        return false;

    } else if (numSent != sizeof(chdr)) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send CONNECT message, sent: {1}", conn_id, numSent);
        return false;
    }

    return true;
}

bool UDPTransport::send_connect_ok(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::ConnectOkMsg hdr {};

    int numSent = sendto(_fd,
                         (uint8_t *)&hdr,
                         sizeof(hdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending CONNECT_OK to UDP socket: {1}", conn_id, strerror(errno));

        return false;

    } else if (numSent != sizeof(hdr)) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send CONNECT_OK message, sent: {1}", conn_id, numSent);
        return false;
    }

    return true;
}

bool UDPTransport::send_disconnect(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::DisconnectMsg dhdr {};

    int numSent = sendto(_fd,
                         (uint8_t *)&dhdr,
                         sizeof(dhdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending DISCONNECT to UDP socket: {1}", conn_id, strerror(errno));

        return false;

    } else if (numSent != sizeof(dhdr)) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send DISCONNECT message, sent: {1}", conn_id, numSent);
        return false;
    }

    return true;
}

bool UDPTransport::send_keepalive(ConnectionContext& conn) {
    UdpProtocol::KeepaliveMsg khdr {};

    const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));
    khdr.ticks_ms = current_tick - conn.tx_next_report_tick;

    LOGGER_DEBUG(_logger, "conn_id: {0} send KEEPALIVE", conn.id);

    int numSent = sendto(_fd,
                         (uint8_t *)&khdr,
                         sizeof(khdr),
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);


    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending KEEPALIVE to UDP socket: {1}", conn.id, strerror(errno));

        return false;

    } else if (numSent != sizeof(khdr)) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send KEEPALIVE message, sent: {1}", conn.id, numSent);
        return false;
    }

    return true;
}

bool UDPTransport::send_report(ConnectionContext& conn) {
    int numSent = sendto(_fd,
                         (uint8_t *)&conn.report,
                         sizeof(conn.report),
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);


    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending REPORT to UDP socket: {1}", conn.id, strerror(errno));

        return false;

    } else if (numSent != sizeof(conn.report)) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send REPORT message, sent: {1}", conn.id, numSent);
        return false;
    }

    conn.report.metrics.duration_ms = 0;
    conn.report.metrics.total_bytes = 0;
    conn.report.metrics.total_packets = 0;

    return true;
}


bool UDPTransport::send_data(ConnectionContext& conn, DataContext& data_ctx, const ConnData& cd, bool discard) {
    UdpProtocol::DataMsg dhdr {};
    uint8_t data[UDP_MAX_PACKET_SIZE] {0};

    if (discard) {
        dhdr.flags.discard = 1;
    }

    const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

    if (current_tick >= conn.tx_next_report_tick) {
        // New report ID
        auto& prev_report = conn.tx_prev_reports[(conn.tx_report_id % conn.tx_prev_reports.size())];
        prev_report.report_id = conn.tx_report_id++;
        prev_report.metrics = conn.tx_report_metrics;

        conn.tx_report_start_tick = current_tick;
        conn.tx_report_metrics = {};

        conn.tx_next_report_tick = current_tick + conn.tx_report_interval_ms;
    }

    dhdr.report_id = conn.tx_report_id;
    dhdr.ticks_ms = current_tick - conn.tx_report_start_tick;

    auto data_len = sizeof(dhdr) + cd.data.size();
    data_len += discard ? 1 :  data_ctx.remote_data_ctx_id_V.size();

    if (data_len > sizeof(data)) {
        LOGGER_ERROR(_logger, "conn_id: {0} data_len: {1} is too large", conn.id, data_len);
        return false;
    }


    auto data_p = data;
    memcpy(data_p, &dhdr, sizeof(dhdr));
    data_p += sizeof(dhdr);

    if (discard) {
        uint8_t zero = 0;
        conn.metrics.tx_discard_objects++;

        LOGGER_DEBUG(_logger, "Sending discard data size: {0}", cd.data.size());
        memcpy(data_p, &zero, 1);
        data_p++;

    } else {
        memcpy(data_p, data_ctx.remote_data_ctx_id_V.data(), data_ctx.remote_data_ctx_id_V.size());
        data_p += data_ctx.remote_data_ctx_id_V.size();
    }

    memcpy(data_p, cd.data.data(), cd.data.size());

    int numSent = sendto(_fd,
                         data,
                         data_len,
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);

    if (numSent < 0) {
        LOGGER_ERROR(_logger, "conn_id: {0} Error sending DATA to UDP socket: {1}", conn.id, strerror(errno));

        return false;

    } else if (numSent != data_len) {
        LOGGER_WARN(_logger, "conn_id: {0} Failed to send DATA len: {1}, sent: {2}", conn.id, data_len, numSent);
        return false;
    }

    conn.tx_report_metrics.total_bytes += cd.data.size();
    conn.tx_report_metrics.total_packets++;

    if (conn.last_tx_msg_tick)
        conn.tx_report_metrics.duration_ms += current_tick - conn.last_tx_msg_tick;

    return true;
}


/*
 * Blocking socket writer. This should be called in its own thread
 *
 * Writer will perform the following:
 *  - loop reads data from fd_write_queue and writes it to the socket
 */
void UDPTransport::fd_writer() {
    timeval to;
    to.tv_usec = 1000;
    to.tv_sec = 0;

    LOGGER_INFO(_logger, "Starting transport writer thread");

    bool sent_data = false;
    int all_empty_count = 0;
    std::unique_lock<std::mutex> lock(_writer_mutex);

    while (not _stop) {
        sent_data = false;

        bool unlock = true;

        // Check each connection context for data to send
        for (const auto& [conn_id, conn]: _conn_contexts) {
            const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds (1));

            // Check if idle
            if (conn->last_rx_msg_tick && current_tick - conn->last_rx_msg_tick >= conn->idle_timeout_ms) {
                LOGGER_ERROR(_logger, "conn_id: {0} TIME OUT, disconnecting connection", conn_id);
                unlock = false;
                lock.unlock();
                close(conn_id);
                break; // Don't continue with for loop since iterator will be invalidated upon close
            }

            // Shape flow by only processing data if wait for tick value is less than or equal to current tick
            if (conn->wait_for_tick > current_tick) {
                continue;
            }

            if (conn->tx_data->empty()) { // No data, go to next connection
                // Send keepalive if needed
                if (conn->last_tx_msg_tick && current_tick - conn->last_tx_msg_tick > conn->ka_interval_ms) {
                    conn->last_tx_msg_tick = current_tick;
                    send_keepalive(*conn);
                }
                continue;
            }

            auto cd = conn->tx_data->pop_front();

            if (!cd.has_value) {
                // Send keepalive if needed
                if (conn->last_tx_msg_tick && current_tick - conn->last_tx_msg_tick > conn->ka_interval_ms) {
                    conn->last_tx_msg_tick = current_tick;
                    send_keepalive(*conn);
                }

                sent_data = true; // Don't treat this as data not sent, which causes a pause
                continue; // Data maybe null if time queue has a delay in pop
            }

            const auto data_ctx_it = conn->data_contexts.find(cd.value.data_ctx_id);
            if (data_ctx_it == conn->data_contexts.end()) {
                LOGGER_WARN(_logger, "No data context, ignoring conn_id: {0} data_ctx_id: {1}", conn_id, cd.value.data_ctx_id);
                conn->metrics.tx_no_context++;
                continue;
            }

            data_ctx_it->second.metrics.tx_queue_expired += cd.expired_count;

            cd.value.trace.push_back({"transport_udp:send_data", cd.value.trace.front().start_time});

            if (!cd.value.trace.empty() && cd.value.trace.back().delta > 60000) {
                std::ostringstream log_msg;
                log_msg << "MethodTrace conn_id: " << cd.value.conn_id
                             << " data_ctx_id: " << cd.value.data_ctx_id
                             << " priority: " << static_cast<int>(cd.value.priority);
                for (const auto &ti: cd.value.trace) {
                    log_msg << " " << ti.method << ": " << ti.delta << " ";
                }

                log_msg << " total_duration: " << cd.value.trace.back().delta;

                LOGGER_INFO(_logger, log_msg.str());
            }

            if (! send_data(*conn, data_ctx_it->second, cd.value, (cd.value.priority ? false : true))) {
                continue;
            }

            data_ctx_it->second.metrics.tx_bytes += cd.value.data.size();
            data_ctx_it->second.metrics.tx_objects++;

            sent_data = true;

            conn->last_tx_msg_tick = current_tick;

            // Calculate the wait for tick value
            conn->running_wait_us += static_cast<int>(cd.value.data.size() / conn->bytes_per_us);

            if (conn->running_wait_us > 1000) {
                conn->wait_for_tick = current_tick + conn->running_wait_us / 1000;

                conn->running_wait_us %= 1000; // Set running age to remainder value less than a tick
                //conn->running_wait_us = 0;
            }
        }

        if (unlock) lock.unlock();

        if (!sent_data) {
            all_empty_count++;

            if (all_empty_count > 5) {
                all_empty_count = 1;
                to.tv_usec = 300;
                select(0, NULL, NULL, NULL, &to);
            }
        }

        lock.lock();
    }

    LOGGER_INFO(_logger, "Done transport writer thread");
}

/*
 * Blocking socket FD reader. This should be called in its own thread.
 *
 * Reader will perform the following:
 *  - Receive data from socket
 *  - Lookup addr in map to find context and name info
 *  - If context doesn't exist, then it's a new connection and the delegate will
 * be called after creating new context
 *  - Create ConnData and send to queue
 *  - Call on_recv_notify() delegate to notify of new data available. This is
 * not called again if there is still pending data to be dequeued for the same
 * StreamId
 */
void
UDPTransport::fd_reader() {
    LOGGER_INFO(_logger, "Starting transport reader thread");
 #if defined(PLATFORM_ESP)
  // TODO (Suhas): Revisit this once we have basic esp functionality working
  const int dataSize = 2048;
#else
  const int dataSize = UDP_MAX_PACKET_SIZE; // TODO Add config var to set this value.  Sizes
    // larger than actual MTU require IP frags
#endif
    uint8_t data[dataSize];

    std::unique_lock<std::mutex> lock(_reader_mutex);
    lock.unlock();      // Will lock later in while loop

    while (not _stop) {
        Addr remote_addr;

        int rLen = recvfrom(_fd,
                            data,
                            dataSize,
                            0 /*flags*/,
                            (struct sockaddr *) &remote_addr.addr,
                            &remote_addr.addr_len);

        if (rLen < 0 || _stop) {
            if ((errno == EAGAIN) || (_stop)) {
                // timeout on read or stop issued
                continue;

            } else {
                LOGGER_ERROR(_logger, "Error reading from UDP socket: {0}", strerror(errno));
                break;
            }
        }

        if (rLen == 0) {
            continue;
        }

        if (data[0] != UdpProtocol::PROTOCOL_VERSION) {
            // TODO: Add metrics to track discards on invalid received message
            continue;
        }

        const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

        remote_addr.id = create_addr_id(remote_addr.addr);

        lock.lock();
        const auto a_conn_it = _addr_conn_contexts.find(remote_addr.id);

        switch (static_cast<UdpProtocol::ProtocolType>(data[1])) { // Process based on type of message
            case UdpProtocol::ProtocolType::CONNECT: {
                UdpProtocol::ConnectMsg chdr;
                memcpy(&chdr, data, sizeof(chdr));

                if (chdr.idle_timeout == 0) {
                    // TODO: Add metric for invalid idle_timeout
                    LOGGER_DEBUG(_logger, "Invalid zero idle timeout for new connection, ignoring");
                    lock.unlock();
                    continue;
                }

                if (a_conn_it == _addr_conn_contexts.end()) { // New connection
                    if (_isServerMode) {
                        ++_last_conn_id;

                        send_connect_ok(_last_conn_id, remote_addr);

                        const auto [conn_it, _] = _conn_contexts.emplace(_last_conn_id,
                                                                        std::make_shared<ConnectionContext>());

                        auto &conn = *conn_it->second;
                        conn.tx_data = std::make_unique<PriorityQueue<ConnData>>(_tconfig.time_queue_max_duration,
                                                                                  _tconfig.time_queue_bucket_interval,
                                                                                  _tick_service,
                                                                                  _tconfig.time_queue_init_queue_size);
                        conn.addr = remote_addr;
                        conn.id = _last_conn_id;

                        conn.tx_report_interval_ms = _tconfig.time_queue_rx_size; // TODO: this temp to set this via UI

                        conn.last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                        conn.idle_timeout_ms = chdr.idle_timeout * 1000;
                        conn.ka_interval_ms = conn.idle_timeout_ms / 3;

                        // TODO: Consider adding BW in connect message to convey what the receiver would like to receive
                        conn.set_KBps(6250); // Set to 50Mbps connection rate

                        _addr_conn_contexts.emplace(remote_addr.id, conn_it->second); // Add to the addr lookup map

                        lock.unlock(); // no need to hold lock, especially with a call to a delegate
                        createDataContext(conn.id, false, 2, false);

                        // New remote address/connection
                        const TransportRemote remote = create_addr_remote(remote_addr.addr);

                        LOGGER_INFO(_logger, "New Connection from {0} port: {1}", remote.host_or_ip, remote.port);

                        // Notify caller that there is a new connection
                        _delegate.on_new_connection(_last_conn_id, remote);
                        continue;

                    } else {
                        /*
                         * Client mode doesn't support creating connections based on received
                         * packets. This will happen when there are scanners/etc. sending random data to this socket
                         */
                        lock.unlock();
                        continue;
                    }
                } else {
                    // Connection already exists, update idle timeout
                    a_conn_it->second->idle_timeout_ms = chdr.idle_timeout * 1000;
                    a_conn_it->second->ka_interval_ms = a_conn_it->second->idle_timeout_ms / 3;
                    lock.unlock();
                    continue;
                }
                break;
            }
            case UdpProtocol::ProtocolType::CONNECT_OK: {
                if (!_isServerMode) {
                    _clientStatus = TransportStatus::Ready;
                }

                if (a_conn_it != _addr_conn_contexts.end()) {
                    LOGGER_INFO(_logger, "conn_id: {0} received CONNECT_OK", a_conn_it->second->id);

                    a_conn_it->second->status = TransportStatus::Ready;
                }

                break;
            }
            case UdpProtocol::ProtocolType::DISCONNECT: {
                if (!_isServerMode) {
                    _clientStatus = TransportStatus::Disconnected;
                }

                if (a_conn_it != _addr_conn_contexts.end()) {
                    LOGGER_INFO(_logger, "conn_id: {0} received DISCONNECT", a_conn_it->second->id);

                    a_conn_it->second->status = TransportStatus::Disconnected;
                    lock.unlock();
                    const auto conn_id = a_conn_it->second->id;
                    close(conn_id);
                    continue;
                }
                break;
            }
            case UdpProtocol::ProtocolType::KEEPALIVE: {
                if (a_conn_it != _addr_conn_contexts.end()) {
                    a_conn_it->second->last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                    UdpProtocol::KeepaliveMsg hdr;
                    memcpy(&hdr, data, sizeof(hdr));

                    a_conn_it->second->last_rx_hdr_tick = hdr.ticks_ms;
                }
                break;
            }
            case UdpProtocol::ProtocolType::REPORT: {
                if (a_conn_it != _addr_conn_contexts.end()) {
                    a_conn_it->second->last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                    UdpProtocol::ReportMessage hdr;
                    memcpy(&hdr, data, sizeof(hdr));

                    const auto& report_id = hdr.report_id;
                    const auto& metrics = hdr.metrics;

                    if (metrics.total_bytes == 0 || metrics.duration_ms == 0) {
                        lock.unlock();
                        continue;
                    }

                    const auto& prev_report = a_conn_it->second->tx_prev_reports[(hdr.report_id % a_conn_it->second->tx_prev_reports.size())];
                    if (prev_report.report_id != hdr.report_id) {
                        LOGGER_WARN(
                          _logger,
                          "Received report id: {0} is not previous id: {1} sizeof array: {2} prev_index: {3}",
                          report_id,
                          prev_report.report_id,
                          sizeof(a_conn_it->second->tx_prev_reports),
                          (hdr.report_id % a_conn_it->second->tx_prev_reports.size()));
                        lock.unlock();
                        continue;
                    }

                    const auto send_KBps = static_cast<int>(prev_report.metrics.total_bytes / prev_report.metrics.duration_ms);
                    //const auto ack_KBps = static_cast<int>(metrics.total_bytes / metrics.duration_ms);
                    const auto ack_KBps = static_cast<int>(metrics.total_bytes / metrics.duration_ms); //std::max(prev_report.metrics.duration_ms, metrics.duration_ms));
                    const auto prev_KBps = (a_conn_it->second->bytes_per_us * 1'000'000 / 1024);
                    const auto loss_pct = 1.0 - static_cast<double>(metrics.total_packets) / prev_report.metrics.total_packets;
                    a_conn_it->second->tx_report_ott = metrics.recv_ott_ms;

                    if (loss_pct >= 0.01 && metrics.total_packets > 10) {
                        LOGGER_INFO(_logger,
                                           "Received REPORT (decrease) conn_id: {0} tx_report_id: {1} duration_ms: {2} "
                                           "({3}) total_bytes: {4} ({5}) total_packets: {6} ({7}) send/ack Kbps: {8} / "
                                           "{9} prev_Kbps: {10} Loss: {11}% TX-OTT: {12}ms RX-OTT: {13}ms",
                                           a_conn_it->second->id,
                                           report_id,
                                           metrics.duration_ms,
                                           prev_report.metrics.duration_ms,
                                           metrics.total_bytes,
                                           prev_report.metrics.total_bytes,
                                           metrics.total_packets,
                                           prev_report.metrics.total_packets,
                                           send_KBps * 8,
                                           ack_KBps * 8,
                                           prev_KBps * 8,
                                           loss_pct,
                                           metrics.recv_ott_ms,
                                           a_conn_it->second->rx_report_ott);

                        a_conn_it->second->tx_zero_loss_count = 0;
                        a_conn_it->second->set_KBps(ack_KBps * .95);

                    } else if (metrics.total_packets > 10 && loss_pct == 0) {
                        a_conn_it->second->tx_zero_loss_count++;

                        // Only increase bandwidth if there is no loss for a little while
                        if (a_conn_it->second->tx_zero_loss_count > 5
                            && a_conn_it->second->set_KBps(ack_KBps * 1.03, true)) {

                            LOGGER_INFO(
                              _logger,
                              "Received REPORT (increase) conn_id: {0} prev_report_id: {1} tx_report_id: {2} "
                              "duration_ms: {3} "
                              "({4}) total_bytes: {5} ({6}) total_packets: {7} ({8}) send/ack Kbps: {9} / "
                              "{10} prev_Kbps: {11} Loss: {12}% TX-OTT: {13}ms RX-OTT: {14}ms",
                              a_conn_it->second->id,
                              a_conn_it->second->tx_report_id - 1,
                              report_id,
                              metrics.duration_ms,
                              prev_report.metrics.duration_ms,
                              metrics.total_bytes,
                              prev_report.metrics.total_bytes,
                              metrics.total_packets,
                              prev_report.metrics.total_packets,
                              send_KBps * 8,
                              ack_KBps * 8,
                              prev_KBps * 8,
                              loss_pct,
                              metrics.recv_ott_ms,
                              a_conn_it->second->rx_report_ott);

                            // Add some data discard packets to measure if increase is okay
                            std::vector<MethodTraceItem> trace;
                            const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

                            trace.push_back({"transport_udp:recv_data", start_time});
                            std::vector<uint8_t> discard_data(100);

                            // Number of objects to send is a burst of 5ms of date based on new rate spread over 100 byte objects
                            const auto send_count = (a_conn_it->second->bytes_per_us * 1000) * 5 / 100;

                            for (int i=0; i < send_count; i++) {
                                ConnData cd { a_conn_it->second->id, 0, 0,
                                              discard_data, trace};
                                a_conn_it->second->tx_data->push(cd, 6, 0, 0);
                            }

                            a_conn_it->second->tx_zero_loss_count = 2;
                        }
                    }
                }
                break;
            }

            case UdpProtocol::ProtocolType::DATA: {
                auto data_p = data;

                UdpProtocol::DataMsg hdr;
                memcpy(&hdr, data_p, sizeof(hdr));
                data_p += sizeof(hdr);
                rLen -= sizeof(hdr);

                const auto& report_id = hdr.report_id;
                const auto remote_data_ctx_id_len = uintV_size(*data_p);
                uintV_t remote_data_ctx_V (data_p, data_p + remote_data_ctx_id_len);
                data_p += remote_data_ctx_id_len;
                rLen -= remote_data_ctx_id_len;

                const auto data_ctx_id = to_uint64(remote_data_ctx_V);

                if (a_conn_it != _addr_conn_contexts.end()) {
                    if (report_id != a_conn_it->second->report.report_id &&
                            (report_id > a_conn_it->second->report.report_id
                             || report_id == 0 || a_conn_it->second->report.report_id - hdr.report_id > 1)) {

                        int rx_tick = current_tick - (a_conn_it->second->report_rx_start_tick + a_conn_it->second->last_rx_hdr_tick);
                        if (rx_tick >= 0) {
                            a_conn_it->second->report.metrics.recv_ott_ms = rx_tick;
                            a_conn_it->second->rx_report_ott = a_conn_it->second->report.metrics.recv_ott_ms;
                        }

                        send_report(*a_conn_it->second);

                        // Init metrics with this packet/data
                        a_conn_it->second->report_rx_start_tick = current_tick;
                        a_conn_it->second->report.report_id = hdr.report_id;
                        a_conn_it->second->report.metrics.duration_ms = current_tick - a_conn_it->second->last_rx_msg_tick;
                        a_conn_it->second->report.metrics.total_bytes = rLen;
                        a_conn_it->second->report.metrics.total_packets = 1;

                    } else if (hdr.report_id == a_conn_it->second->report.report_id) {
                        a_conn_it->second->report.metrics.duration_ms += current_tick - a_conn_it->second->last_rx_msg_tick;
                        a_conn_it->second->report.metrics.total_bytes += rLen;
                        a_conn_it->second->report.metrics.total_packets++;
                    }

                    a_conn_it->second->last_rx_msg_tick = current_tick;
                    a_conn_it->second->last_rx_hdr_tick = hdr.ticks_ms;

                    // Only process data if not set to discard
                    if (!hdr.flags.discard) {
                        const auto data_ctx_it = a_conn_it->second->data_contexts.find(data_ctx_id);
                        if (data_ctx_it == a_conn_it->second->data_contexts.end()) {
                            LOGGER_DEBUG(_logger, "Data context not found for RX object conn_id: {0} data_ctx_id: {1}", a_conn_it->second->id, data_ctx_id);

                            a_conn_it->second->metrics.rx_no_context++;
                            lock.unlock();
                            continue;
                        }

                        std::vector<uint8_t> buffer(data_p, data_p + rLen);

                        std::vector<MethodTraceItem> trace;
                        const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

                        trace.push_back({"transport_udp:recv_data", start_time});
                        ConnData cd { a_conn_it->second->id, data_ctx_id, 2,
                                      std::move(buffer), std::move(trace)};
                        cd.trace.reserve(10);


                        data_ctx_it->second.rx_data.push(cd);

                        lock.unlock();
                        if ( data_ctx_it->second.rx_data.size() < 10
                            || data_ctx_it->second.in_data_cb_skip_count > 20) {

                            _delegate.on_recv_dgram(cd.conn_id, cd.data_ctx_id);
                        } else {
                            data_ctx_it->second.in_data_cb_skip_count++;
                        }
                        continue;
                    }
                }
                break;
            }
            default:
                // TODO: Add metric to track discard due to invalid type
                break;
        }
        lock.unlock();
    }

    LOGGER_INFO(_logger, "Done transport reader thread");
}

TransportError UDPTransport::enqueue(const TransportConnId &conn_id,
                                     const DataContextId &data_ctx_id,
                                     std::vector<uint8_t> &&bytes,
                                     std::vector<qtransport::MethodTraceItem> &&trace,
                                     const uint8_t priority,
                                     const uint32_t ttl_ms,
                                     const uint32_t delay_ms,
                                     [[maybe_unused]] const EnqueueFlags flags) {
    if (bytes.empty()) {
        return TransportError::None;
    }

    trace.push_back({"transport_udp:enqueue", trace.front().start_time});

    std::lock_guard<std::mutex> _(_writer_mutex);

    trace.push_back({"transport_udp:enqueue:afterLock", trace.front().start_time});

    const auto conn_it = _conn_contexts.find(conn_id);

    if (conn_it == _conn_contexts.end()) {
        // Invalid connection id
        return TransportError::InvalidConnContextId;
    }

    const auto data_ctx_it = conn_it->second->data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_it->second->data_contexts.end()) {
        // Invalid data context id
        return TransportError::InvalidDataContextId;
    }

    data_ctx_it->second.metrics.enqueued_objs++;

    const auto trace_start_time = trace.front().start_time;
    ConnData cd { conn_id,
                  data_ctx_id,
                  priority,
                  std::move(bytes),
                  std::move(trace)};

    conn_it->second->tx_data->push(std::move(cd), ttl_ms, priority, delay_ms);

    return TransportError::None;
}

std::optional<std::vector<uint8_t>> UDPTransport::dequeue(TransportConnId conn_id,
                                                          std::optional<DataContextId> data_ctx_id) {

    if (!data_ctx_id) return std::nullopt;

    std::lock_guard<std::mutex> _(_reader_mutex);

    const auto conn_it = _conn_contexts.find(conn_id);
    if (conn_it == _conn_contexts.end()) {
        LOGGER_WARN(_logger, "dequeue: invalid conn_id: {0}", conn_id);
        // Invalid context id
        return std::nullopt;
    }

    const auto data_ctx_it = conn_it->second->data_contexts.find(*data_ctx_id);
    if (data_ctx_it == conn_it->second->data_contexts.end()) {
        LOGGER_ERROR(_logger, "dequeue: invalid stream for conn_id: {0} data_ctx_id: {1}", conn_id , *data_ctx_id);

        return std::nullopt;
    }

    if (auto cd = data_ctx_it->second.rx_data.pop()) {
        cd->trace.push_back({"transport_udp:dequeue", cd->trace.front().start_time});

        if (!cd->trace.empty() && cd->trace.back().delta > 1500) {
            std::ostringstream log_msg;
            log_msg << "MethodTrace conn_id: " << cd->conn_id
                         << " data_ctx_id: " << cd->data_ctx_id
                         << " priority: " << static_cast<int>(cd->priority);
            for (const auto &ti: cd->trace) {
                log_msg << " " << ti.method << ": " << ti.delta << " ";
            }

            log_msg << " total_duration: " << cd->trace.back().delta;
            LOGGER_INFO(_logger, log_msg.str());
        }

        return std::move(cd.value().data);
    }

    return std::nullopt;
}

TransportConnId UDPTransport::connect_client() {
    std::ostringstream s_log;

    _clientStatus = TransportStatus::Connecting;

    _fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_fd == -1) {
#if defined(PLATFORM_ESP)
      // TODO: Suhas: Figure out better API than aborting
      abort();
#else
      throw std::runtime_error("socket() failed");
#endif
    }

int err = 0;
#if not defined(PLATFORM_ESP)
    // TODO: Add config for these values
    size_t snd_rcv_max = UDP_MAX_PACKET_SIZE * 16;
    timeval rcv_timeout { .tv_sec = 0, .tv_usec = 1000 };

    err = setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    snd_rcv_max = UDP_MAX_PACKET_SIZE * 16; // TODO: Add config for value
    err = setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
        throw std::runtime_error(s_log.str());
    }
#endif

    err = setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }


    struct sockaddr_in srvAddr;
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    srvAddr.sin_port = 0;
    err = bind(_fd, (struct sockaddr *) &srvAddr, sizeof(srvAddr));
    if (err) {
        s_log << "client_connect: Unable to bind to socket: " << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    std::string sPort = std::to_string(htons(_serverInfo.port));
    struct addrinfo hints = {}, *address_list = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    err = getaddrinfo(
            _serverInfo.host_or_ip.c_str(), sPort.c_str(), &hints, &address_list);
    if (err) {
        strerror(1);
        s_log << "client_connect: Unable to resolve remote ip address: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    struct addrinfo *item = nullptr, *found_addr = nullptr;
    for (item = address_list; item != nullptr; item = item->ai_next) {
        if (item->ai_family == AF_INET && item->ai_socktype == SOCK_DGRAM &&
            item->ai_protocol == IPPROTO_UDP) {
            found_addr = item;
            break;
        }
    }

    if (found_addr == nullptr) {
        LOGGER_CRITICAL(_logger, "client_connect: No IP address found");
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *) &_serverAddr.addr;
    memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
    ipv4->sin_port = htons(_serverInfo.port);
    _serverAddr.addr_len = sizeof(sockaddr_in);

    freeaddrinfo(address_list);

    _serverAddr.id = create_addr_id(_serverAddr.addr);

    ++_last_conn_id;

    const auto& [conn_it, is_new] = _conn_contexts.emplace(_last_conn_id,
                                                          std::make_shared<ConnectionContext>());

    auto &conn = *conn_it->second;
    conn.addr = _serverAddr;
    conn.id = _last_conn_id;
    conn.tx_data = std::make_unique<PriorityQueue<ConnData>>(_tconfig.time_queue_max_duration,
                                                              _tconfig.time_queue_bucket_interval,
                                                              _tick_service,
                                                              _tconfig.time_queue_init_queue_size);


    conn.tx_report_interval_ms = _tconfig.time_queue_rx_size; // TODO: this temp to set this via UI

    conn.set_KBps(2000); // Set to 16Mbps=2000KBps connection rate

    _addr_conn_contexts.emplace(_serverAddr.id, conn_it->second); // Add to the addr lookup map
    createDataContext(conn.id, false, 2, false);

    send_connect(conn.id, conn.addr);

    // Notify caller that the connection is now ready
    _delegate.on_connection_status(_last_conn_id, TransportStatus::Ready);


#if defined(PLATFORM_ESP)
    auto cfg = create_config("FDReader", 1, 12 * 1024, 5);
    auto esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
        abort();
    }
#endif

    _running_threads.emplace_back(&UDPTransport::fd_reader, this);

#if defined(PLATFORM_ESP)
    cfg = create_config("FDWriter", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif

    _running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return _last_conn_id;
}

TransportConnId UDPTransport::connect_server() {
    std::stringstream s_log;

    _fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_fd < 0) {
        s_log << "connect_server: Unable to create socket: " << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    // set for re-use
    int one = 1;
    int err = setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    if (err != 0) {
        s_log << "connect_server: setsockopt error: " << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif

    }

    // TODO: Add config for this value
    size_t snd_rcv_max = UDP_MAX_PACKET_SIZE * 16;
    timeval rcv_timeout{.tv_sec = 0, .tv_usec = 1000};

    err = setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    err = setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif

    }

    err = setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }


    struct sockaddr_in srv_addr;
    memset((char *) &srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_port = htons(_serverInfo.port);
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    err = bind(_fd, (struct sockaddr *) &srv_addr, sizeof(srv_addr));
    if (err < 0) {
        s_log << "connect_server: unable to bind to socket: " << strerror(errno);
        LOGGER_CRITICAL(_logger, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    LOGGER_INFO(_logger, "connect_server: port: {0} fd: {1}", _serverInfo.port, _fd);

#if defined(PLATFORM_ESP)
    cfg = create_config("FDServerReader", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif
    _running_threads.emplace_back(&UDPTransport::fd_reader, this);

#if defined(PLATFORM_ESP)
    cfg = create_config("FDServerWriter", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif

    _running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return _last_conn_id;
}
