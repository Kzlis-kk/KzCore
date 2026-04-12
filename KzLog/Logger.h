#pragma once

#include "LogStream.h"
#include <memory>

namespace KzLog {

enum class LogLevel : uint8_t {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    Error,
    FATAL
};

// 全局初始化接口 (由 main 函数调用)
inline constexpr off_t kDefaultRollsize = 512* 1024 * 1024;
void InitAsyncLogger(std::string_view basename, off_t rollsize = kDefaultRollsize, int flushInterval = 3, int checkEveryN = 1024) noexcept;
// 全局清理接口
void StopAsyncLogger() noexcept;

// 编译器剥离路径前缀
class SourceFile {
public:
    template <int N>
    constexpr SourceFile(const char (&arr)[N]) : _data(arr), _size(N - 1) {
        const char* slash = nullptr;
        for (int i = 0; i < _size; i++) {
            if (arr[i] == '/') {
                slash = &arr[i];
            }
        }
        if (slash) {
            _data = slash + 1;
            _size -= static_cast<int>(_data - arr);
        }
    }

    constexpr std::string_view toStringView() {
        return std::string_view(_data, _size);
    }

    const char* _data;
    int _size;
};

/**
 * @brief RAII 日志前端封装
 * * 生命周期：
 * 构造时：写入 [时间戳] [级别][线程ID] [文件:行号]
 * 析构时：写入 \n，并推入无锁队列
 */
class Logger {
public:
    Logger(LogLevel level, SourceFile file, int line) noexcept;
    
    ~Logger();

    // 禁用拷贝和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogStream& stream() noexcept { return _stream; }

private:
    LogStream _stream;
    LogLevel _level;

};

// 宏定义：为了方便使用和条件编译
#define LOG_TRACE KzLog::Logger(KzLog::LogLevel::TRACE, __FILE__, __LINE__).stream()
#define LOG_DEBUG KzLog::Logger(KzLog::LogLevel::DEBUG, __FILE__, __LINE__).stream()
#define LOG_INFO  KzLog::Logger(KzLog::LogLevel::INFO,  __FILE__, __LINE__).stream()
#define LOG_WARN  KzLog::Logger(KzLog::LogLevel::WARN,  __FILE__, __LINE__).stream()
#define LOG_ERROR KzLog::Logger(KzLog::LogLevel::Error, __FILE__, __LINE__).stream()
#define LOG_SYSERR KzLog::Logger(KzLog::LogLevel::Error, __FILE__, __LINE__).stream()
#define LOG_FATAL KzLog::Logger(KzLog::LogLevel::FATAL, __FILE__, __LINE__).stream()
#define LOG_SYSFATAL KzLog::Logger(KzLog::LogLevel::FATAL, __FILE__, __LINE__).stream()

} // namespace KzLog