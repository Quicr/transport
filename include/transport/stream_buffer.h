#pragma once

#include <iostream>
#include <iterator>
#include <list>
#include <span>

namespace qtransport {
    template <typename T, class Allocator = std::allocator<T>>
    class stream_buffer
    {
        using buffer_t = std::list<T, Allocator>;

    public:
        stream_buffer() = default;
        bool empty() const noexcept
        {
            return _buffer.empty();
        }

        size_t size() noexcept
        {
            return _size;
        }

        size_t actual_size() noexcept
        {
            return _buffer.size();
        }

        std::optional<T> front() noexcept
        {
            if (_size && _remove_len == 0) {
                _buffer_cursor = _buffer.begin();
                return *_buffer_cursor;
            }

            if (_size && _size != _remove_len && _buffer_cursor != _buffer.end()) {
                return *_buffer_cursor;
            }

            return std::nullopt;
        }

        std::vector<T> front(std::uint16_t length) const noexcept
        {
            if (!_size || _buffer_cursor == _buffer.end()) {
                _buffer_cursor = _buffer.begin();
                return {};
            } else if (_size == 1) {    // If size equals one, reset the cursor to beginning
                _buffer_cursor = _buffer_cursor.begin();
                return *_buffer_cursor;
            }

            std::vector<T> result(length);
            std::copy_n(_buffer_cursor, length, result.begin());
            return result;
        }

        void pop()
        {
            next();
        }

        void pop(std::uint16_t length)
        {
            next(length);
        }

        bool available(std::uint16_t length) const noexcept
        {
            return _buffer.size() >= length;
        }

        void push(const T& value)
        {
            purge();
            _buffer.push_back(value);
            _size++;
        }

        void push(T&& value)
        {
            purge();
            _buffer.push_back(std::move(value));
            _size++;
        }

        void push(std::span<T> value)
        {
            purge();
            _buffer.insert(_buffer.end(), value.begin(), value.end());
            _size += value.size();
        }

        void push(std::initializer_list<T> value)
        {
            purge();
            _buffer.insert(_buffer.end(), value.begin(), value.end());
            _size += value.size();
        }

        void purge()
        {
            if (_remove_len > 0) {
                if (_remove_len >= _size) {
                    _buffer.clear();
                    _size = 0;

                } else {
                    auto last = std::next(_buffer.begin(), _remove_len);
                    _buffer.erase(_buffer.begin(), last);
                    _size -= _remove_len;
                }
                _remove_len = 0;
            }
        }

    private:
        buffer_t _buffer;
        buffer_t::iterator _buffer_cursor { _buffer.end() };
        size_t _remove_len {0};
        size_t _size {0};

        void next()
        {
            next(1);
        }

        void next(uint16_t len)
        {
            if (len == 0 || _buffer_cursor == _buffer.end()) return;

            if (len > _size) len = _size;

            _remove_len += len;
            _buffer_cursor = std::next(_buffer_cursor, len);
        }

    };
}