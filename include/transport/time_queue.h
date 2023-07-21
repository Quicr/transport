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
     * @brief Interface class for the Queue Timer Service
     *
     * @details Queue timer service keeps track of time using ticks as a counter of
     *      elapsed time. The precision 500us or greater. This results in the lowest
     *      tick interval to be 500us or greater.
     */
    class queue_timer_service
    {
      public:
        using tick_type = int;
        using duration_t = std::chrono::microseconds;

        /**
         * Timer context that caller constructs and passes to timer
         * to be updated.
         */
        struct timer_context
        {
            tick_type delta;          /// Delta (distance) in ticks since last call
            tick_type ticks;          /// Current tick value when updated
            tick_type previous_ticks; /// Previous tick value when last updated
        };

        virtual void get_ticks(const duration_t& interval, timer_context& ctx) = 0;
    };

    /**
     * Calculates time that's elapsed between update calls and calculates the
     * current bucket index advance distance.
     */
    class queue_timer_thread : public queue_timer_service
    {
      private:
        /*=======================================================================*/
        // Internal type definitions
        /*=======================================================================*/
        using clock_type = std::chrono::high_resolution_clock;

      public:
        /**
         * @brief
         * @param interval The interval at which ticks should update.
         */
        queue_timer_thread() { _tick_thread = std::thread(&queue_timer_thread::tick_loop, this); }

        queue_timer_thread(const queue_timer_thread& other)
          : _ticks{ other._ticks.load() }
          , _stop{ other._stop.load() }
        {
            _tick_thread = std::thread(&queue_timer_thread::tick_loop, this);
        }

        ~queue_timer_thread()
        {
            _stop = true;
            if (_tick_thread.joinable())
                _tick_thread.join();
        }

        queue_timer_thread& operator=(const queue_timer_thread& other)
        {
            _ticks = other._ticks.load();
            _stop = other._stop.load();
            _tick_thread = std::thread(&queue_timer_thread::tick_loop, this);
            return *this;
        }

        void get_ticks(const duration_t& interval, timer_context& ctx) override
        {
            auto increment = std::max(interval, _interval) / _interval;

            ctx.ticks = _ticks / increment;

            ctx.delta = ctx.previous_ticks > 0 ? ctx.ticks - ctx.previous_ticks : 0;

            ctx.previous_ticks = ctx.ticks;
        }

      private:
        void tick_loop()
        {
            const auto check_delay = _interval / 2;
            const auto interval_delta = _interval.count();
            auto last_time = clock_type::now();

            while (!_stop) {
                auto now = clock_type::now();
                const auto& diff = (now - last_time).count();

                if (diff >= interval_delta) {
                    ++_ticks;
                    last_time += _interval;
                }

                std::this_thread::sleep_for(check_delay);
            }
        }

      private:
        /// The current ticks since the timer began.
        std::atomic<uint64_t> _ticks{ 0 };

        /// Flag to stop timer thread.
        std::atomic<bool> _stop{ false };

        /// The interval at which ticks should increase.
        duration_t _interval{ 500 };

        /// The thread to update ticks on.
        std::thread _tick_thread;
    };

    /**
     * @brief Time based queue that maintains the push/pop order, but expires older
     *        values given a specific ttl.
     *
     * @tparam T            The element type to be stored.
     * @tparam Duration_t   The duration type to check for.
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

        using tick_type = queue_timer_service::tick_type;
        using bucket_type = std::vector<std::vector<T>>;
        using index_type = std::uint32_t;

        struct queue_value_type
        {
            queue_value_type(index_type bucket_index, index_type value_index, uint64_t expiry_tick)
              : _bucket_index{ bucket_index }
              , _value_index{ value_index }
              , _expiry_tick(expiry_tick)
            {
            }

            index_type _bucket_index;
            index_type _value_index;
            tick_type _expiry_tick;
        };

        using queue_type = std::vector<queue_value_type>;

      public:
        /**
         * @brief Construct a time_queue with defaults or supplied parameters
         *
         * @param duration  Duration of the queue. Value must be > 0, != and a multiple of interval.
         * @param interval  Interval size of each bucket, > 0 and != duration
         * @param timer     Shared pointer to timer service
         *
         * @throws          std::invalid_argument If the duration or interval do not meet requirements.
         * @throws          std::runtime_error If the timer is null.
         */
        time_queue(size_t duration, size_t interval, std::shared_ptr<queue_timer_service> timer)
          : _duration{ duration }
          , _interval{ interval }
          , _total_buckets{ _duration / _interval }
          , _timer(timer)
        {

            if (duration == 0 || duration % interval != 0 || duration == interval) {
                throw std::invalid_argument("Invalid time_queue constructor args");
            }

            if (timer == nullptr) {
                throw std::runtime_error("Timer cannot be null");
            }

            _buckets.resize(_total_buckets);
            _queue.reserve(_total_buckets);
        }

        /**
         * @brief Construct a time_queue with defaults or supplied parameters
         *
         * @param duration                  Duration of the queue, > 0, != and a multiple of interval.
         * @param interval                  Interval size of each bucket. Value must be > 0 and != duration
         * @param timer                     Shared pointer to timer service
         * @param initial_queue_size        Initial size of the queue to reserve.
         *
         * @throws std::invalid_argument    If the duration or interval do not meet requirements.
         * @throws std::runtime_error       If the timer is null.
         */
        time_queue(size_t duration,
                   size_t interval,
                   std::shared_ptr<queue_timer_service> timer,
                   size_t initial_queue_size)
          : time_queue(duration, interval, timer)
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
         * @param value The value to push onto the queue.
         * @param ttl   The time to live of the value, according to Duration_t.
         */
        void push(const T& value, size_t ttl)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();

            // Insert object forward in time based on current bucket, which may wrap
            const index_type future_bucket_index =
              (_bucket_index + (std::min(ttl, _duration) / _interval) - 1) % _total_buckets;

            internal_push(value, future_bucket_index, ticks + ttl);
        }

        /**
         * @brief Pushes a new value onto the queue with a time to live of Interval.
         * @param value The value to push onto the queue.
         */
        void push(const T& value)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();

            const auto future_bucket_index = (_bucket_index + (_duration / _interval) - 1) % _total_buckets;

            internal_push(value, future_bucket_index, ticks + 1);
        }

        /**
         * @brief Pushes a new value onto the queue with a time to live.
         * @param value The value to push onto the queue.
         * @param ttl   The time to live of the value, according to Duration_t.
         */
        void push(T&& value, size_t ttl)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();
            const tick_type expiry_tick = ticks + ttl;

            // Insert object forward in time based on current bucket, which may wrap
            const auto future_bucket_index =
              (_bucket_index + (std::min(ttl, _duration) / _interval) - 1) % _total_buckets;

            internal_push(std::move(value), future_bucket_index, expiry_tick);
        }

        /**
         * @brief Pushes a new value onto the queue with a time to live of Interval.
         * @param value The value to push onto the queue.
         */
        void push(T&& value)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();

            const auto future_bucket_index = (_bucket_index + (_duration / _interval) - 1) % _total_buckets;

            internal_push(std::move(value), _bucket_index + 1, ticks + 1);
        }

        /**
         * @brief Pops off the most valid front of the queue.
         * @returns The popped value, else nullopt.
         */
        std::optional<T> pop()
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();

            while (_queue_index < _queue.size()) {
                auto& [bucket_index, value_index, expiry_tick] = _queue.at(_queue_index++);
                auto& bucket = _buckets.at(bucket_index);

                if (value_index >= bucket.size() || ticks > expiry_tick) {
                    continue;
                }

                return bucket.at(value_index);
            }

            _queue.clear();
            _queue_index = 0;

            return std::nullopt;
        }

        /**
         * @brief Returns the most valid front of the queue without popping.
         * @returns The front value of the queue, else nullopt
         */
        std::optional<T> front()
        {
            std::lock_guard<std::mutex> lock(_mutex);

            const tick_type ticks = advance();

            while (_queue_index < _queue.size()) {
                auto& [bucket_index, value_index, expiry_tick] = _queue.at(_queue_index);
                auto& bucket = _buckets.at(bucket_index);

                if (value_index >= bucket.size() || ticks > expiry_tick) {
                    _queue_index++;
                    continue;
                }

                return bucket.at(value_index);
            }

            _queue.clear();
            _queue_index = 0;

            return std::nullopt;
        }

        size_t size() const { return _queue.size() - _queue_index; }
        bool empty() const { return (_queue.empty() || _queue_index >= _queue.size()); }

      private:
        /**
         * @brief Based on current time, adjust and move the bucket index with time
         *        (sliding window)
         *
         * @returns Current tick value at time of advance
         */
        inline tick_type advance()
        {
            _timer->get_ticks(Duration_t(_interval), _timer_ctx);

            if (_timer_ctx.delta == 0)
                return _timer_ctx.ticks;

            if (_timer_ctx.delta >= static_cast<tick_type>(_total_buckets)) {
                _buckets.clear();
                _buckets.resize(_total_buckets);

                _queue.clear();
                _bucket_index = _queue_index = 0;

                return _timer_ctx.ticks;
            }

            if (_queue_index && _queue_index >= _queue.size()) {
                _queue.clear();
                _queue_index = 0;
            }

            for (int i = 0; i < _timer_ctx.delta; i++) {
                _buckets[(_bucket_index + i) % _total_buckets].clear();
            }

            _bucket_index = (_bucket_index + _timer_ctx.delta) % _total_buckets;

            return _timer_ctx.ticks;
        }

        /**
         * Internal definition of push. Pushes value into specified bucket, and
         * then emplaces the location info into the queue.
         */
        template<typename Value>
        inline void internal_push(Value value, index_type index, tick_type expiry_tick)
        {
            _buckets[index].push_back(value);
            _queue.emplace_back(index, _buckets[index].size() - 1, expiry_tick);
        }

      private:
        std::mutex _mutex;

        /// The duration of the entire queue.
        size_t _duration;

        /// The interval at which buckets are cleared.
        size_t _interval;

        /// The total amount of buckets. Value is calculated by duration / interval.
        size_t _total_buckets;

        /// The memory storage for all elements to be managed.
        bucket_type _buckets;

        /// The index in time of the current bucket.
        index_type _bucket_index{ 0 };

        /// The FIFO ordered queue of values as they were inserted.
        queue_type _queue;

        /// The index of the first valid item in the queue.
        index_type _queue_index{ 0 };

        /// Instance of timer to get time ticks
        queue_timer_service::timer_context _timer_ctx{ 0, 0, 0 };
        std::shared_ptr<queue_timer_service> _timer{ nullptr };
    };

}; // namespace qtransport
