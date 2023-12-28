/**
 *  time_queue.h
 *
 *  Copyright (C) 2023
 *  Cisco Systems, Inc.
 *  All Rights Reserved.
 *
 *  Description:
 *      A time based queue, where the length of the queue is a duration,
 *      divided into buckets based on a given time interval. As time
 *      progresses, buckets in the past are cleared, and the main queue
 *      is updated so that the front only returns a valid object that
 *      has not expired. To improve performance, buckets are only cleared
 *      on push or pop operations. Thus, buckets in the past can be
 *      cleared in bulk based on how many we should have advanced since
 *      the last time we updated.
 *
 *  Portability Issues:
 *      None.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace qtransport {

    /**
     * Interface for services that calculate ticks.
     */
    struct tick_service
    {
        using tick_type = size_t;
        using duration_type = std::chrono::microseconds;

        virtual tick_type get_ticks(const duration_type& interval) const = 0;
    };

    /**
     * @brief Calculates elapsed time in ticks.
     *
     * @details Calculates time that's elapsed between update calls. Keeps
     *          track of time using ticks as a counter of elapsed time. The
     *          precision 500us or greater, which results in the tick interval
     *          being >= 500us.
     */
    class threaded_tick_service : public tick_service
    {
        using clock_type = std::chrono::steady_clock;

      public:
        threaded_tick_service() { _tick_thread = std::thread(&threaded_tick_service::tick_loop, this); }

        threaded_tick_service(const threaded_tick_service& other)
          : _ticks{ other._ticks.load() }
          , _stop{ other._stop.load() }
        {
            _tick_thread = std::thread(&threaded_tick_service::tick_loop, this);
        }

        ~threaded_tick_service()
        {
            _stop = true;
            if (_tick_thread.joinable())
                _tick_thread.join();
        }

        threaded_tick_service& operator=(const threaded_tick_service& other)
        {
            _ticks = other._ticks.load();
            _stop = other._stop.load();
            _tick_thread = std::thread(&threaded_tick_service::tick_loop, this);
            return *this;
        }

        tick_type get_ticks(const duration_type& interval) const override
        {
            const tick_type increment = std::max(interval, _interval) / _interval;
            return _ticks / increment;
        }

      private:
        void tick_loop()
        {
            const int interval_us = _interval.count();

            timeval sleep_time = {.tv_sec = 0, .tv_usec = interval_us};
            while (!_stop) {
                select(1, NULL, NULL, NULL, &sleep_time);
                sleep_time.tv_usec = interval_us;
                ++_ticks;
            }
        }

      private:
        /// The current ticks since the tick_service began.
        std::atomic<uint64_t> _ticks{ 0 };

        /// Flag to stop tick_service thread.
        std::atomic<bool> _stop{ false };

        /// The interval at which ticks should increase.
        const duration_type _interval{ 500 };

        /// The thread to update ticks on.
        std::thread _tick_thread;
    };

    /**
     * @brief Aging element FIFO queue.
     *
     * @details Time based queue that maintains the push/pop order, but expires older values given a specific ttl.
     *
     * @tparam T            The element type to be stored.
     * @tparam Duration_t   The duration type to check for. All the variables that are interval, duration, ttl, ...
     *                      are of this unit. Ticks are of this unit. For example, setting to millisecond will define
     *                      the unit for ticks and all associated variables to be millisecond.
     */
    template<typename T, typename Duration_t>
    class time_queue
    {
        /*=======================================================================*/
        // Time queue type assertions
        /*=======================================================================*/

        template<typename>
        struct is_chrono_duration : std::false_type
        {};

        template<typename Rep, typename Period>
        struct is_chrono_duration<std::chrono::duration<Rep, Period>> : std::true_type
        {};

        static_assert(is_chrono_duration<Duration_t>::value);

        /*=======================================================================*/
        // Internal type definitions
        /*=======================================================================*/

        using tick_type = tick_service::tick_type;
        using bucket_type = std::vector<T>;
        using index_type = std::uint32_t;

        struct queue_value_type
        {
            queue_value_type(bucket_type& bucket, index_type value_index, tick_type expiry_tick)
              : _bucket{ bucket }
              , _value_index{ value_index }
              , _expiry_tick(expiry_tick)
            {
            }

            bucket_type& _bucket;
            index_type _value_index;
            tick_type _expiry_tick;
        };

        using queue_type = std::vector<queue_value_type>;

      public:
        /**
         * @brief Construct a time_queue with defaults or supplied parameters
         *
         * @param duration      Duration of the queue in Duration_t. Value must be > 0, and != interval.
         * @param interval      Interval of ticks in Duration_t. Must be > 0, < duration, duration % interval == 0.
         * @param tick_service  Shared pointer to tick_service service.
         *
         * @throws std::invalid_argument    If the duration or interval do not meet requirements or If the tick_service
         * is null.
         */
        time_queue(size_t duration, size_t interval, const std::shared_ptr<tick_service>& tick_service)
          : _duration{ duration }
          , _interval{ interval }
          , _total_buckets{ _duration / _interval }
          , _tick_service(tick_service)
        {
            if (duration == 0 || duration % interval != 0 || duration == interval) {
                throw std::invalid_argument("Invalid time_queue constructor args");
            }

            if (!tick_service) {
                throw std::invalid_argument("Tick service cannot be null");
            }

            _buckets.resize(_total_buckets);
            _queue.reserve(_total_buckets);
        }

        /**
         * @brief Construct a time_queue with defaults or supplied parameters
         *
         * @param duration              Duration of the queue in Duration_t. Value must be > 0, and != interval.
         * @param interval              Interval of ticks in Duration_t. Value must be > 0, < duration, duration %
         *                              interval == 0.
         * @param tick_service          Shared pointer to tick_service.
         * @param initial_queue_size    Initial size of the queue to reserve.
         *
         * @throws std::invalid_argument If the duration or interval do not meet requirements or the tick_service is
         * null.
         */
        time_queue(size_t duration,
                   size_t interval,
                   const std::shared_ptr<tick_service>& tick_service,
                   size_t initial_queue_size)
          : time_queue(duration, interval, tick_service)
        {
            _queue.reserve(initial_queue_size);
        }

        time_queue() = delete;
        time_queue(const time_queue&) = default;
        time_queue(time_queue&&) = default;

        time_queue& operator=(const time_queue&) = default;
        time_queue& operator=(time_queue&&) = default;

        /**
         * @brief Pushes a new value onto the queue with a time-to-live.
         *
         * @param value The value to push onto the queue.
         * @param ttl   Time to live for an object using the unit of Duration_t
         *
         * @throws std::invalid_argument If ttl is greater than duration.
         */
        void push(const T& value, size_t ttl) { internal_push(value, ttl); }

        /**
         * @brief Pushes a new value onto the queue with a time-to-live.
         *
         * @param value The value to push onto the queue.
         * @param ttl   Time to live for an object using the unit of Duration_t
         *
         * @throws std::invalid_argument If ttl is greater than duration.
         */
        void push(T&& value, size_t ttl) { internal_push(std::move(value), ttl); }

        /**
         * @brief Pop (increment) front
         *
         * @details This method should be called after front when the object is processed. This
         *      will move the queue forward. If at the end of the queue, it'll be cleared and reset.
         */
        void pop() noexcept
        {
            if (_queue.empty() || ++_queue_index < _queue.size())
                return;

            clear();
        }

        /**
         * @brief Pops (removes) the front of the queue.
         *
         * @returns The popped value, else nullopt.
         */
        [[nodiscard]] std::optional<T> pop_front()
        {
            if (auto obj = front()) {
                pop();
                return obj;
            }

            return std::nullopt;
        }

        /**
         * @brief Returns the most valid front of the queue without popping.
         * @returns The front value of the queue, else nullopt
         */
        [[nodiscard]] std::optional<T> front()
        {
            const tick_type ticks = advance();

            if (_queue.empty())
                return std::nullopt;

            while (_queue_index < _queue.size()) {
                auto& [bucket, value_index, expiry_tick] = _queue.at(_queue_index);

                if (value_index >= bucket.size() || ticks > expiry_tick) {
                    _queue_index++;
                    continue;
                }
                return bucket.at(value_index);
            }

            clear();

            return std::nullopt;
        }

        size_t size() const noexcept { return _queue.size() - _queue_index; }
        bool empty() const noexcept { return _queue.empty() || _queue_index >= _queue.size(); }

        /**
         * @brief Clear/reset the queue to no objects
         */
        void clear() noexcept
        {
            _queue.clear();
            _queue_index = _bucket_index = 0;

            for (auto& bucket : _buckets) {
                bucket.clear();
            }
        }

      private:


        /**
         * @brief Based on current time, adjust and move the bucket index with time
         *        (sliding window)
         *
         * @returns Current tick value at time of advance
         */
        tick_type advance()
        {
            const tick_type new_ticks = _tick_service->get_ticks(Duration_t(_interval));
            const tick_type delta = _current_ticks ? new_ticks - _current_ticks : 0;
            _current_ticks = new_ticks;

            if (delta == 0)
                return _current_ticks;

            if (delta >= static_cast<tick_type>(_total_buckets)) {
                clear();
                return _current_ticks;
            }

            for (int i = 0; i < delta; ++i) {
                _buckets[(_bucket_index + i) % _total_buckets].clear();
            }

            _bucket_index = (_bucket_index + delta) % _total_buckets;

            return _current_ticks;
        }

        /**
         * @brief Pushes new element onto the queue and adds it to future bucket.
         *
         * @details Internal definition of push. Pushes value into specified
         *          bucket, and then emplaces the location info into the queue.
         *
         * @param value The value to push onto the queue.
         * @param ttl   Time to live for an object using the unit of Duration_t
         *
         * @throws std::invalid_argument If ttl is greater than duration.
         */
        template<typename Value>
        inline void internal_push(Value value, size_t ttl)
        {
            if (ttl > _duration) {
                throw std::invalid_argument("TTL is greater than max duration");
            } else if (ttl == 0) {
                ttl = _duration;
            }

            ttl = ttl / _interval;

            const tick_type ticks = advance();

            const tick_type expiry_tick = ticks + ttl;

            const index_type future_index = (_bucket_index + ttl - 1) % _total_buckets;

            bucket_type& bucket = _buckets[future_index];

            bucket.push_back(value);
            _queue.emplace_back(bucket, bucket.size() - 1, expiry_tick);
        }

      private:
        /// The duration in ticks of the entire queue.
        const size_t _duration;

        /// The interval at which buckets are cleared in ticks.
        const size_t _interval;

        /// The total amount of buckets. Value is calculated by duration / interval.
        const size_t _total_buckets;

        /// The index in time of the current bucket.
        index_type _bucket_index{ 0 };

        /// The index of the first valid item in the queue.
        index_type _queue_index{ 0 };

        /// Last calculated tick value.
        tick_type _current_ticks{ 0 };

        /// The memory storage for all elements to be managed.
        std::vector<bucket_type> _buckets;

        /// The FIFO ordered queue of values as they were inserted.
        queue_type _queue;

        /// Tick service for calculating new tick and jumps in time.
        std::shared_ptr<tick_service> _tick_service;
    };

}; // namespace qtransport
