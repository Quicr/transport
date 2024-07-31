/*
 * Copyright (c) 2023 Cisco Systems, Inc. and others.  All rights reserved.
 */
#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <unistd.h>
#include <condition_variable>
#include <iostream>
#include <atomic>

namespace qtransport {

/**
 * @brief safe_queue is a thread safe basic queue
 *
 * @details This class is a thread safe wrapper for std::queue<T>.
 * 		Not all operators or methods are implemented.
 *
 * @todo Implement any operators or methods needed
 */
template<typename T>
class SafeQueue
{
public:
  /**
   * @brief safe_queue constructor
   *
   * @param limit     Limit number of messages in queue before push blocks. Zero
   *                  is unlimited.
   */
  SafeQueue(uint32_t limit = 1000)
    : _stop_waiting{ false }
    , _limit{ limit }
  {
  }

  ~SafeQueue() { stop_waiting(); }

  /**
   * @brief inserts element at the end of queue
   *
   * @details Inserts element at the end of queue. If queue is at max size,
   *    the front element will be popped/removed to make room.
   *    In this sense, the queue is sliding forward with every new message
   *    added to queue.
   *
   * @param elem
   * @return True if successfully pushed, false if not.  The cause for false is
   * that the queue is full.
   */
  bool push(T const& elem)
  {
    bool rval = true;

    std::lock_guard<std::mutex> _(_mutex);

    if (_queue.empty()) {
        _cv.notify_one();
        _empty = false;
    }

    else if (_queue.size() >= _limit) { // Make room by removing first element
     _queue.pop();
     rval = false;
    }

    _queue.push(elem);

    return rval;
  }

  /**
   * @brief Remove the first object from queue (oldest object)
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   */
  std::optional<T> pop()
  {
    std::lock_guard<std::mutex> _(_mutex);
    return pop_internal();
  }

  /**
    * @brief Get first object without removing from queue
    *
    * @return std::nullopt if queue is empty, otherwise reference to object
    */
  std::optional<T> front()
  {
    std::lock_guard<std::mutex> _(_mutex);

    if (_queue.empty()) {
      return std::nullopt;
    }

    return _queue.front();
  }

  /**
  * @brief Remove (aka pop) the first object from queue
  *
  */
  void pop_front()
  {
    std::lock_guard<std::mutex> _(_mutex);

    pop_front_internal();
  }

  /**
   * @brief Block waiting for data in queue, then remove the first object from
   * queue (oldest object)
   *
   * @details This will block if the queue is empty. Due to concurrency, it's
   * possible that when unblocked the queue might still be empty. In this case,
   * try again.
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   */
  std::optional<T> block_pop()
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [&]() { return (_stop_waiting || (_queue.size() > 0)); });

    if (_stop_waiting) {
      return std::nullopt;
    }

    return pop_internal();
  }

  /**
   * @brief Size of the queue
   *
   * @return size of the queue
   */
  size_t size()
  {
    std::lock_guard<std::mutex> _(_mutex);
    return _queue.size();
  }

  /**
   * @brief Clear the queue
   */
  void clear() {
    std::lock_guard<std::mutex> _(_mutex);
    std::queue<T> empty;
    std::swap(_queue, empty);
  }

  /**
   * @brief Check if queue is empty
   *
   * @returns True if empty, false if not
   */
  bool empty() const { return _empty; }

    /**
   * @brief Put the queue in a state such that threads will not wait
   *
   * @return Nothing
   */
  void stop_waiting()
  {
    std::lock_guard<std::mutex> _(_mutex);
    _stop_waiting = true;
    _cv.notify_all();
  }

  void set_limit(uint32_t limit)
  {
    std::lock_guard<std::mutex> _(_mutex);
    _limit = limit;
  }

private:

  /**
   * @brief Remove the first object from queue (oldest object)
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   *
   * @details The mutex must be locked by the caller
   */
  std::optional<T> pop_internal()
  {
    if (_queue.empty()) {
      _empty = true;
      return std::nullopt;
    }

    auto elem = _queue.front();
    _queue.pop();

    if (_queue.empty()) {
      _empty = true;
    }

    return elem;
  }

  /**
 * @brief Remove the first object from queue (oldest object)
 *
 * @details The mutex must be locked by the caller
 */
  void pop_front_internal()
  {
    if (_queue.empty()) {
      _empty = true;
      return;
    }

    _queue.pop();

    if (_queue.empty()) {
      _empty = true;
    }
  }

  std::atomic<bool> _empty { true };
  bool _stop_waiting;                // Instruct threads to stop waiting
  uint32_t _limit;                   // Limit of number of messages in queue
  std::condition_variable _cv;       // Signaling for thread syncronization
  std::mutex _mutex;                 // read/write lock
  std::queue<T> _queue;              // Queue
};

} /* namespace qtransport */
