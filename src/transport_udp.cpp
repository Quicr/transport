#include <cassert>
#include <cstring> // memcpy
#include <iostream>
#include <thread>
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

UDPTransport::~UDPTransport() {
    // TODO: Close all streams and connections

    // Stop threads
    stop = true;

    // Close socket fd
    if (fd >= 0)
        ::close(fd);

    logger->Log("Closing transport threads");
    for (auto &thread: running_threads) {
        if (thread.joinable())
            thread.join();
    }
}

UDPTransport::UDPTransport(const TransportRemote &server,
                           const TransportConfig& tcfg,
                           TransportDelegate &delegate,
                           bool isServerMode,
                           const cantina::LoggerPointer &logger)
        : stop(false),
        logger(std::make_shared<cantina::Logger>("UDP", logger)),
        tconfig(tcfg),
        fd(-1),
        isServerMode(isServerMode),
        serverInfo(server), delegate(delegate) {
    _tick_service = std::make_shared<threaded_tick_service>();
}

TransportStatus UDPTransport::status() const {
    if (stop) {
        return TransportStatus::Shutdown;
    } else if (isServerMode && fd > 0) {
        return TransportStatus::Ready;
    } else if (!isServerMode) {
        return clientStatus;
    }
}

/*
 * UDP doesn't support multiple streams for clients.  This will return the same
 * stream ID used for the context
 */
DataContextId UDPTransport::createDataContext(const qtransport::TransportConnId conn_id,
                                              [[maybe_unused]] bool use_reliable_transport,
                                              uint8_t priority,
                                              [[maybe_unused]] bool bidir) {

    const auto conn_it = conn_contexts.find(conn_id);

    if (conn_it == conn_contexts.end()) {
        logger->error << "Failed to create data context, invalid connection id: " << conn_id << std::flush;
        return 0; // Error
    }

    auto& conn = *conn_it->second;

    // Currently only a single/datagram stream/flow is implemented.
    auto data_ctx_id = 0;

    const auto& [data_ctx_it, is_new] = conn.data_contexts.try_emplace(data_ctx_id);

    if (is_new) {
        data_ctx_it->second.data_ctx_id = data_ctx_id;
        data_ctx_it->second.priority = priority;
        data_ctx_it->second.rx_data.set_limit(tconfig.time_queue_rx_size);
        data_ctx_it->second.tx_data = std::make_unique<priority_queue<ConnData>>(tconfig.time_queue_max_duration,
                                                                                 tconfig.time_queue_bucket_interval,
                                                                                 _tick_service,
                                                                                 tconfig.time_queue_init_queue_size);
    }

    return data_ctx_id;
}

TransportConnId UDPTransport::start() {

    if (isServerMode) {
        return connect_server();
    }

    return connect_client();
}

void UDPTransport::deleteDataContext(const TransportConnId& conn_id, DataContextId data_ctx_id) {

    // Do not delete the default datagram context id ZERO
    if (data_ctx_id > 0) {
        auto conn_it = conn_contexts.find(conn_id);
        if (conn_it != conn_contexts.end()) {
            logger->info << "Delete data context id: " << data_ctx_id << " in conn_id: " << conn_id << std::flush;
            conn_it->second->data_contexts.erase(data_ctx_id);
        }
    }
}

bool UDPTransport::getPeerAddrInfo(const TransportConnId &conn_id,
                                   sockaddr_storage *addr) {
    // Locate the given transport context
    auto it = conn_contexts.find(conn_id);

    // If not found, return false
    if (it == conn_contexts.end()) return false;

    // Copy the address information
    std::memcpy(addr, &it->second->addr, sizeof(it->second->addr));

    return true;
}

