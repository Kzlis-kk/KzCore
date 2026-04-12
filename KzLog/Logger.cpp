#include "Logger.h"
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "AsyncLogging.h"
#include "KzAlgorithm/Jeaiii.h"
#include "KzAlloc/RAII_Utils.h"
#include "KzThread/CurrentThread.h"

namespace KzLog {

// 全局的异步日志引擎指针
static std::unique_ptr<AsyncLogging, KzAlloc::AllocatorDeleter<AsyncLogging, KzAlloc::KzAllocator<AsyncLogging>>> g_asyncLogger = nullptr;
// 当前线程的无锁队列指针
thread_local ThreadLocalBuffer<>* t_local_buffer = nullptr;

// 秒级时间戳缓存
thread_local time_t t_last_second = 0;
thread_local char t_time_cache[32]; // "YYYYMMDD-HHMMSS"

// ============================================================================
// 辅助函数
// ============================================================================

void InitAsyncLogger(std::string_view basename, off_t rollsize, int flushInterval, int checkEveryN) noexcept {
    g_asyncLogger = KzAlloc::allocate_unique<AsyncLogging>(basename, rollsize, flushInterval, checkEveryN);
}

void StopAsyncLogger() noexcept {
    g_asyncLogger.reset();
}

// 级别字符串映射
constexpr std::string_view LogLevelName[6] = {
    "[TRACE] ", "[DEBUG] ", "[INFO ] ", "[WARN ] ", "[ERROR] ", "[FATAL] "
};


Logger::Logger(LogLevel level, SourceFile file, int line) noexcept : _level(level) {
    // 获取高精度时间 (微秒级)
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);

    // 秒级缓存优化
    if (tv.tv_sec != t_last_second) {
        t_last_second = tv.tv_sec;
        struct tm tm_time;
        ::localtime_r(&t_last_second, &tm_time);
        
        // 格式化 "YYYYMMDD-HHMMSS."
        int year = tm_time.tm_year + 1900;
        KzAlgorithm::Jeaiii::write_pair(t_time_cache, year / 100);
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 2, year % 100);
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 4, tm_time.tm_mon + 1);
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 6, tm_time.tm_mday);
        t_time_cache[8] = '-';
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 9, tm_time.tm_hour);
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 11, tm_time.tm_min);
        KzAlgorithm::Jeaiii::write_pair(t_time_cache + 13, tm_time.tm_sec);
        t_time_cache[15] = '.';
    }

    KzAlgorithm::Jeaiii::write_pair(t_time_cache + 16, tv.tv_usec / 10000);
    KzAlgorithm::Jeaiii::write_pair(t_time_cache + 18, (tv.tv_usec % 10000) / 100);
    KzAlgorithm::Jeaiii::write_pair(t_time_cache + 20, tv.tv_usec % 100);
    // 写入时间戳 (缓存的秒 + 实时计算的微秒)
    _stream.append(std::string_view(t_time_cache, 22));

    // 写入线程 ID
    _stream.append(KzThread::CurrentThread::tidString());

    // 写入日志级别
    _stream.append(LogLevelName[static_cast<uint8_t>(_level)]);

    _stream << '[' << file.toStringView() << ':' << line << "] ";
}

Logger::~Logger() {
    // 追加换行符
    _stream << '\n';
    
    const auto& buf = _stream.buffer();

    // 致命错误处理
    if (_level == LogLevel::FATAL) [[unlikely]] {
        // 同步输出到标准错误
        ::write(STDERR_FILENO, buf.data(), buf.length());
        
        // 如果后端引擎存在，强制同步刷盘
        if (g_asyncLogger) {
            g_asyncLogger->flush();
        }
        
        ::abort();
    }

    // 正常异步写入
    if (g_asyncLogger) [[likely]] {
        // 当前线程第一次写日志时，向全局注册表注册
        if (t_local_buffer == nullptr) [[unlikely]] {
            t_local_buffer = g_asyncLogger->registerThread();
        }
        
        // 无锁推入队列
        t_local_buffer->push(buf.toStringView());
    } else {
        // 如果引擎没初始化，直接打到终端
        ::write(STDOUT_FILENO, buf.data(), buf.length());
    }
}

} // namespace KzLog