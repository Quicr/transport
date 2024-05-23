#pragma once

#include <iostream>
#include <iterator>
#include <deque>
#include <mutex>
#include <span>
#include <optional>

#include <transport/uintvar.h>

namespace qtransport {
    template <typename T, class Allocator = std::allocator<T>>
    class StreamBuffer
    {
        using buffer_t = std::deque<T, Allocator>;

    public:
        StreamBuffer() = default;
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

        std::vector<T> front(std::uint32_t length) noexcept
        {

            if (!_buffer.empty()) {
                std::lock_guard<std::mutex> _(_rwLock);

                std::vector<T> result(length);
                std::copy_n(_buffer.begin(), length, result.begin());
                return result;
            }
            return std::vector<T>();
        }

        void pop()
        {
            if (_buffer.size()) {
                std::lock_guard<std::mutex> _(_rwLock);
                _buffer.pop_front();
            }
        }

        void pop(std::uint32_t length)
        {
            if (!length || _buffer.empty()) return;

            std::lock_guard<std::mutex> _(_rwLock);

            if (length >= _buffer.size()) {
                _buffer.clear();
            } else {
                _buffer.erase(_buffer.begin(), _buffer.begin() + length);
            }
        }

        bool available(std::uint32_t length) const noexcept
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

        /**
         * Decodes a variable length int (uintV) from start of stream buffer
         *
         * @details Reads uintV from stream buffer. If all bytes are available, the
         *      unsigned 64bit integer will be returned and the buffer
         *      will be moved past the uintV. Nullopt will be returned if not enough
         *      bytes are available.
         *
         * @return Returns uint64 decoded value or nullopt if not enough bytes are available
         */
        std::optional<uint64_t> decode_uintV()
        {
            if (const auto uv_msb = front()) {
                if (available(uintV_size(*uv_msb))) {
                    uint64_t uv_len = uintV_size(*uv_msb);
                    auto val = to_uint64(front(uv_len));

                    pop(uv_len);

                    return val;
                }
            }

            return std::nullopt;
        }

        /**
         * Decodes a variable length array of uint8_t bytes from start of stream buffer
         *
         * @details Reads uintV from stream buffer to get the length of the byte array. Then
         *      reads byte array from stream buffer after the uintV length.  Vector of bytes
         *      will be returned if all bytes are available. Otherwise nullopt will be returned
         *      to indicate not enough bytes available.

         * @return Returns vector<uint8_t> or nullopt if not enough bytes are available
         */
        std::optional<std::vector<uint8_t>> decode_bytes()
        {
            if (const auto uv_msb = front()) {
                if (available(uintV_size(*uv_msb))) {
                    uint64_t uv_len = uintV_size(*uv_msb);
                    auto len = to_uint64(front(uv_len));

                    if (_buffer.size() >= uv_len + len) {
                        pop(uv_len);
                        auto v = front(len);
                        pop(len);

                        return v;
                    }
                }
            }

            return std::nullopt;
        }


    private:
        buffer_t _buffer;
        std::mutex _rwLock;
    };
}