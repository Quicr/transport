/*
 * Copyright (c) 2023 Cisco Systems, Inc. and others.  All rights reserved.
 */
#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <unistd.h>

namespace qtransport {

/**
 * @brief safeQueue is a thread safe basic queue
 *
 * @details This class is a thread safe wrapper for std::queue<T>.
 * 		Not all operators or methods are implemented.
 *
 * @todo Implement any operators or methods needed
 */
template <typename T> class safeQueue {
public:
  /**
   * @brief safeQueue constructor
   *
   * @param limit     Limit number of messages in queue before push blocks. Zero
   *                  is unlimited.
   */
  safeQueue(uint32_t limit = 1000) { this->limit = limit; }

  ~safeQueue() {}

  /**
   * @brief inserts element at the end of queue
   *
   * @param elem
   * @return True if successfully pushed, false if not.  The cause for false is
   * that the queue is full.
   */
  bool push(T const &elem) {
    if (limit && size() >= limit) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex);

    if (size() == 0)
      empty_block_mutex.unlock();

    queue.push(elem);

    return true;
  }

  /**
   * @brief Remove the first object from queue (oldest object)
   *
   * @return std::nullopt if queue is empty, otherwise reference to object
   */
  std::optional<T> pop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (queue.empty()) {
      return std::nullopt;
    }

    auto elem = queue.front();
    queue.pop();

    // Lock empty lock to trigger blocking of threads using block_pop()
    if (size() == 0) {
      empty_block_mutex.try_lock();
    }

    return elem;
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
  std::optional<T> block_pop() {

    empty_block_mutex.lock();
    empty_block_mutex.unlock();

    return pop();
  }

  /**
   * @brief Size of the queue
   *
   * @return size of the queue
   */
  size_t size() { return queue.size(); }

  void setLimit(uint32_t limit) { this->limit = limit; }

private:
  std::mutex empty_block_mutex; // Blocking mutex for when the queue is empty
  std::mutex mutex;             // read/write lock
  uint32_t limit;               // Limit of number of messages in queue
  std::queue<T> queue;          // Queue
};

} /* namespace qtransport */
