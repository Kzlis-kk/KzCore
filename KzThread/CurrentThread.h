#pragma once 

#include <unistd.h>
#include <sys/syscall.h>
#include <string_view>
#include "KzAlgorithm/Jeaiii.h"

namespace KzThread {
namespace CurrentThread {

inline thread_local pid_t _cachedTid = 0;
inline thread_local char _tidString[32];
inline thread_local int _tidStringLength;
inline thread_local char _threadName[32] = "unknown";
inline thread_local int _threadNameLength = 7;
inline pid_t g_mainThreadTid = ::getpid();

namespace detail {
    inline pid_t gettid() noexcept { return static_cast<pid_t>(::syscall(SYS_gettid)); } 

    inline void cacheTid() noexcept {
        if (_cachedTid == 0) {
            _cachedTid = gettid();

            char* end = KzAlgorithm::Jeaiii::to_chars(_tidString, static_cast<uint64_t>(_cachedTid));

            _tidStringLength = static_cast<int>(end - _tidString);
            while (_tidStringLength < 5) {
                *end++ = ' ';
                _tidStringLength++;
            }
            
            // 保证一个空格做分隔符
            *end++ = ' ';
            _tidStringLength++;

            // 二进制安全
            *end = '\0'; 
        } 
    }

} // namespace detail

inline pid_t tid() noexcept {
    if (!_cachedTid) [[unlikely]] {
        detail::cacheTid();
    }
    return _cachedTid;
}

inline std::string_view tidString() noexcept {
    return std::string_view(_tidString, _tidStringLength);
}

inline std::string_view name() noexcept {
    return std::string_view(_threadName, _threadNameLength);
} 

inline bool inMainThread() noexcept {
    return tid() == g_mainThreadTid;;
}


} // namespace CurrentThread
} // namespace KzThread