#pragma once

#include <algorithm>
#include <chrono>
#include <forward_list>
#include <iostream>
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

        struct NullTimerException : public Exception
        {
            using Exception::Exception;
        };

      public:
        /**
         * Construct a priority queue
         */
        priority_queue(std::shared_ptr<queue_timer_service> timer) { priority_queue(1000, 1, _timer, 1000); }

        /**
         * Construct a priority queue
         *
         * @param duration              Max duration of time for the queue
         * @param interval              Interval per bucket, Default is 1
         * @param timer                 Shared pointer to timer service
         * @param initial_queue_size     Number of default fifo queue size (reserve)
         */
        priority_queue(size_t duration,
                       size_t interval,
                       std::shared_ptr<queue_timer_service> timer,
                       size_t initial_queue_size)
          : _timer(timer)
        {

            if (timer == nullptr) {
                throw NullTimerException("Timer cannot be null");
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
            std::lock_guard<std::mutex> lock(_mutex);

            if (priority >= PMAX) {
                throw InvalidPriorityException("Priority not within range");
            }

            if (!_queue[priority]) {
                _queue[priority] = std::make_unique<timeQueue>(_duration_ms, _interval_ms, _timer,
                                                               _initial_queue_size, 0, 0);
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
            std::lock_guard<std::mutex> lock(_mutex);

            for (size_t i = 0; i < _queue.size(); i++) {
                if (_queue[i]) {
                    const auto& obj = _queue[i]->front();
                    if (obj.has_value()) {
                        return obj;
                    }
                }
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
            std::lock_guard<std::mutex> lock(_mutex);

            for (size_t i = 0; i < _queue.size(); i++) {
                if (_queue[i]) {
                    const auto& obj = _queue[i]->pop_front();
                    if (obj.has_value()) {
                        return obj;
                    }
                }
            }

            return std::nullopt;
        }

        /**
         * @brief Pop/remove the first object from queue
         */
        void pop()
        {
            std::lock_guard<std::mutex> lock(_mutex);

            for (size_t i = 0; i < _queue.size(); i++) {
                if (_queue[i] && !_queue[i]->empty()) {
                    _queue[i]->pop();
                    return;
                }
            }
        }

        // TODO: Consider changing empty/size to look at timeQueue sizes - maybe support blocking pops
        size_t size() const
        {
            size_t sz = 0;
            for (size_t i = 0; i < _queue.size(); i++) {
                if (_queue[i]) {
                    sz += _queue[i]->size();
                }
            }

            return sz;
        }

        bool empty() const
        {
            for (size_t i = 0; i < _queue.size(); i++) {
                if (_queue[i] && !_queue[i]->empty()) {
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

        std::shared_ptr<queue_timer_service> _timer;
    };

}; // end of namespace qtransport