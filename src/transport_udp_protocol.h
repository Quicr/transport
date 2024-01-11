#pragma once

namespace qtransport {
    namespace UdpProtocol {
        /* ------------------------------------------------------------------------
         * Wire messages
         * ------------------------------------------------------------------------
         */
        constexpr uint8_t PROTOCOL_VERSION = 1;

        /**
         * @brief UDP Protocol Types
         * @details Each UDP packet is encoded with a common header, which includes a type.
         */
        enum class ProtocolType : uint8_t {
            CONNECT = 0,
            DISCONNECT = 1,
            KEEPALIVE = 2,
            DATA = 3,
            DATA_DISCARD = 4,
            REPORT = 5,
        };

        /**
         * @brief UDP Protocol Common Header
         * @brief Every UDP packet starts with this common header. The data that follows is defined by the type
         */
        struct CommonHeader {
            uint8_t version {PROTOCOL_VERSION};       /// Protocol version
            ProtocolType type;                        /// Indicates this is a peering message
        };

        /**
         * @brief Connect Message
         *
         * @details UDP Protocol starts off with a connect message. Messages will be discarded by the remote
         *      until the new connection sends a connect message.
         */
        struct ConnectMsg : CommonHeader {
            ConnectMsg() { type = ProtocolType::CONNECT; }

            uint16_t idle_timeout { 120 };            /// Idle timeout in seconds. Must not be zero

        } __attribute__((__packed__, aligned(1)));

        /**
         * @brief Disconnect Message
         *
         * @details Disconnect notification. Remote will immediately purge/close the active connection
         */
        struct DisconnectMsg : CommonHeader {
            DisconnectMsg() { type = ProtocolType::DISCONNECT; }
        } __attribute__((__packed__, aligned(1)));

        /**
         * @brief Keepalive Message
         *
         * @details Keepalive message. Sent only when no other messages have been sent in idle_timeout / 3.
         */
        struct KeepaliveMsg : CommonHeader {
            KeepaliveMsg() { type = ProtocolType::KEEPALIVE; }
        } __attribute__((__packed__, aligned(1)));

        /**
         * @brief Data Message
         *
         * @details Data message. Bytes following the header is the data
         *
         * @note Can be either type of DATA or DATA_DISCARD
         */
        struct DataMsg : CommonHeader {
            DataMsg() { type = ProtocolType::DATA; } // Can be either DATA or DATA_DISCARD type

            uint16_t report_id { 0 };           /// Report ID this data applies to
        } __attribute__((__packed__, aligned(1)));


        /**
         * @brief Report Metrics
         */
         struct ReportMetrics {

             uint32_t total_packets { 0 };       /// Total number of packets received
             uint32_t total_bytes { 0 };         /// Total number of data (sans header) bytes received
             uint32_t duration_ms { 0 };         /// Duration in milliseconds of time from first to latest packet received

         } __attribute__((__packed__, aligned(1)));

        /**
         * @brief Report Message
         *
         * @details Report message. The remote will send a report message upon
         */
        struct ReportMessage : CommonHeader {
            ReportMessage() { type = ProtocolType::REPORT; }

            uint16_t report_id { 0 };           /// Report ID of this report
            ReportMetrics metrics;

        } __attribute__((__packed__, aligned(1)));


    } // end UdpProtocol namespace
} // end qtransport namespace
