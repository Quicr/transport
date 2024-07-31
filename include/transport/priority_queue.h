#pragma once

#include <algorithm>
#include <chrono>
#include <forward_list>
#include <numeric>
#include <optional>
#include <array>

#include "time_queue.h"

namespace qtransport {
    /**
     * @brief Priority queue that uses time_queue for each priority
     *
     * @details Order is maintained for objects pushed by priority.
     *          During each `front()`/`pop()` the queue will always
     *          pop the lower priority objects first. Lower priority
     *          objects will be serviced first in the order they were
     *          added to the queue.
     *
     * @tparam DataType   The element type to be stored.
     * @tparam PMAX       Max priorities to allow - Range becomes 0 - PMAX
     */
    template<typename DataType, uint8_t PMAX = 32>
    class PriorityQueue
    {
        using timeType = std::chrono::milliseconds;
        using timeQueue = TimeQueue<DataType, timeType>;

        struct Exception : public std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        struct InvalidPriorityException : public Exception
        {
            using Exception::Exception;
        };

      public:
        ~PriorityQueue() {
        }

        /**
         * Construct a priority queue
         * @param tick_service Shared pointer to tick_service service
         */
        PriorityQueue(const std::shared_ptr<TickService>& tick_service)
          : PriorityQueue(1000, 1, _tick_service, 1000)
        {
        }

        /**
         * Construct a priority queue
         *
         * @param duration              Max duration of time for the queue
         * @param interval              Interval per bucket, Default is 1
         * @param tick_service          Shared pointer to tick_service service
         * @param initial_queue_size    Number of default fifo queue size (reserve)
         */
        PriorityQueue(size_t duration,
                       size_t interval,
                       const std::shared_ptr<TickService>& tick_service,
                       size_t initial_queue_size)
          : _tick_service(tick_service)
        {

            if (tick_service == nullptr) {
                throw std::invalid_argument("Tick service cannot be null");
            }

            _initial_queue_size = initial_queue_size;
            _duration_ms = duration;
            _interval_ms = interval;
        }

        /**
         * @brief Pushes a new value onto the queue with a time to live and priority
         *
         * @param value     The value to push onto the queue.
         * @param ttl       The time to live of the value in milliseconds.
         * @param priority  The priority of the value (range is 0 - PMAX)
         * @param delay_ttl Delay POP by this ttl value in milliseconds
         */
        void push(DataType& value, uint32_t ttl, uint8_t priority = 0, uint32_t delay_ttl=0)
        {
            std::lock_guard<std::mutex> _(_mutex);

            auto& queue = get_queue_by_priority(priority);
            queue->push(value, ttl, delay_ttl);
        }

        /**
         * @brief Pushes a new value onto the queue with a time to live and priority
         *
         * @param value     The value to push onto the queue.
         * @param ttl       The time to live of the value in milliseconds.
         * @param priority  The priority of the value (range is 0 - PMAX)
         * @param delay_ttl Delay POP by this ttl value in milliseconds
         */
        void push(DataType&& value, uint32_t ttl, uint8_t priority = 0, uint32_t delay_ttl=0) {
            std::lock_guard<std::mutex> _(_mutex);

            auto& queue = get_queue_by_priority(priority);
            queue->push(std::move(value), ttl, delay_ttl);
        }

        /**
         * @brief Get the first object from queue
         *
         * @return TimeQueueElement<DataType> value from time queue
         */
        TimeQueueElement<DataType> front()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (!tqueue || tqueue->empty())
                    continue;

                return std::move(tqueue->front());
            }

            return {};
        }

        /**
         * @brief Get and remove the first object from queue
         *
         * @return TimeQueueElement<DataType> from time queue
         */
        TimeQueueElement<DataType> pop_front()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (!tqueue || tqueue->empty())
                    continue;

                return std::move(tqueue->pop_front());
            }

            return {};
        }

        /**
         * @brief Pop/remove the first object from queue
         */
        void pop()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (tqueue && !tqueue->empty())
                    return tqueue->pop();
            }
        }

        /**
         * @brief Clear queue
         */
         void clear()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (tqueue && !tqueue->empty())
                    tqueue->clear();
            }
        }

        // TODO: Consider changing empty/size to look at timeQueue sizes - maybe support blocking pops
        size_t size() const
        {
            return std::accumulate(_queue.begin(), _queue.end(), 0, [](auto sum, auto& tqueue) {
                return tqueue ? sum + tqueue->size() : sum;
            });
        }

        bool empty() const
        {
            for (auto& tqueue : _queue) {
                if (tqueue && !tqueue->empty()) {
                    return false;
                }
            }

            return true;
        }

      private:
        /**
         * @brief Get queue by priority
         *
         * @param priority  The priority queue value (range is 0 - PMAX)
         *
         * @return Unique pointer to queue for the given priority
         */
        std::unique_ptr<timeQueue>& get_queue_by_priority(const uint8_t priority)
        {
            if (priority >= PMAX) {
                throw InvalidPriorityException("Priority not within range");
            }

            if (!_queue[priority]) {
                _queue[priority] = std::make_unique<timeQueue>(_duration_ms, _interval_ms, _tick_service, _initial_queue_size);
            }

            return _queue[priority];
        }

        std::mutex _mutex;
        size_t _initial_queue_size;
        size_t _duration_ms;
        size_t _interval_ms;

        std::array<std::unique_ptr<timeQueue>, PMAX> _queue;
        std::shared_ptr<TickService> _tick_service;
    };
}; // end of namespace qtransport