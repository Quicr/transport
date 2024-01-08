#pragma once

#include <algorithm>
#include <chrono>
#include <forward_list>
#include <iostream>
#include <numeric>
#include <optional>
#include <thread>
#include <type_traits>

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
    class priority_queue
    {

        using timeType = std::chrono::milliseconds;
        using timeQueue = time_queue<DataType, timeType>;

        struct Exception : public std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        struct InvalidPriorityException : public Exception
        {
            using Exception::Exception;
        };

      public:
        ~priority_queue() {
            std::lock_guard<std::mutex> _(_mutex);
        }

        /**
         * Construct a priority queue
         * @param tick_service Shared pointer to tick_service service
         */
        priority_queue(const std::shared_ptr<tick_service>& tick_service)
          : priority_queue(1000, 1, _tick_service, 1000)
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
        priority_queue(size_t duration,
                       size_t interval,
                       const std::shared_ptr<tick_service>& tick_service,
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
         */
        void push(DataType& value, uint32_t ttl, uint8_t priority = 0)
        {
            std::lock_guard<std::mutex> _(_mutex);

            if (priority >= PMAX) {
                throw InvalidPriorityException("Priority not within range");
            }

            if (!_queue[priority]) {
                _queue[priority] = std::make_unique<timeQueue>(_duration_ms, _interval_ms, _tick_service, _initial_queue_size);
            }

            auto& queue = _queue[priority];
            queue->push(value, ttl);
        }

        /**
         * @brief Get the first object from queue
         *
         * @return std::nullopt if queue is empty, otherwise reference to object
         */
        std::optional<DataType> front()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (!tqueue)
                    continue;

                if (auto obj = tqueue->front())
                    return obj;
            }

            return std::nullopt;
        }

        /**
         * @brief Get and remove the first object from queue
         *
         * @return std::nullopt if queue is empty, otherwise reference to object
         */
        std::optional<DataType> pop_front()
        {
            std::lock_guard<std::mutex> _(_mutex);

            for (auto& tqueue : _queue) {
                if (!tqueue)
                    continue;

                if (auto obj = tqueue->pop_front())
                    return obj;
            }

            return std::nullopt;
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
        std::mutex _mutex;
        size_t _initial_queue_size;
        size_t _duration_ms;
        size_t _interval_ms;

        std::array<std::unique_ptr<timeQueue>, PMAX> _queue;

        std::shared_ptr<tick_service> _tick_service;
    };
}; // end of namespace qtransport