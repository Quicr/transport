#pragma once

#include <any>
#include <deque>
#include <mutex>
#include <optional>
#include <transport/span.h>

#include <transport/uintvar.h>

namespace qtransport {
    template<typename T, class Allocator = std::allocator<T>>
    class StreamBuffer
    {
        using buffer_t = std::deque<T, Allocator>;

      public:
        StreamBuffer() = default;

        /**
         * @brief Initialize the parsed data
         * @details Parsed data allows the caller to work on reading data from the
         *        stream buffer. The datatype is any to support the caller data types.
         *        This method will initialize the parsed data using the type specified
         * @tparam D              Data type for value
         */
        template<typename D>
        void initAny()
        {
            _parsed_data.emplace<D>();
        }

        template<typename D>
        void initAnyB()
        {
            _parsed_dataB.emplace<D>();
        }

        /**
         * @brief Initialize the parsed data and type
         * @tparam D              Data type for value
         * @param type            user defined type value for the parsed data any object
         */
        template<typename D>
        void initAny(uint64_t type)
        {
            _parsed_data.emplace<D>();
            _parsed_data_type = type;
        }

        /**
         * @brief Get the parsed data
         * @details Parsed data allows the caller to work on reading data from the
         *        stream buffer. The datatype is any to support the caller data types.
         *        This returns a reference to the any variable cast to the data type
         *
         * @tparam D              Data type of value
         */
        template<typename D>
        D& getAny()
        {
            return std::any_cast<D&>(_parsed_data);
        }

        template<typename D>
        D& getAnyB()
        {
            return std::any_cast<D&>(_parsed_dataB);
        }

        /**
         * @brief Get the user defined parsed type value
         * @return Parsed data type value that was set via initAny(). nullopt if not set
         */
        std::optional<uint64_t> getAnyType() { return _parsed_data_type; }

        /**
         * @brief Set the user-defined parsed data type value
         * @param type          User defined value for the data type
         */
        void setAnyType(uint64_t type) { _parsed_data_type = type; }

        void resetAny()
        {
            _parsed_data.reset();
            _parsed_dataB.reset();
            _parsed_data_type = std::nullopt;
        }

        void resetAnyB()
        {
            _parsed_dataB.reset();
        }

        bool anyHasValue()
        {
            return _parsed_data.has_value();
        }

        bool anyHasValueB()
        {
            return _parsed_dataB.has_value();
        }

        bool empty() const noexcept { return _buffer.empty(); }

        size_t size() noexcept { return _buffer.size(); }

        /**
         * @brief Get the first data byte in stream buffer
         * @returns data byt or nullopt if no data
         */
        std::optional<T> front() noexcept
        {
            if (_buffer.size()) {
                std::lock_guard<std::mutex> _(_rwLock);
                return _buffer.front();
            }

            return std::nullopt;
        }

        /**
         * @brief Front length number of data bytes
         *
         * @param length            Get the first up to length number of data bytes
         *
         * @returns data vector of bytes or nullopt if no data
         */
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
            if (!length || _buffer.empty())
                return;

            std::lock_guard<std::mutex> _(_rwLock);

            if (length >= _buffer.size()) {
                _buffer.clear();
            } else {
                _buffer.erase(_buffer.begin(), _buffer.begin() + length);
            }
        }

        /**
         * @brief Checks if lenght bytes are avaialble for front
         *
         * @param length        length of bytes needed
         *
         * @return True if data length is available, false if not.
         */
        bool available(std::uint32_t length) const noexcept { return _buffer.size() >= length; }

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

        void push(const Span<const T>& value)
        {
            const std::lock_guard<std::mutex> _(_rwLock);
            _buffer.insert(_buffer.end(), value.begin(), value.end());
        }

        void push(std::initializer_list<T> value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            _buffer.insert(_buffer.end(), value.begin(), value.end());
        }

        void push_lv(const Span<const T>& value)
        {
            std::lock_guard<std::mutex> _(_rwLock);
            const auto len = to_uintV(static_cast<uint64_t>(value.size()));
            _buffer.insert(_buffer.end(), len.begin(), len.end());
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
        std::any _parsed_data; /// Working buffer for parsed data
        std::any _parsed_dataB; /// Second Working buffer for parsed data
        std::optional<uint64_t> _parsed_data_type; /// working buffer type value
    };
}