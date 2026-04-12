#include "LogFile.h"
#include <cstdio>
#include <cassert>
#include <cstring>

#include "KzAlgorithm/Jeaiii.h"

namespace KzLog {

namespace detail {
// 辅助结构体：负责且仅负责初始化一次 Hostname 和 PID
struct ProcessInfo {
    int hostLen;
    int pidLen;
    char pid[32];
    char hostname[64];

    explicit ProcessInfo() {
        if (::gethostname(hostname, sizeof(hostname)) != 0) [[unlikely]] {
            ::strcpy(hostname, "unknown");
        }
        hostLen = ::strlen(hostname);

        pidLen = ::snprintf(pid, sizeof(pid), "%d", ::getpid());
    }

};

static ProcessInfo processInfo;
}; // namespace

void LogFile::append(std::string_view logLine) noexcept {
    FileUtil* ptr = std::launder(reinterpret_cast<FileUtil*>(&_file));
    ptr->append(logLine);

    // 写够了 rollSize 必须滚动
    if (ptr->writtenBytes() > _rollsize) [[unlikely]] {
        rollFile();
    }
    else {
        // 不是每次 append 都调用 time()
        // 而是每写入 checkEveryN 次检查一次
        ++_count;
        if (_count >= _checkEveryN) [[unlikely]] {
            time_t now = ::time(NULL);
            if (now / kRollPerSeconds * kRollPerSeconds > _startOfPeriod) {
                // 跨天了，强制滚动
                rollFile();
            }
            else if (now - _lastFlush > _flushInterval) {
                // 没跨天，但是超过 flush 间隔了，刷盘
                _lastFlush = now;
                ptr->flush();
            }
        }
    }
}

void LogFile::rollFile() noexcept {
    time_t now = ::time(NULL);
    if (now > _lastRoll) {
        // 新的一秒，重置计数器
        _rollIndex = 0;
        _lastRoll = now;
        _lastFlush = now;
        _startOfPeriod = now / kRollPerSeconds * kRollPerSeconds;
    }
    else {
        // 还是同一秒（甚至可能是时光倒流，也视为同一秒处理），计数器增加
        // 防止超高吞吐下的覆盖
        ++_rollIndex;
    }

    char filename[512];
    getLogFileName(now, filename, sizeof(filename));
    FileUtil* old_ptr = std::launder(reinterpret_cast<FileUtil*>(&_file));
    if (_lastRoll > 0) [[likely]] { // 确保不是第一次构造
        old_ptr->~FileUtil(); 
    }
    std::construct_at(old_ptr, filename);
}

// 冷路径，去除snprintf只是为了风格统一
void LogFile::getLogFileName(time_t& now, char* p, size_t len) noexcept {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);

    char* end = p + len;

    // 格式: basename.YYYYMMDD-HHMMSS.hostname.pid.log
    // 如果 rollIndex > 0，格式变为: basename.YYYYMMDD-HHMMSS.index.hostname.pid.log
    size_t baseLen = std::min(_basename.size(), len);
    std::memcpy(p, _basename.data(), baseLen);
    p += baseLen;

    if (p < end) [[likely]] *p++ = '.';
    if (p + 15 < end) [[unlikely]] p = end - 16;

    p += _timeFormatter.format(tv.tv_sec, p);

    if (_rollIndex > 0 && p < end) {
        *p++ = '.';

        char* start = p;
        int temp = _rollIndex;
        do {
            if (p < end) {
                *p++ = '0' + (temp % 10);
            }
            temp /= 10;
        } while (temp);

        std::reverse(start, p);
    }

    if (p < end) [[likely]] {
        size_t hostLen = std::min(static_cast<size_t>(end - p), static_cast<size_t>(detail::processInfo.hostLen));
        ::memcpy(p, detail::processInfo.hostname, hostLen);
        p += hostLen;
    }

    if (p < end) [[likely]] {
        size_t pidLen = std::min(static_cast<size_t>(end - p), static_cast<size_t>(detail::processInfo.pidLen));
        ::memcpy(p, detail::processInfo.pid, pidLen);
        p += pidLen;
    }

    if (p + 4 < end) [[likely]] {
        ::memcpy(p, ".log", 5);
    } 
    else if (len > 0) {
        p[len - 1] = '\0';
    }
}

} // namespace KzLog