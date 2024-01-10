
#pragma once

#include <cassert>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <transport/transport.h>

#include "transport/priority_queue.h"
#include "transport/safe_queue.h"

namespace qtransport {

    struct AddrId {
        uint64_t ip_hi;
        uint64_t ip_lo;
        uint16_t port;

        AddrId() {
            ip_hi = 0;
            ip_lo = 0;
            port = 0;
        }

        bool operator==(const AddrId &o) const {
            return ip_hi == o.ip_hi && ip_lo == o.ip_lo && port == o.port;
        }

        bool operator<(const AddrId &o) const {
            return std::tie(ip_hi, ip_lo, port) < std::tie(o.ip_hi, o.ip_lo, o.port);
        }
    };

    struct ConnData {
        TransportConnId conn_id;
        DataContextId data_ctx_id;
        uint8_t priority;
        std::vector<uint8_t> data;
    };

    class UDPTransport : public ITransport {
    public:
        UDPTransport(const TransportRemote &server,
                     TransportDelegate &delegate,
                     bool isServerMode,
                     const cantina::LoggerPointer &logger);

        virtual ~UDPTransport();

        TransportStatus status() const override;

        TransportConnId start() override;

        void close(const TransportConnId &conn_id) override;

        virtual bool getPeerAddrInfo(const TransportConnId &conn_id,
                                     sockaddr_storage *addr) override;

        DataContextId createDataContext(const TransportConnId conn_id,
                                        bool use_reliable_transport,
                                        uint8_t priority, bool bidir) override;

        void deleteDataContext(const TransportConnId &conn_id, DataContextId data_ctx_id) override;

        TransportError enqueue(const TransportConnId &conn_id,
                               const DataContextId &data_ctx_id,
                               std::vector<uint8_t> &&bytes,
                               const uint8_t priority,
                               const uint32_t ttl_ms,
                               const EnqueueFlags flags) override;

        std::optional<std::vector<uint8_t>>
        dequeue(const TransportConnId &conn_id, const DataContextId &data_ctx_id) override;

    private:
        TransportConnId connect_client();

        TransportConnId connect_server();

        TransportRemote create_addr_remote(const sockaddr_storage& addr);

        AddrId create_addr_id(const sockaddr_storage& addr);

        void fd_reader();

        void fd_writer();

        bool stop;
        std::vector<std::thread> running_threads;

        struct Addr {
            socklen_t addr_len;
            struct sockaddr_storage addr {0};
            AddrId id;

            Addr() {
                addr_len = sizeof(addr);
            }
        };

        struct DataContext {
            DataContextId data_ctx_id{0};
            uint8_t priority {10};

            safe_queue<ConnData> rx_data;
            std::unique_ptr<priority_queue<ConnData>> tx_data;
        };

        struct ConnectionContext {
            Addr addr;
            TransportConnId id;                     // This/conn ID
            DataContextId next_data_ctx_id{1};
            std::map<DataContextId, DataContext> data_contexts;

            // Shaping variables
            uint64_t wait_for_tick {0};
            uint64_t running_wait_us {0};   // Running wait time in microseconds - When more than 1ms, the wait for tick will be updated

            double bytes_per_us {6.4};     // Default to 50Mbps

            void set_bytes_per_us(uint32_t Kbps) {
                const auto bytes_per_sec = (Kbps * 1024) / 8;
                bytes_per_us = ((Kbps * 1024) / 8) / 1'000'000.0 /* 1 second double value */;
            }
        };

        cantina::LoggerPointer logger;
        int fd; // UDP socket
        bool isServerMode;

        TransportRemote serverInfo;
        Addr serverAddr;

        TransportDelegate &delegate;

        TransportConnId last_conn_id{0};
        std::map<TransportConnId, std::shared_ptr<ConnectionContext>> conn_contexts;
        std::map<AddrId, std::shared_ptr<ConnectionContext>> addr_conn_contexts;

        std::shared_ptr<tick_service> _tick_service;
    };

} // namespace qtransport
