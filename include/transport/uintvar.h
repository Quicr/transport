#pragma once

#include <vector>

namespace qtransport {

    using uintV_t = std::vector<uint8_t>;

    /**
     * @brief Get the byte size from variable length-integer
     *
     * @param uintV_msbbyte     MSB byte of the variable length integer
     *
     * @returns the size in bytes of the variable length integer
     */
    inline uint8_t uintV_size(const uint8_t uintV_msbbyte)
    {
        if ((uintV_msbbyte & 0xC0) == 0xC0) {
            return 8;
        } else if ((uintV_msbbyte & 0x80) == 0x80) {
            return 4;
        } else if ((uintV_msbbyte & 0x40) == 0x40) {
            return 2;
        } else {
            return 1;
        }
    }

    /**
     * @brief Convert uint64_t to Variable-Length Integer
     *
     * @details Encode unsigned 64bit value to shorten wrire format per RFC9000 Section 16 (Variable-Length Integer
     * Encoding)
     *
     * @param value         64bit value to convert
     *
     * @returns vector of encoded bytes or empty vector if value is invalid
     */
    inline uintV_t to_uintV(uint64_t value)
    {
        static constexpr uint64_t len_1 = (static_cast<uint64_t>(-1) << (64 - 6) >> (64 - 6));
        static constexpr uint64_t len_2 = (static_cast<uint64_t>(-1) << (64 - 14) >> (64 - 14));
        static constexpr uint64_t len_4 = (static_cast<uint64_t>(-1) << (64 - 30) >> (64 - 30));

        uint8_t net_bytes[8]{ 0 }; // Network order bytes
        uint8_t len{ 0 };          // Length of bytes encoded

        uint8_t* byte_value = reinterpret_cast<uint8_t*>(&value);

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        constexpr std::array<uint8_t, sizeof(uint64_t)> host_order{ 0, 1, 2, 3, 4, 5, 6, 7 };
#else
        constexpr std::array<uint8_t, sizeof(uint64_t)> host_order{ 7, 6, 5, 4, 3, 2, 1, 0 };
#endif

        if (byte_value[host_order[0]] & 0xC0) { // Check if invalid
            return {};
        }

        if (value > len_4) { // 62 bit encoding (8 bytes)
            for (int i = 0; i < 8; i++) {
                net_bytes[i] = byte_value[host_order[i]];
            }
            net_bytes[0] |= 0xC0;
            len = 8;
        } else if (value > len_2) { // 30 bit encoding (4 bytes)
            for (int i = 0; i < 4; i++) {
                net_bytes[i] = byte_value[host_order[i + 4]];
            }
            net_bytes[0] |= 0x80;
            len = 4;
        } else if (value > len_1) { // 14 bit encoding (2 bytes)
            net_bytes[0] = byte_value[host_order[6]] | 0x40;
            net_bytes[1] = byte_value[host_order[7]];
            len = 2;
        } else {
            net_bytes[0] = byte_value[host_order[7]];
            len = 1;
        }

        std::vector<uint8_t> encoded_bytes(net_bytes, net_bytes + len);
        return encoded_bytes;
    }

    /**
     * @brief Convert Variable-Length Integer to uint64_t
     *
     * @param uintV             Encoded variable-Length integer
     *
     * @returns uint64_t value of the variable length integer
     */
    inline uint64_t to_uint64(const uintV_t& uintV)
    {
        if (uintV.empty()) {
            return 0;
        }

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        constexpr std::array<uint8_t, sizeof(uint64_t)> host_order{ 0, 1, 2, 3, 4, 5, 6, 7 };
#else
        constexpr std::array<uint8_t, sizeof(uint64_t)> host_order{ 7, 6, 5, 4, 3, 2, 1, 0 };
#endif
        uint64_t value{ 0 };
        uint8_t* byte_value = reinterpret_cast<uint8_t*>(&value);

        const auto offset = 8 - uintV.size();

        for (size_t i = 0; i < uintV.size(); i++) {
            byte_value[host_order[i + offset]] = uintV[i];
        }

        byte_value[host_order[offset]] = uintV[0] & 0x3f; // Zero MSB length bits

        return value;
    }

} // namespace qtransport
