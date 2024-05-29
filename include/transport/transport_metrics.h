#pragma once

#include <chrono>

#include <transport/safe_queue.h>

namespace qtransport {
     /*
      * Min/Max/Avg structure
      */
    struct MinMaxAvg
    {
        uint64_t min {0};               /// Minimum value in period
        uint64_t max {0};               /// Maximum value in period
        uint64_t avg {0};               /// Average value in period

        uint64_t _value_sum {0};        /// Accumulating sum of values in period
        uint64_t _value_count {0};      /// Number of values in period

      auto operator<=>(const MinMaxAvg&) const = default;

        /**
         * @brief  Add value to period
         *
         * @details Add value will update min/max/avg based on the value being added.
         *
         * @param value           The value to add.
         */
        void addValue(const uint64_t value) {
            min = min ? std::min(min, value) : value;
            max = std::max(max, value);

            _value_sum += value;
            _value_count++;

            if (!_value_count) _value_count = 1;

            avg = _value_sum / _value_count;
        }

        void clear()
        {
            min = 0;
            max = 0;
            avg = 0;
            _value_sum = 0;
            _value_count = 0;
        }
    };

    /*
     *  QUIC Metrics
     */
    struct QuicConnectionMetrics
    {
        uint64_t cwin_congested{ 0 };      /// CC: Number of times CWIN is low or zero (congested)
        uint64_t _prev_cwin_congested{ 0 }; /// Previous number of times CWIN is congested
        uint64_t tx_congested {0};         /// count of times transmit connection is considered congested

        MinMaxAvg tx_rate_bps;             /// Rate in bits per second in period
        MinMaxAvg rx_rate_bps;             /// Estimated rate in bits per second in period
        MinMaxAvg tx_cwin_bytes;           /// Congestion window bytes in period
        MinMaxAvg tx_in_transit_bytes;     /// Number of bytes in transit
        MinMaxAvg rtt_us;                  /// Round trip time in microseconds in period
        MinMaxAvg srtt_us;                 /// Smooth Round trip time in microseconds in period

        uint64_t tx_retransmits{ 0 };      /// count of retransmits
        uint64_t tx_lost_pkts{0};          /// Number of lost packets sent
        uint64_t tx_timer_losses {0};      /// Packet losses detected due to timer expiring
        uint64_t tx_spurious_losses {0};   /// Number of packet lost that were later acked

        uint64_t rx_dgrams{ 0 };           /// count of datagrams received
        uint64_t rx_dgrams_bytes{ 0 };     /// Number of receive datagram bytes

        uint64_t tx_dgram_cb{ 0 };         /// count of picoquic callback for datagram can be sent
        uint64_t tx_dgram_ack{ 0 };        /// count of picoquic callback for acked datagrams
        uint64_t tx_dgram_lost{ 0 };       /// count of picoquic callback for lost datagrams
        uint64_t tx_dgram_spurious{ 0 };   /// count of picoquic callback for late/delayed dgram acks
        uint64_t tx_dgram_drops { 0 };     /// count of drops due to data context missing

        auto operator<=>(const QuicConnectionMetrics&) const = default;

        /**
         * @brief Reset metrics for period
         */
        void resetPeriod()
        {
            tx_rate_bps.clear();
            rx_rate_bps.clear();
            tx_cwin_bytes.clear();
            tx_in_transit_bytes.clear();
            rtt_us.clear();
            srtt_us.clear();
        }
    };

    struct QuicDataContextMetrics
    {
        uint64_t enqueued_objs{ 0 };            /// count of objects enqueued by the app to be transmitted

        uint64_t rx_stream_cb{ 0 };             /// count of callbacks to receive data
        uint64_t rx_stream_bytes{ 0 };          /// count of stream bytes received

        uint64_t tx_buffer_drops{ 0 };          /// Count of write buffer drops of data due to RESET request
        uint64_t tx_queue_discards{ 0 };        /// count of objects discarded due to TTL expiry or clear
        uint64_t tx_queue_expired{ 0 };         /// count of objects expired before pop/front

        uint64_t tx_delayed_callback{ 0 };      /// Count of times transmit callbacks were delayed
        uint64_t _prev_tx_delayed_callback{ 0 }; /// Previous transmit delayed callback value, set each interval
        uint64_t tx_reset_wait{ 0 };            /// count of times data context performed a reset and wait
        MinMaxAvg tx_queue_size;                /// TX queue size in period
        MinMaxAvg tx_callback_ms;               /// Callback time in milliseconds in period
        MinMaxAvg tx_object_duration_us;        /// TX object time in queue duration in microseconds

        uint64_t tx_dgrams{ 0 };                /// count of datagrams sent
        uint64_t tx_dgrams_bytes{ 0 };          /// count of datagrams sent bytes

