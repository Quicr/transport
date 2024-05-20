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

        std::optional<T> front() noexcept
        {
            if (_buffer_cursor != _buffer.end()) {
                return *_buffer_cursor;
            }
            return std::nullopt;
        }

        size_t size() noexcept
        {
            return _size;
        }

        size_t actual_size() noexcept
        {
            return _buffer.size();
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

            purge();
            _buffer.push_back(value);
            _size++;


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

            purge();
            _buffer.push_back(std::move(value));
            _size++;

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

            purge();
            _buffer.insert(_buffer.end(), value.begin(), value.end());
            _size += value.size();

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

            purge();
            _buffer.insert(_buffer.end(), value.begin(), value.end());
            _size += value.size();

            if (update_cursor) {
                _buffer_cursor = _buffer.begin();
            }
        }

        void pop()
        {
            next();
        }

        void pop(std::uint16_t length)
        {
            next(length);
        }

    private:
        buffer_t _buffer;
        buffer_t::iterator _buffer_cursor { _buffer.begin() };
        int _remove_len {0};
        int _size {0};

        void next()
        {
            next(1);
        }

        void next(uint16_t len)
        {
            if (_buffer_cursor == _buffer.end()) {
                _remove_len = _size;
                return;
            }

            if (len > _size) len = _size;

            if (!len) return;

            _remove_len += len;

            _buffer_cursor = std::next(_buffer_cursor, len);
        }

        void purge()
        {
            //std::cout << "remove_len: " << _remove_len << std::endl;
            if (_remove_len > 0) {
                _size -= _remove_len;
                auto last = std::next(_buffer.begin(), _remove_len);
                _buffer.erase(_buffer.begin(), last);
                _remove_len = 0;
            }
        }
    };
}