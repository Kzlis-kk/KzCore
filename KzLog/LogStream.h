#pragma once

#include <string_view>
#include <cstdio>
#include <charconv>

#include "KzAlgorithm/Jeaiii.h"
#include "KzAlgorithm/Hex.h"
#include "FixedBuffer.h"

namespace KzLog {

using namespace std::string_view_literals;
class LogStream {
public:
    using Buffer = FixedBuffer<kSmallBuffer>;

    LogStream& operator<<(short v) noexcept {
        formatInteger(static_cast<int64_t>(v));
        return *this;
    }
    LogStream& operator<<(unsigned short v) noexcept {
        formatInteger(static_cast<uint64_t>(v));
        return *this;
    }
    LogStream& operator<<(int v) noexcept {
        formatInteger(static_cast<int64_t>(v));
        return *this;
    }
    LogStream& operator<<(unsigned int v) noexcept {
        formatInteger(static_cast<uint64_t>(v));
        return *this;
    }
    LogStream& operator<<(long v) noexcept {
        formatInteger(static_cast<int64_t>(v));
        return *this;
    }
    LogStream& operator<<(unsigned long v) noexcept {
        formatInteger(static_cast<uint64_t>(v));
        return *this;
    }
    LogStream& operator<<(long long v) noexcept {
        formatInteger(static_cast<int64_t>(v));
        return *this;
    }
    LogStream& operator<<(unsigned long long v) noexcept {
        formatInteger(static_cast<uint64_t>(v));
        return *this;
    }

    LogStream& operator<<(const void* ptr) noexcept {
        if (_buffer.avail() >= kMaxNumericSize) [[likely]] {
            char* end = KzAlgorithm::Hex::to_hex(_buffer.current(), reinterpret_cast<uintptr_t>(ptr));
            _buffer.add(static_cast<size_t>(end - _buffer.current()));
        }
        return *this;
    }

    LogStream& operator<<(float v) noexcept {
        return *this << static_cast<double>(v);
    }
    LogStream& operator<<(double v) noexcept {
        if (_buffer.avail() >= kMaxNumericSize) [[likely]] {
            // chars_format::general 对应 printf 的 %g (自动选择定点或科学计数法)
            // 现代编译器底层使用 Ryu 或 Dragonbox 算法
            auto [ptr, ec] = std::to_chars(
                _buffer.current(), 
                _buffer.current() + _buffer.avail(), 
                v,
                std::chars_format::general
            );

            if (ec == std::errc()) [[likely]] {
                // 转换成功，ptr 指向结束位置
                _buffer.add(static_cast<size_t>(ptr - _buffer.current()));
            } else {
                // 极其罕见：空间不足（已被 avail() 防御）或转换失败
                // 这里直接丢弃
            }
        }
        return *this;
    }

    LogStream& operator<<(char v) noexcept {
        _buffer.append(std::string_view(&v, 1));
        return *this;
    }
    LogStream& operator<<(unsigned char v) noexcept {
        formatInteger(static_cast<uint64_t>(v));
        return *this;
    }

    LogStream& operator<<(bool v) noexcept {
        _buffer.append(v ? "1"sv : "0"sv);
        return *this;
    }

    LogStream& operator<<(const char* str) {
        if (str) [[likely]] {
            _buffer.append(std::string_view(str, std::strlen(str)));
        }
        else {
            _buffer.append("(null)"sv);
        }
        return *this;
    }
    LogStream& operator<<(const unsigned char* str) noexcept {
        return *this << reinterpret_cast<const char*>(str);
    }
    LogStream& operator<<(std::string_view str) noexcept {
        _buffer.append(str);
        return *this;
    }

    LogStream& operator<<(Buffer& buffer) noexcept {
        _buffer.append(buffer.toStringView());
        return *this;
    }

    void append(std::string_view logLine) noexcept { _buffer.append(logLine); }
    const Buffer& buffer() const noexcept { return _buffer; }
    void reset() noexcept { _buffer.reset(); }

private:
    static constexpr int kMaxNumericSize = 32;

    Buffer _buffer;

    void formatInteger(uint64_t v) noexcept {
        if (_buffer.avail() > kMaxNumericSize) [[likely]] {
            char* end = KzAlgorithm::Jeaiii::to_chars(_buffer.current(), v);
            _buffer.add(static_cast<size_t>(end - _buffer.current()));
        }
    }
    void formatInteger(int64_t v) noexcept {
        if (_buffer.avail() > kMaxNumericSize) [[likely]] {
            char* end = KzAlgorithm::Jeaiii::to_chars(_buffer.current(), v);
            _buffer.add(static_cast<size_t>(end - _buffer.current()));
        }
    }
};


} // namespace KzLog