        uint64_t tx_stream_cb{ 0 };             /// count of stream callbacks to send data
        uint64_t tx_stream_objects{ 0 };        /// count of stream objects sent
        uint64_t tx_stream_bytes{ 0 };          /// count of stream bytes sent

        constexpr auto operator<=>(const QuicDataContextMetrics&) const = default;

        /**
         * @brief Reset metrics for period
         */
        void resetPeriod()
        {
          tx_queue_size.clear();
          tx_callback_ms.clear();
          tx_object_duration_us.clear();
        }
    };

    /*
     * Custom UDP protocol metrics
     */
    struct UdpDataContextMetrics
    {
        uint64_t enqueued_objs{ 0 };

        uint64_t tx_queue_expired{ 0 }; /// count of objects expired before pop/front
        uint64_t tx_bytes{ 0 };         /// count of bytes sent
        uint64_t tx_objects{ 0 };       /// count of objects (messages) sent

        uint64_t rx_bytes{ 0 };   /// count of bytes received
        uint64_t rx_objects{ 0 }; /// count of objects received

        constexpr auto operator<=>(const UdpDataContextMetrics&) const = default;
    };

    struct UdpConnectionMetrics {
        uint64_t rx_no_context {0};                 /// count of times RX object data context doesn't exist

        uint64_t tx_no_context {0};                 /// count of times TX object data context doesn't exist
        uint64_t tx_discard_objects {0};            /// count of discard objects sent
    };


    using time_stamp_us = std::chrono::time_point<std::chrono::steady_clock, std::chrono::microseconds>;

    struct MetricsConnSample
    {
        time_stamp_us sample_time;      /// Sample time
        uint64_t conn_ctx_id {0};       /// Conn context ID
        std::optional<UdpConnectionMetrics> udp_sample;
        std::optional<QuicConnectionMetrics> quic_sample;

        MetricsConnSample()
          : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now())) {}

        MetricsConnSample(const uint64_t conn_id, const UdpConnectionMetrics udp_sample)
          : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()))
          , conn_ctx_id(conn_id)
          , udp_sample(udp_sample) {}

        MetricsConnSample(const time_stamp_us sample_time, const uint64_t conn_id, const UdpConnectionMetrics udp_sample)
          : sample_time(sample_time)
          , conn_ctx_id(conn_id)
          , udp_sample(udp_sample) {}

        MetricsConnSample(const uint64_t conn_id, const QuicConnectionMetrics quic_sample)
          : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()))
          , conn_ctx_id(conn_id)
          , quic_sample(quic_sample) {}

        MetricsConnSample(const time_stamp_us sample_time, const uint64_t conn_id, const QuicConnectionMetrics quic_sample)
          : sample_time(sample_time)
          , conn_ctx_id(conn_id)
          , quic_sample(quic_sample) {}
    };

    struct MetricsDataSample
    {
      time_stamp_us sample_time;      /// Sample time
      uint64_t conn_ctx_id {0};       /// Conn context ID
      uint64_t data_ctx_id {0};       /// Data context ID
      std::optional<UdpDataContextMetrics> udp_sample;
      std::optional<QuicDataContextMetrics> quic_sample;

      MetricsDataSample()
        : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now())) {}

      MetricsDataSample(const uint64_t conn_id, const uint64_t data_id, const UdpDataContextMetrics udp_sample)
        : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()))
        , conn_ctx_id(conn_id)
        , data_ctx_id(data_id)
        , udp_sample(udp_sample) {}

      MetricsDataSample(const time_stamp_us sample_time, const uint64_t conn_id, const uint64_t data_id,
                        const UdpDataContextMetrics udp_sample)
        : sample_time(sample_time)
        , conn_ctx_id(conn_id)
        , data_ctx_id(data_id)
        , udp_sample(udp_sample) {}

      MetricsDataSample(const uint64_t conn_id, const uint64_t data_id, const QuicDataContextMetrics quic_sample)
        : sample_time(std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()))
        , conn_ctx_id(conn_id)
        , data_ctx_id(data_id)
        , quic_sample(quic_sample) {}

      MetricsDataSample(const time_stamp_us sample_time, const uint64_t conn_id, const uint64_t data_id,
                        const QuicDataContextMetrics quic_sample)
        : sample_time(sample_time)
        , conn_ctx_id(conn_id)
        , data_ctx_id(data_id)
        , quic_sample(quic_sample) {}
    };


    constexpr uint64_t METRICS_INTERVAL_US = 5'000'000;             /// Metrics interval for samples in microseconds
    constexpr size_t MAX_METRICS_SAMPLES_QUEUE = 500;               /// Max metric samples pending to be written

} // end namespace qtransport