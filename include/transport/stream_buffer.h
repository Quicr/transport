#pragma once

#include <iostream>
#include <deque>
#include <iterator>
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

        const T& front() const noexcept
        {
            return _buffer.front();
        }

        size_t size() noexcept
        {
            return std::distance(_buffer_cursor, _buffer.end());
        }

        std::vector<T> front(std::uint16_t length) const noexcept
        {
            std::vector<T> result(length);
            std::copy_n(_buffer_cursor, length, result.begin());
            return result;
        }

        bool available(std::uint16_t length) const noexcept
        {
            return _buffer.size() >= length;
        }

        void push(const T& value)
        {
            bool update_cursor = false;
            if (_buffer_cursor == _buffer.end()) {
                update_cursor = true;
            }

            _buffer.push_back(value);

            if (update_cursor) {
                _buffer_cursor = _buffer.begin();
            }
        }

        void push(T&& value)
        {
            bool update_cursor = false;
            if (_buffer_cursor == _buffer.end()) {
                update_cursor = true;
            }

            _buffer.push_back(std::move(value));

            if (update_cursor) {
                _buffer_cursor = _buffer.begin();
            }
        }

        void push(std::span<T> value)
        {
            bool update_cursor = false;
            if (_buffer_cursor == _buffer.end()) {
                update_cursor = true;
            }

            _buffer.insert(_buffer.end(), value.begin(), value.end());

            if (update_cursor) {
                _buffer_cursor = _buffer.begin();
            }
        }

        void push(std::initializer_list<T> value)
        {
            bool update_cursor = false;
            if (_buffer_cursor == _buffer.end()) {
                update_cursor = true;
            }

            _buffer.insert(_buffer.end(), value.begin(), value.end());

            if (update_cursor) {
                _buffer_cursor = _buffer.begin();
            }
        }

        void pop()
        {
            _buffer_cursor = std::next(_buffer_cursor);
        }

        void pop(std::uint16_t length)
        {
            if (size() < length) length = size();
            _buffer_cursor = std::next(_buffer_cursor, length);
        }

    private:
        buffer_t _buffer;
        buffer_t::iterator _buffer_cursor { _buffer.begin() };
    };
}