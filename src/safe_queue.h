/*
 * Copyright (c) 2023 Cisco Systems, Inc. and others.  All rights reserved.
 */
#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <unistd.h>
#include <condition_variable>

namespace qtransport {

/**
 * @brief safeQueue is a thread safe basic queue
 *
 * @details This class is a thread safe wrapper for std::queue<T>.
 * 		Not all operators or methods are implemented.
 *
 * @todo Implement any operators or methods needed
 */
template<typename T>
class safeQueue
{
public:
  /**
   * @brief safeQueue constructor
   *
   * @param limit     Limit number of messages in queue before push blocks. Zero
   *                  is unlimited.
   */
  safeQueue(uint32_t limit = 1000)
    : stop_waiting{ false }
    , limit{ limit }
  {
  }

  ~safeQueue() { stopWaiting(); }

  /**
   * @brief inserts element at the end of queue
   *
   * @param elem
   * @return True if successfully pushed, false if not.  The cause for false is
   * that the queue is full.
   */
  bool push(T const& elem)
  {
    std::lock_guard<std::mutex> lock(mutex);

    if (limit && queue.size() >= limit) {
      return false;
    }

    queue.push(elem);

    cv.notify_one();

    return true;
  }

  /**
   * @brief Remove the first object from queue (oldest object)
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   */
  std::optional<T> pop()
  {
    std::lock_guard<std::mutex> lock(mutex);

    return popInternal();
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
    std::unique_lock<std::mutex> lock(mutex);

    cv.wait(lock, [&]() { return (stop_waiting || (queue.size() > 0)); });

    return popInternal();
  }

  /**
   * @brief Size of the queue
   *
   * @return size of the queue
   */
  size_t size()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
  }

  /**
   * @brief Put the queue in a state such that threads will not wait
   *
   * @return Nothing
   */
  void stopWaiting()
  {
    std::lock_guard<std::mutex> lock(mutex);
    cv.notify_all();
    stop_waiting = true;
  }

  void setLimit(uint32_t limit)
  {
    std::lock_guard<std::mutex> lock(mutex);
    this->limit = limit;
  }

private:

  /**
   * @brief Remove the first object from queue (oldest object)
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   *
   * @details The mutex must be locked by the caller
   */
  std::optional<T> popInternal()
  {
    if (queue.empty()) {
      return std::nullopt;
    }

    auto elem = queue.front();
    queue.pop();

    if (queue.size() > 0) {
      cv.notify_one();
    }

    return elem;
  }

  bool stop_waiting;            // Instruct threads to stop waiting
  uint32_t limit;               // Limit of number of messages in queue
  std::condition_variable cv;   // Signaling for thread syncronization
  std::mutex mutex;             // read/write lock
  std::queue<T> queue;          // Queue
};

} /* namespace qtransport */
