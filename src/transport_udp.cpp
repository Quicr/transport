#include <cassert>
#include <cstring> // memcpy
#include <iostream>
#include <sstream>
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

using namespace qtransport;

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

    _tick_service.reset();
}

UDPTransport::UDPTransport(const TransportRemote &server,
                           TransportDelegate &delegate,
                           bool isServerMode,
                           const cantina::LoggerPointer &logger)
        : stop(false), logger(std::make_shared<cantina::Logger>("UDP", logger)), fd(-1), isServerMode(isServerMode),
          serverInfo(server), delegate(delegate) {
    _tick_service = std::make_shared<threaded_tick_service>();
}

TransportStatus UDPTransport::status() const {
    return (fd > 0) ? TransportStatus::Ready : TransportStatus::Disconnected;
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
        data_ctx_it->second.rx_data.set_limit(1000);
        data_ctx_it->second.tx_data = std::make_unique<priority_queue<ConnData>>(1000,
                                                                                 1,
                                                                                 _tick_service,
                                                                                 1000);
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

    if (isServerMode) {
        auto conn_it = conn_contexts.find(conn_id);
        if (conn_it != conn_contexts.end()) {
            addr_conn_contexts.erase(conn_it->second->addr.id);
            conn_contexts.erase(conn_it);
        }
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
        default: {
            // IPv6
            sockaddr_in6 *s = (sockaddr_in6 *) &addr;

            remote.port = s->sin6_port;
            inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
            break;
        }
    }

    return std::move(remote);
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

    while (not stop) {
        sent_data = false;

        // Check each connection context for data to send
        for (const auto& [conn_id, conn]: conn_contexts) {
            // NOTE: Currently only implement single data context
            const auto& data_ctx = conn->data_contexts[0];

            const auto current_tick = _tick_service->get_ticks(std::chrono::milliseconds (1));

            // Shape flow by only processing data if wait for tick value is less than or equal to current tick
            if (conn->wait_for_tick > current_tick) {
                logger->info << "SHAPING conn_id: " << conn_id
                             << " running_age: " << conn->running_wait_us
                             << " wait_for_tick: " << conn->wait_for_tick
                             << " current_tick: " << current_tick
                             << std::flush;
                continue;
            }

            if (data_ctx.tx_data->empty()) // No data, go to next connection
                continue;

            auto cd = data_ctx.tx_data->pop_front();

            if (!cd) continue; // Data maybe null if queue is polled too fast

            const auto data_len = cd.value().data.size();

            int numSent = sendto(fd,
                                 cd.value().data.data(),
                                 data_len,
                                 0 /*flags*/,
                                 (struct sockaddr *) &conn->addr.addr,
                                 sizeof(sockaddr_in));


            if (numSent < 0) {
                logger->error << "conn_id: " << conn_id
                              << "Error sending on UDP socket: " << strerror(errno)
                              << std::flush;

                continue;

            } else if (numSent != data_len) {
                logger->info << "conn_id: " << conn_id
                             << "Failed to send all data data_size: " << data_len << " sent: " << numSent
                             << std::flush;
            }

            sent_data = true;

            // Calculate the wait for tick value
            conn->running_wait_us += static_cast<int>(data_len / conn->bytes_per_us);
            if (conn->running_wait_us > 1000) {
                conn->wait_for_tick = current_tick + conn->running_wait_us / 1000;
                conn->running_wait_us %= 1000; // Set running age to remainder value less than a tick
            }

            logger->info << "sent conn_id: " << conn_id
                         << " pri: " << static_cast<int>(cd->priority)
                         << " data_len: " << data_len
                         << " running_age: " << conn->running_wait_us
                         << " wait_for_tick: " << conn->wait_for_tick
                         << " current_tick: " << current_tick
                         << std::flush;
        }

        if (!sent_data) {
            all_empty_count++;

            if (all_empty_count > 5) {
                all_empty_count = 1;
                to.tv_usec = 1000;
                select(0, NULL, NULL, NULL, &to);
            }
        }
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
void UDPTransport::fd_reader() {
    logger->Log("Starting transport reader thread");

    const int dataSize = 65535; // TODO Add config var to set this value.  Sizes
    // larger than actual MTU require IP frags
    uint8_t data[dataSize];

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

        std::vector<uint8_t> buffer(data, data + rLen);

        ConnData cd;
        cd.data = buffer;
        cd.data_ctx_id = 0; // Currently only implement one data context

        remote_addr.id = create_addr_id(remote_addr.addr);

        const auto a_conn_it = addr_conn_contexts.find(remote_addr.id);

        if (a_conn_it == addr_conn_contexts.end()) { // New connection
            if (isServerMode) {
                ++last_conn_id;

                const auto [conn_it, _] = conn_contexts.emplace(last_conn_id,
                                                                 std::make_shared<ConnectionContext>());

                auto &conn = *conn_it->second;

                createDataContext(last_conn_id, false, 10, false);

                conn.addr = remote_addr;
                conn.id = last_conn_id;
                conn.set_bytes_per_us(50000); // Set to 50Mbps connection rate

                addr_conn_contexts.emplace(remote_addr.id, conn_it->second); // Add to the addr lookup map

                // New remote address/connection
                const TransportRemote remote = create_addr_remote(remote_addr.addr);

                cd.conn_id = last_conn_id;

                // Notify caller that there is a new connection
                delegate.on_new_connection(last_conn_id, std::move(remote));

                conn_it->second->data_contexts[cd.data_ctx_id].rx_data.push(cd);
                if (conn_it->second->data_contexts[cd.data_ctx_id].rx_data.size() < 4) {
                    delegate.on_recv_notify(cd.conn_id, cd.data_ctx_id);
                }

            } else {
                // Client mode doesn't support creating connections based on received
                // packets
                continue;
            }
        } else {
            cd.conn_id = a_conn_it->second->id;
            a_conn_it->second->data_contexts[0].rx_data.push(cd);
            if (a_conn_it->second->data_contexts[0].rx_data.size() < 4) {
                delegate.on_recv_notify(cd.conn_id, cd.data_ctx_id);
            }
        }
    }

    logger->Log("Done transport reader thread");
}

TransportError UDPTransport::enqueue(const TransportConnId &conn_id,
                      const DataContextId &data_ctx_id,
                      std::vector<uint8_t> &&bytes,
                      const uint8_t priority,
                      const uint32_t ttl_ms,
                      [[maybe_unused]] const EnqueueFlags flags) {
    if (bytes.empty()) {
        return TransportError::None;
    }


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

    ConnData cd;
    cd.data = bytes;
    cd.conn_id = conn_id;
    cd.data_ctx_id = data_ctx_id;
    cd.priority = priority;

    data_ctx_it->second.tx_data->push(cd, ttl_ms, priority);

    return TransportError::None;
}

std::optional<std::vector<uint8_t>> UDPTransport::dequeue(const TransportConnId &conn_id,
                                                          const DataContextId &data_ctx_id) {

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
        return cd.value().data;
    }

    return std::nullopt;
}

