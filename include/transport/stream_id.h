#pragma once

#include <atomic>
#include <stdint.h>

namespace qtransport {
  class StreamId {
    using value_type = uint64_t;

  public:
    StreamId() = default;
    ~StreamId() = default;

    constexpr StreamId(value_type value, bool is_server, bool is_unidirectional)
        : _is_server{is_server},
          _is_unidirectional{is_unidirectional} {
      adjust(value);
    }

    constexpr StreamId(value_type value)
        : _value{value},
          _is_server{value & 0b01},
          _is_unidirectional{value & 0b10} {}

    constexpr StreamId(const StreamId &) = default;
    constexpr StreamId(StreamId &&) = default;

    constexpr StreamId &operator=(const StreamId &) = default;
    constexpr StreamId &operator=(StreamId &&) = default;
    constexpr StreamId &operator=(const value_type &value) {
      _is_server = value & 0b01;
      _is_unidirectional = value & 0b10;
      adjust(value);
      return *this;
    }
    constexpr StreamId &operator=(value_type &&value) {
      _is_server = value & 0b01;
      _is_unidirectional = value & 0b10;
      _value = std::move(value);
      adjust(_value);
      return *this;
    }

    constexpr operator value_type() const { return _value; }

    constexpr StreamId &operator++() noexcept {
      adjust(_value + 4);
      return *this;
    }

    constexpr StreamId operator++(int) noexcept {
      StreamId id(*this);
      ++*this;
      return id;
    }

  private:
    constexpr void adjust(value_type value) {
      _value = (value & (~0x0u << 2)) | _is_server | (_is_unidirectional * 2);
    }

  private:
    value_type _value : 62;
    value_type _is_server : 1;
    value_type _is_unidirectional : 1;
  };
}// namespace qtransport

template<>
class std::atomic<qtransport::StreamId> : std::__atomic_base<qtransport::StreamId> {
  using value_type = qtransport::StreamId;
  using base_type = std::__atomic_base<value_type>;

public:
  constexpr atomic(value_type value) noexcept : base_type(value) {}
  constexpr atomic(const atomic &) = delete;
  constexpr atomic(atomic &&) = delete;
  constexpr atomic &operator=(const atomic &) = delete;
  constexpr atomic &operator=(atomic &&) = delete;

  template<typename... Args>
  constexpr atomic(Args... args) noexcept : base_type(value_type(std::forward<Args>(args)...)) {}

  atomic &operator++() {
    auto v = load();
    store(++v);
    return *this;
  }

  atomic operator++(int) {
    auto v = load();
    auto v_out = v++;
    store(v);
    return v_out;
  }

  constexpr operator value_type() const { return load(); }
};
