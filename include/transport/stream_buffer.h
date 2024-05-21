#pragma once

#include <iostream>
#include <iterator>
#include <deque>
#include <mutex>
#include <span>

namespace qtransport {
    template <typename T, class Allocator = std::allocator<T>>
    class stream_buffer
    {
        using buffer_t = std::deque<T, Allocator>;

    public:
        stream_buffer() = default;
        bool empty() const noexcept
        {
            return _buffer.empty();
        }

        size_t size() noexcept
        {
            return _buffer.size();
        }

        std::optional<T> front() noexcept
        {
            if (_buffer.size()) {
                std::lock_guard<std::mutex> _(_rwLock);
                return _buffer.front();
            }

            return std::nullopt;
        }

        std::vector<T> front(std::uint16_t length) noexcept
        {

            if (_buffer.size()) {
                std::lock_guard<std::mutex> _(_rwLock);

                std::vector<T> result(length);
                std::copy_n(_buffer.begin(), length, result.begin());
                return result;
            }
        }

        void pop()
        {
            if (_buffer.size()) {
                std::lock_guard<std::mutex> _(_rwLock);
                _buffer.pop_front();
            }
        }

        void pop(std::uint16_t length)
        {
            if (!length || !_buffer.size()) return;

            std::lock_guard<std::mutex> _(_rwLock);

            if (length >= _buffer.size()) {
                _buffer.clear();
            } else {
                _buffer.erase(_buffer.begin(), _buffer.begin() + length);
            }
        }

        bool available(std::uint16_t length) const noexcept
        {
            return _buffer.size() >= length;
        }

        void push(const T& value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            _buffer.push_back(value);
        }

        void push(T&& value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            _buffer.push_back(std::move(value));
        }

        void push(std::span<T> value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            _buffer.insert(_buffer.end(), value.begin(), value.end());
        }

        void push(std::initializer_list<T> value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            _buffer.insert(_buffer.end(), value.begin(), value.end());
        }

    private:
        buffer_t _buffer;
        std::mutex _rwLock;
    };
}