void UDPTransport::close(const TransportConnId &conn_id) {

    logger->debug << "Close UDP conn_id: " << conn_id << std::flush;

    std::unique_lock<std::mutex> wlock(_writer_mutex);
    std::unique_lock<std::mutex> rlock(_reader_mutex);

    auto conn_it = conn_contexts.find(conn_id);
    if (conn_it != conn_contexts.end()) {

        if (conn_it->second->status == TransportStatus::Ready) {
            send_disconnect(conn_it->second->id, conn_it->second->addr);
        }

        addr_conn_contexts.erase(conn_it->second->addr.id);
        conn_contexts.erase(conn_it);

        if (!isServerMode) {
            stop = true;
        }

        wlock.unlock(); // Make sure to not lock when calling delegates
        rlock.unlock(); // Make sure to not lock when calling delegates
        delegate.on_connection_status(conn_id, TransportStatus::Disconnected);
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

    return std::move(id);
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
            logger->error << "Unknown AFI: " << static_cast<int>(addr.ss_family) << std::flush;
            break;
    }

    return std::move(remote);
}

bool UDPTransport::send_connect(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::ConnectMsg chdr {};

    chdr.idle_timeout = 20;

    int numSent = sendto(fd,
                         (uint8_t *)&chdr,
                         sizeof(chdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        logger->error << "conn_id: " << conn_id
                      << " Error sending CONNECT to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != sizeof(chdr)) {
        logger->info << "conn_id: " << conn_id
                     << " Failed to send CONNECT message, sent: " << numSent
                     << std::flush;
        return false;
    }

    return true;
}

bool UDPTransport::send_connect_ok(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::ConnectOkMsg hdr {};

    int numSent = sendto(fd,
                         (uint8_t *)&hdr,
                         sizeof(hdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        logger->error << "conn_id: " << conn_id
                      << " Error sending CONNECT OK to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != sizeof(hdr)) {
        logger->info << "conn_id: " << conn_id
                     << " Failed to send CONNECT OK message, sent: " << numSent
                     << std::flush;
        return false;
    }

    return true;
}

bool UDPTransport::send_disconnect(const TransportConnId conn_id, const Addr& addr) {
    UdpProtocol::DisconnectMsg dhdr {};

    int numSent = sendto(fd,
                         (uint8_t *)&dhdr,
                         sizeof(dhdr),
                         0 /*flags*/,
                         (struct sockaddr *) &addr.addr,
                         addr.addr_len);


    if (numSent < 0) {
        logger->error << "conn_id: " << conn_id
                      << " Error sending DISCONNECT to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != sizeof(dhdr)) {
        logger->info << "conn_id: " << conn_id
                     << " Failed to send DISCONNECT message, sent: " << numSent
                     << std::flush;
        return false;
    }

    return true;
}

bool UDPTransport::send_keepalive(ConnectionContext& conn) {
    UdpProtocol::KeepaliveMsg khdr {};

    const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));
    khdr.ticks_ms = current_tick - conn.tx_next_report_tick;

    logger->debug << "conn_id: " << conn.id
                  << " send KEEPALIVE" << std::flush;

    int numSent = sendto(fd,
                         (uint8_t *)&khdr,
                         sizeof(khdr),
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);


    if (numSent < 0) {
        logger->error << "conn_id: " << conn.id
                      << " Error sending KEEPALIVE to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != sizeof(khdr)) {
        logger->info << "conn_id: " << conn.id
                     << " Failed to send KEEPALIVE message, sent: " << numSent
                     << std::flush;
        return false;
    }

    return true;
}

bool UDPTransport::send_report(ConnectionContext& conn) {
    int numSent = sendto(fd,
                         (uint8_t *)&conn.report,
                         sizeof(conn.report),
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);


    if (numSent < 0) {
        logger->error << "conn_id: " << conn.id
                      << " Error sending REPORT to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != sizeof(conn.report)) {
        logger->info << "conn_id: " << conn.id
                     << " Failed to send REPORT message, sent: " << numSent
                     << std::flush;
        return false;
    }

    conn.report.metrics.duration_ms = 0;
    conn.report.metrics.total_bytes = 0;
    conn.report.metrics.total_packets = 0;

    return true;
}


