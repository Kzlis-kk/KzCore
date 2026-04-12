#pragma once

#include <string_view>
#include <cstring>
#include <KzSTL/KzString.h>

namespace KzLog {

// 减去内部指针的开销，确保整个对象刚好 4KB
constexpr int kSmallBuffer = 4096 - sizeof(char*); 
// 确保整个对象刚好 4MB
constexpr int kLargeBuffer = 4 * 1024 * 1024 - sizeof(char*);

/**
 * @brief 高性能定长缓冲区
 * * 主要用于 LogStream，作为日志前端的组装容器。
 */
template <int SIZE>
class FixedBuffer {
public:
    // 没有防御空指针或者 len 为 0 的操作，由于只为 LogStream 使用，处理逻辑交给上层
    void append(const char* buf, size_t len) noexcept {
        // 只有空间足够才写入，溢出部分直接丢弃（日志系统的各种截断策略中，截断比Crash好）
        if (avail() < len) [[unlikely]] {
            if (avail() == 0) return;
            len = avail() - 1;
            ::memcpy(_cur, buf, len);
            _cur[len] = '\n';
            _cur += len + 1;
            return;
        }
        ::memcpy(_cur, buf, len);
        _cur += len;
    }
    void append(std::string_view sv) noexcept {
        append(sv.data(), sv.size());
    }

    const char* data() const noexcept { return _data; }
    size_t length() const noexcept { return  static_cast<size_t>(_cur - _data); }

    char* current() noexcept { return _cur; }
    size_t avail() const noexcept { return static_cast<size_t>(end() - _cur); }

    // 手动移动写指针 (通常配合直接写入 current() 后使用)
    // 这里默认上层安全，故只用assert防御
    void add(size_t len) noexcept {
        assert(avail() >= len);
        _cur += len;
    }

    void reset() noexcept { _cur = _data; }
    void bzero() noexcept { ::memset(_data, 0, sizeof(_data)); }

    std::string_view toStringView() const noexcept { return std::string_view(_data, length()); }
    KzSTL::KzString toKzString() const { return KzSTL::KzString(_data, length()); }

private:
    const char* end() const noexcept { return _data + sizeof(_data); }

    char _data[SIZE];  // 核心存储
    char* _cur = _data;  // 当前写指针
};

} // namespace KzLog