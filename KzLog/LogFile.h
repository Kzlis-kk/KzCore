#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <ctime>
#include <string_view>
#include <cstring>
#include "KzSTL/KzString.h"
#include "FixedBuffer.h"
#include "TimeFormatter.h"

namespace KzLog {

// === 内部辅助类：FileUtil ===
// RAII 封装 FILE*，并使用 setbuffer 优化
class FileUtil {
public:
    static constexpr int kBufferSize = 64 * 1024;
    static constexpr int kBufferThreshold = kBufferSize / 2;
    

    explicit FileUtil(const char* filename) noexcept
    // 直接使用 open 系统调用，O_APPEND 保证多进程追加安全，O_CLOEXEC 防止子进程继承
    : _fd(::open(filename, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644))
    {
        if (_fd < 0) [[unlikely]] {
            fprintf(stderr, "[FATAL] LogFile::FileUtil failed to open file: %s, %s\n", 
                filename, strerror(errno));
            abort();
        }
    }

    FileUtil(const FileUtil&) = delete;
    FileUtil& operator=(const FileUtil&) = delete;

    ~FileUtil() {
        if (_fd >= 0) [[likely]] {
            ::close(_fd);
        }
    }

    void append(std::string_view data) noexcept {
        // 阈值设定：大于等于 64KB 的数据，直接写入磁盘
        if (data.size() >= kBufferThreshold) {
            // 先把当前缓冲区里残留的数据刷入磁盘，保证日志顺序
            flush();
            // 大数据直接零拷贝系统调用
            write_uninterrupted(data);
            return;
        }

        // 小数据逻辑：检查缓冲区剩余空间
        if (data.size() > _buffer.avail()) {
            // 缓冲区装不下了，先刷盘
            flush();
            // 刷盘后，因为 len < kBufferSize，现在肯定装得下了
        }

        // 极速内存拷贝
        _buffer.append(data);
    }

    void flush() noexcept {
        if (_buffer.length() > 0) {
            write_uninterrupted(_buffer.toStringView());
            _buffer.reset(); // 复用 FixedBuffer 的重置逻辑
        }
    }

    off_t writtenBytes() const noexcept { return _writtenBytes; }

private:
    // 封装底层的 write，处理 EINTR 和部分写入
    void write_uninterrupted(std::string_view data) noexcept {
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = ::write(_fd, data.data() + written, data.size() - written);
            if (n > 0) [[likely]] {
                written += n;
                _writtenBytes += n;
            } else if (n < 0) [[unlikely]] {
                if (errno == EINTR) continue;
                fprintf(stderr, "FileUtil write failed: %s\n", strerror(errno));
                break;
            } else {
                break;
            }
        }
    }

private:
    int _fd;
    off_t _writtenBytes = 0;
    FixedBuffer<kBufferSize> _buffer;
};

/**
 * @brief 日志文件管理类
 * * 功能：自动滚动日志（按大小或按天）、自动 Flush
 * * 因为使用时只有单线程使用，这里没有保证线程安全，如需要线程安全需要外部实现
 */
class LogFile {
public:
    LogFile(std::string_view basename, off_t rollsize,
                int flushInterval = 3, int checkEveryN = 1024) noexcept
    : _rollsize(rollsize),
      _flushInterval(flushInterval),
      _checkEveryN(checkEveryN),
      _basename(basename)
{
    rollFile(); // 构造时立即滚动一次，确保文件打开
}
    
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    LogFile(LogFile&&) = delete;
    LogFile& operator=(LogFile&&) = delete;
    
    // 写入日志          
    void append(std::string_view logLine) noexcept;

    // 强制刷新缓冲区到磁盘
    void flush() noexcept {
        FileUtil* ptr = std::launder(reinterpret_cast<FileUtil*>(&_file));
        ptr->flush();
    }

    // 手动滚动日志
    void rollFile() noexcept;

private:
    // 获取日志文件名
    // 格式: basename.YYYYMMDD-HHMMSS.hostname.pid.log
    void getLogFileName(time_t& now, char* p, size_t len) noexcept;

    static constexpr int kRollPerSeconds = 60 * 60 * 24; // 一天的秒数

    KzSTL::KzString _basename; // 日志文件名 base
    off_t _rollsize;    // 滚动大小阈值
    int _flushInterval; // 刷新间隔（秒）
    int _checkEveryN;   // 检查频次，每写入 N 次检查一次是否需要滚动 (减少 time 系统调用)

    int _count = 0;           // 写入计数器，用于 checkEveryN
    int _rollIndex = 0;       // 同一秒内的滚动计数器

    time_t _startOfPeriod = 0;// 当前日志文件所属的"天" (对齐到当天 00:00:00)
    time_t _lastRoll = 0;     // 上次滚动的时间戳
    time_t _lastFlush = 0;    // 上次 Flush 的时间戳

    alignas(FileUtil) std::byte _file[sizeof(FileUtil)]; // 底层文件操作对象

    TimeFormatter _timeFormatter; // 缓存秒级时间
};

}