bool UDPTransport::send_data(ConnectionContext& conn, const ConnData& cd, bool discard) {
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

    const auto data_len = sizeof(dhdr) + cd.data.size();
    if (data_len > sizeof(data)) {
        logger->error << "conn_id: " << conn.id << " data_len: " << data_len << " is too large" << std::flush;
        return false;
    }

    if (discard) {
        logger->debug << "Sending discard data size: " << cd.data.size() << std::flush;
    }

    memcpy(data, &dhdr, sizeof(dhdr));
    memcpy(data + sizeof(dhdr), cd.data.data(), cd.data.size());

    int numSent = sendto(fd,
                         data,
                         data_len,
                         0 /*flags*/,
                         (struct sockaddr *) &conn.addr.addr,
                         conn.addr.addr_len);

    if (numSent < 0) {
        logger->error << "conn_id: " << conn.id
                      << " Error sending DATA to UDP socket: " << strerror(errno)
                      << std::flush;

        return false;

    } else if (numSent != data_len) {
        logger->info << "conn_id: " << conn.id
                     << " Failed to send DATA len: " << data_len << ", sent: " << numSent
                     << std::flush;
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

    logger->Log("Starting transport writer thread");

    bool sent_data = false;
    int all_empty_count = 0;
    std::unique_lock<std::mutex> lock(_writer_mutex);

    while (not stop) {
        sent_data = false;

        bool unlock = true;

        // Check each connection context for data to send
        for (const auto& [conn_id, conn]: conn_contexts) {
            // NOTE: Currently only implement single data context
            auto& data_ctx = conn->data_contexts[0];

            const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds (1));

            // Check if idle
            if (conn->last_rx_msg_tick && current_tick - conn->last_rx_msg_tick >= conn->idle_timeout_ms) {
                logger->error << "conn_id: " << conn_id << " TIME OUT, disconnecting connection" << std::flush;
                unlock = false;
                lock.unlock();
                close(conn_id);
                break; // Don't continue with for loop since iterator will be invalidated upon close
            }

            // Shape flow by only processing data if wait for tick value is less than or equal to current tick
            if (conn->wait_for_tick > current_tick) {
                continue;
            }

            if (data_ctx.tx_data->empty()) { // No data, go to next connection
                // Send keepalive if needed
                if (conn->last_tx_msg_tick && current_tick - conn->last_tx_msg_tick > conn->ka_interval_ms) {
                    conn->last_tx_msg_tick = current_tick;
                    send_keepalive(*conn);
                }
                continue;
            }

            auto cd = data_ctx.tx_data->pop_front();

            if (!cd.has_value) {
                // Send keepalive if needed
                if (conn->last_tx_msg_tick && current_tick - conn->last_tx_msg_tick > conn->ka_interval_ms) {
                    conn->last_tx_msg_tick = current_tick;
                    send_keepalive(*conn);
                }

                sent_data = true; // Don't treat this as data not sent, which causes a pause
                continue; // Data maybe null if time queue has a delay in pop
            }

            data_ctx.tx_queue_expired += cd.expired_count;

            cd.value.trace.push_back({"transport_udp:send_data", cd.value.trace.front().start_time});

            if (!cd.value.trace.empty() && cd.value.trace.back().delta > 60000) {

                logger->info << "MethodTrace conn_id: " << cd.value.conn_id
                             << " data_ctx_id: " << cd.value.data_ctx_id
                             << " priority: " << static_cast<int>(cd.value.priority);
                for (const auto &ti: cd.value.trace) {
                    logger->info << " " << ti.method << ": " << ti.delta << " ";
                }

                logger->info << " total_duration: " << cd.value.trace.back().delta << std::flush;
            }

            if (! send_data(*conn, cd.value, (cd.value.priority ? false : true))) {
                continue;
            }

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

    logger->Log("Done transport writer thread");
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
    logger->Log(cantina::LogLevel::Info, "Starting transport reader thread");
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

    while (not stop) {
        Addr remote_addr;

        int rLen = recvfrom(fd,
                            data,
                            dataSize,
                            0 /*flags*/,
                            (struct sockaddr *) &remote_addr.addr,
                            &remote_addr.addr_len);

        if (rLen < 0 || stop) {
            if ((errno == EAGAIN) || (stop)) {
                // timeout on read or stop issued
                continue;

            } else {
                logger->error << "Error reading from UDP socket: " << strerror(errno)
                              << std::flush;
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
        const auto a_conn_it = addr_conn_contexts.find(remote_addr.id);

        switch (static_cast<UdpProtocol::ProtocolType>(data[1])) { // Process based on type of message
            case UdpProtocol::ProtocolType::CONNECT: {
                UdpProtocol::ConnectMsg chdr;
                memcpy(&chdr, data, sizeof(chdr));

                if (chdr.idle_timeout == 0) {
                    // TODO: Add metric for invalid idle_timeout
                    logger->debug << "Invalid zero idle timeout for new connection, ignoring" << std::flush;
                    lock.unlock();
                    continue;
                }

                if (a_conn_it == addr_conn_contexts.end()) { // New connection
                    if (isServerMode) {
                        ++last_conn_id;

                        send_connect_ok(last_conn_id, remote_addr);

                        const auto [conn_it, _] = conn_contexts.emplace(last_conn_id,
                                                                        std::make_shared<ConnectionContext>());

                        auto &conn = *conn_it->second;

                        createDataContext(last_conn_id, false, 10, false);

                        conn.addr = remote_addr;
                        conn.id = last_conn_id;

                        conn.tx_report_interval_ms = tconfig.time_queue_rx_size; // TODO: this temp to set this via UI

                        conn.last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                        conn.idle_timeout_ms = chdr.idle_timeout * 1000;
                        conn.ka_interval_ms = conn.idle_timeout_ms / 3;

                        // TODO: Consider adding BW in connect message to convey what the receiver would like to receive
                        conn.set_KBps(6250); // Set to 50Mbps connection rate

                        addr_conn_contexts.emplace(remote_addr.id, conn_it->second); // Add to the addr lookup map

                        lock.unlock(); // no need to hold lock, especially with a call to a delegate

                        // New remote address/connection
                        const TransportRemote remote = create_addr_remote(remote_addr.addr);

                        // Notify caller that there is a new connection
                        delegate.on_new_connection(last_conn_id, std::move(remote));
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
                if (!isServerMode) {
                    clientStatus = TransportStatus::Ready;
                }

                if (a_conn_it != addr_conn_contexts.end()) {
                    logger->info << "conn_id: " << a_conn_it->second->id
                                 << " received CONNECT_OK" << std::flush;

                    a_conn_it->second->status = TransportStatus::Ready;
                }

                break;
            }
            case UdpProtocol::ProtocolType::DISCONNECT: {
                if (!isServerMode) {
                    clientStatus = TransportStatus::Disconnected;
                }

                if (a_conn_it != addr_conn_contexts.end()) {
                    logger->info << "conn_id: " << a_conn_it->second->id
                                 << " received DISCONNECT" << std::flush;

                    a_conn_it->second->status = TransportStatus::Disconnected;
                    lock.unlock();
                    const auto conn_id = a_conn_it->second->id;
                    close(conn_id);
                    continue;
                }
                break;
            }
            case UdpProtocol::ProtocolType::KEEPALIVE: {
                if (a_conn_it != addr_conn_contexts.end()) {
                    a_conn_it->second->last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                    UdpProtocol::KeepaliveMsg hdr;
                    memcpy(&hdr, data, sizeof(hdr));

                    a_conn_it->second->last_rx_hdr_tick = hdr.ticks_ms;
                }
                break;
            }
            case UdpProtocol::ProtocolType::REPORT: {
                if (a_conn_it != addr_conn_contexts.end()) {
                    a_conn_it->second->last_rx_msg_tick = _tick_service->get_ticks(std::chrono::milliseconds(1));

                    UdpProtocol::ReportMessage hdr;
                    memcpy(&hdr, data, sizeof(hdr));

                    if (hdr.metrics.total_bytes == 0 || hdr.metrics.duration_ms == 0) {
                        lock.unlock();
                        continue;
                    }

                    const auto& prev_report = a_conn_it->second->tx_prev_reports[(hdr.report_id % a_conn_it->second->tx_prev_reports.size())];
                    if (prev_report.report_id != hdr.report_id) {
                        logger->warning << "Received report id: " << hdr.report_id
                                        << " is not previous id: " << prev_report.report_id
                                        << " sizeof array: " << sizeof(a_conn_it->second->tx_prev_reports)
                                        << " prev_index: " << (hdr.report_id % a_conn_it->second->tx_prev_reports.size())
                                        << std::flush;
                        lock.unlock();
                        continue;
                    }

                    const auto send_KBps = static_cast<int>(prev_report.metrics.total_bytes / prev_report.metrics.duration_ms);
                    //const auto ack_KBps = static_cast<int>(hdr.metrics.total_bytes / hdr.metrics.duration_ms);
                    const auto ack_KBps = static_cast<int>(hdr.metrics.total_bytes / hdr.metrics.duration_ms); //std::max(prev_report.metrics.duration_ms, hdr.metrics.duration_ms));
                    const auto prev_KBps = (a_conn_it->second->bytes_per_us * 1'000'000 / 1024);
                    const auto loss_pct = 1.0 - static_cast<double>(hdr.metrics.total_packets) / prev_report.metrics.total_packets;
                    a_conn_it->second->tx_report_ott = hdr.metrics.recv_ott_ms;

                    if (loss_pct >= 0.01 && hdr.metrics.total_packets > 10) {
                        logger->info << "Received REPORT (decrease) conn_id: " << a_conn_it->second->id
                                     << " tx_report_id: " << hdr.report_id
                                     << " duration_ms: " << hdr.metrics.duration_ms
                                     << " (" << prev_report.metrics.duration_ms << ")"
                                     << " total_bytes: " << hdr.metrics.total_bytes
                                     << " (" << prev_report.metrics.total_bytes << ")"
                                     << " total_packets: " << hdr.metrics.total_packets
                                     << " (" << prev_report.metrics.total_packets << ")"
                                     << " send/ack Kbps: " << send_KBps * 8 << " / " << ack_KBps * 8
                                     << " prev_Kbps: " << prev_KBps * 8
                                     << " Loss: " << loss_pct << "%"
                                     << " TX-OTT: " << hdr.metrics.recv_ott_ms << "ms"
                                     << " RX-OTT: " << a_conn_it->second->rx_report_ott << "ms"
                                     << " expired_count: " << a_conn_it->second->data_contexts[0].tx_queue_expired
                                     << std::flush;

                        a_conn_it->second->tx_zero_loss_count = 0;
                        a_conn_it->second->set_KBps(ack_KBps * .95);

                    } else if (hdr.metrics.total_packets > 10 && loss_pct == 0) {
                        a_conn_it->second->tx_zero_loss_count++;

                        // Only increase bandwidth if there is no loss for a little while
                        if (a_conn_it->second->tx_zero_loss_count > 5
                            && a_conn_it->second->set_KBps(ack_KBps * 1.03, true)) {

                            logger->info << "Received REPORT (increase) conn_id: " << a_conn_it->second->id
                                         << " prev_report_id: " << a_conn_it->second->tx_report_id - 1
                                         << " tx_report_id: " << hdr.report_id
                                         << " duration_ms: " << hdr.metrics.duration_ms
                                         << " (" << prev_report.metrics.duration_ms << ")"
                                         << " total_bytes: " << hdr.metrics.total_bytes
                                         << " (" << prev_report.metrics.total_bytes << ")"
                                         << " total_packets: " << hdr.metrics.total_packets
                                         << " (" << prev_report.metrics.total_packets << ")"
                                         << " send/ack Kbps: " << send_KBps * 8 << " / " << ack_KBps * 8
                                         << " prev_Kbps: " << prev_KBps * 8
                                         << " Loss: " << loss_pct << "%"
                                         << " TX-OTT: " << hdr.metrics.recv_ott_ms << "ms"
                                         << " RX-OTT: " << a_conn_it->second->rx_report_ott << "ms"
                                         << " expired_count: " << a_conn_it->second->data_contexts[0].tx_queue_expired
                                         << std::flush;

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
                                a_conn_it->second->data_contexts[0].tx_data->push(cd, 6, 0, 0);
                            }

                            a_conn_it->second->tx_zero_loss_count = 2;
                        }
                    }
                }
                break;
            }

            case UdpProtocol::ProtocolType::DATA: {
                UdpProtocol::DataMsg hdr;
                memcpy(&hdr, data, sizeof(hdr));
                rLen -= sizeof(hdr);

                if (a_conn_it != addr_conn_contexts.end()) {
                    if (hdr.report_id != a_conn_it->second->report.report_id &&
                            (hdr.report_id > a_conn_it->second->report.report_id
                             || hdr.report_id == 0 || a_conn_it->second->report.report_id - hdr.report_id > 1)) {

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

                    // Only send data if not set to discard
                    if (!hdr.flags.discard) {
                        std::vector<uint8_t> buffer(data + sizeof(hdr), data + sizeof(hdr) + rLen);

                        std::vector<MethodTraceItem> trace;
                        const auto start_time = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now());

                        trace.push_back({"transport_udp:recv_data", start_time});
                        ConnData cd { a_conn_it->second->id, 0, 2,
                                      std::move(buffer), std::move(trace)};
                        cd.trace.reserve(10);

                        a_conn_it->second->data_contexts[0].rx_data.push(cd);

                        lock.unlock();
                        if ( a_conn_it->second->data_contexts[0].rx_data.size() < 10
                            || a_conn_it->second->data_contexts[0].in_data_cb_skip_count > 20) {

                            delegate.on_recv_notify(cd.conn_id, cd.data_ctx_id);
                        } else {
                            a_conn_it->second->data_contexts[0].in_data_cb_skip_count++;
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

    logger->Log("Done transport reader thread");
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

    const auto conn_it = conn_contexts.find(conn_id);

    if (conn_it == conn_contexts.end()) {
        // Invalid connection id
        return TransportError::InvalidConnContextId;
    }

    const auto data_ctx_it = conn_it->second->data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_it->second->data_contexts.end()) {
        // Invalid data context id
        return TransportError::InvalidDataContextId;
    }

    const auto trace_start_time = trace.front().start_time;
    ConnData cd { conn_id,
                  data_ctx_id,
                  priority,
                  std::move(bytes),
                  std::move(trace)};

    data_ctx_it->second.tx_data->push(std::move(cd), ttl_ms, priority, delay_ms);

    return TransportError::None;
}

std::optional<std::vector<uint8_t>> UDPTransport::dequeue(const TransportConnId &conn_id,
                                                          const DataContextId &data_ctx_id) {

    std::lock_guard<std::mutex> _(_reader_mutex);

    const auto conn_it = conn_contexts.find(conn_id);
    if (conn_it == conn_contexts.end()) {
        logger->warning << "dequeue: invalid conn_id: " << conn_id
                        << std::flush;
        // Invalid context id
        return std::nullopt;
    }

    const auto data_ctx_it = conn_it->second->data_contexts.find(data_ctx_id);
    if (data_ctx_it == conn_it->second->data_contexts.end()) {
        logger->error << "dequeue: invalid stream for conn_id: " << conn_id
                      << " data_ctx_id: " << data_ctx_id << std::flush;

        return std::nullopt;
    }

    if (auto cd = data_ctx_it->second.rx_data.pop()) {
        cd->trace.push_back({"transport_udp:dequeue", cd->trace.front().start_time});

        if (!cd->trace.empty() && cd->trace.back().delta > 1500) {
            logger->info << "MethodTrace conn_id: " << cd->conn_id
                         << " data_ctx_id: " << cd->data_ctx_id
                         << " priority: " << static_cast<int>(cd->priority);
            for (const auto &ti: cd->trace) {
                logger->info << " " << ti.method << ": " << ti.delta << " ";
            }

            logger->info << " total_duration: " << cd->trace.back().delta << std::flush;
        }

        return std::move(cd.value().data);
    }

    return std::nullopt;
}

TransportConnId UDPTransport::connect_client() {
    std::stringstream s_log;

    clientStatus = TransportStatus::Connecting;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
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

    err = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    snd_rcv_max = UDP_MAX_PACKET_SIZE * 16; // TODO: Add config for value
    err = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }
#endif

    err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
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
    err = bind(fd, (struct sockaddr *) &srvAddr, sizeof(srvAddr));
    if (err) {
        s_log << "client_connect: Unable to bind to socket: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    std::string sPort = std::to_string(htons(serverInfo.port));
    struct addrinfo hints = {}, *address_list = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    err = getaddrinfo(
            serverInfo.host_or_ip.c_str(), sPort.c_str(), &hints, &address_list);
    if (err) {
        strerror(1);
        s_log << "client_connect: Unable to resolve remote ip address: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
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
        logger->critical << "client_connect: No IP address found" << std::flush;
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *) &serverAddr.addr;
    memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
    ipv4->sin_port = htons(serverInfo.port);
    serverAddr.addr_len = sizeof(sockaddr_in);

    freeaddrinfo(address_list);

    serverAddr.id = create_addr_id(serverAddr.addr);

    ++last_conn_id;

    std::lock_guard<std::mutex> _(_writer_mutex);  // just for safety in case a close is called at the same time

    const auto& [conn_it, is_new] = conn_contexts.emplace(last_conn_id,
                                                    std::make_shared<ConnectionContext>());

    auto &conn = *conn_it->second;
    conn.addr = serverAddr;
    conn.id = last_conn_id;

    conn.tx_report_interval_ms = tconfig.time_queue_rx_size; // TODO: this temp to set this via UI

    conn.set_KBps(2000); // Set to 16Mbps=2000KBps connection rate

    createDataContext(last_conn_id, false, 10, false);

    addr_conn_contexts.emplace(serverAddr.id, conn_it->second); // Add to the addr lookup map

    send_connect(conn.id, conn.addr);

    // Notify caller that the connection is now ready
    delegate.on_connection_status(last_conn_id, TransportStatus::Ready);


#if defined(PLATFORM_ESP)
    auto cfg = create_config("FDReader", 1, 12 * 1024, 5);
    auto esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
        abort();
    }
#endif

    running_threads.emplace_back(&UDPTransport::fd_reader, this);

#if defined(PLATFORM_ESP)
    cfg = create_config("FDWriter", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif

    running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return last_conn_id;
}

TransportConnId UDPTransport::connect_server() {
    std::stringstream s_log;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        s_log << "connect_server: Unable to create socket: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    // set for re-use
    int one = 1;
    int err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    if (err != 0) {
        s_log << "connect_server: setsockopt error: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif

    }

    // TODO: Add config for this value
    size_t snd_rcv_max = UDP_MAX_PACKET_SIZE * 16;
    timeval rcv_timeout{.tv_sec = 0, .tv_usec = 1000};

    err = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }

    err = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif

    }

    err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
#if not defined(PLATFORM_ESP)
        throw std::runtime_error(s_log.str());
#else
        abort();
#endif
    }


    struct sockaddr_in srv_addr;
    memset((char *) &srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_port = htons(serverInfo.port);
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    err = bind(fd, (struct sockaddr *) &srv_addr, sizeof(srv_addr));
    if (err < 0) {
        s_log << "connect_server: unable to bind to socket: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    logger->info << "connect_server: port: " << serverInfo.port << " fd: " << fd
                 << std::flush;

#if defined(PLATFORM_ESP)
    cfg = create_config("FDServerReader", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif
    running_threads.emplace_back(&UDPTransport::fd_reader, this);

#if defined(PLATFORM_ESP)
    cfg = create_config("FDServerWriter", 1, 12 * 1024, 5);
    esp_err = esp_pthread_set_cfg(&cfg);
    if(esp_err != ESP_OK) {
     abort();
    }
#endif

    running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return last_conn_id;
}