TransportConnId UDPTransport::connect_client() {
    std::stringstream s_log;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
        throw std::runtime_error("socket() failed");
    }

    size_t snd_rcv_max = 64000;   // TODO: Add config for value
    timeval rcv_timeout{.tv_sec = 0, .tv_usec = 1000};

    int err =
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    snd_rcv_max = 1000000; // TODO: Add config for value
    err =
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    err =
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }


    struct sockaddr_in srvAddr;
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    srvAddr.sin_port = 0;
    err = bind(fd, (struct sockaddr *) &srvAddr, sizeof(srvAddr));
    if (err) {
        s_log << "client_connect: Unable to bind to socket: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
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
        throw std::runtime_error(s_log.str());
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
        throw std::runtime_error("client_connect: No IP address found");
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *) &serverAddr.addr;
    memcpy(ipv4, found_addr->ai_addr, found_addr->ai_addrlen);
    ipv4->sin_port = htons(serverInfo.port);
    serverAddr.addr_len = sizeof(sockaddr_in);

    freeaddrinfo(address_list);

    serverAddr.id = create_addr_id(serverAddr.addr);

    ++last_conn_id;

    const auto& [conn_it, _] = conn_contexts.emplace(last_conn_id,
                                                    std::make_shared<ConnectionContext>());

    auto &conn = *conn_it->second;
    conn.addr = serverAddr;
    conn.id = last_conn_id;
    conn.set_bytes_per_us(16000); // Set to 6Mbps connection rate

    addr_conn_contexts.emplace(serverAddr.id, conn_it->second); // Add to the addr lookup map

    // Notify caller that the connection is now ready
    delegate.on_connection_status(last_conn_id, TransportStatus::Ready);

    running_threads.emplace_back(&UDPTransport::fd_reader, this);

    running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return last_conn_id;
}

TransportConnId UDPTransport::connect_server() {
    std::stringstream s_log;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        s_log << "connect_server: Unable to create socket: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    // set for re-use
    int one = 1;
    int err =
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    if (err != 0) {
        s_log << "connect_server: setsockopt error: " << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    // TODO: Add config for this value
    size_t snd_rcv_max = 2000000;
    timeval rcv_timeout{.tv_sec = 0, .tv_usec = 10000};

    err =
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set send buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    err =
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &snd_rcv_max, sizeof(snd_rcv_max));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive buffer size: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
    }

    err =
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    if (err != 0) {
        s_log << "client_connect: Unable to set receive timeout: "
              << strerror(errno);
        logger->Log(cantina::LogLevel::Critical, s_log.str());
        throw std::runtime_error(s_log.str());
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

    running_threads.emplace_back(&UDPTransport::fd_reader, this);
    running_threads.emplace_back(&UDPTransport::fd_writer, this);

    return last_conn_id;
}
