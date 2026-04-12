#pragma once 

#include <time.h>
#include <cstdint>

namespace KzTimer {

class TimeStamp {
public:
    static constexpr int64_t kNanoSecondsPerSecond = 1000 * 1000 * 1000;
    
    explicit TimeStamp() noexcept : _ns(0) {}
    explicit TimeStamp(int64_t nanoseconds) noexcept : _ns(nanoseconds) {}

    static TimeStamp invalid() noexcept { return TimeStamp(); }
    bool valid() const { return _ns > 0; }

    static TimeStamp now() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return TimeStamp(ts.tv_sec * kNanoSecondsPerSecond + ts.tv_nsec);
    }

    static TimeStamp nowRealtime() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return TimeStamp(ts.tv_sec * kNanoSecondsPerSecond + ts.tv_nsec);
    }

    int64_t nanoseconds() const noexcept { return _ns; }

    TimeStamp addSeconds(int64_t seconds) noexcept {
        return TimeStamp(_ns + seconds * kNanoSecondsPerSecond);
    }

    TimeStamp addNano(int64_t ns) noexcept {
        return TimeStamp(_ns + ns);
    }

    static double timeDifference(TimeStamp high, TimeStamp low) noexcept {
        return static_cast<double>(high._ns - low._ns) / kNanoSecondsPerSecond;
    } 

    // C++20 重载比较运算符
    auto operator<=>(const TimeStamp&) const noexcept = default;

private:
    int64_t _ns;

};

static_assert(sizeof(TimeStamp) == sizeof(int64_t), "TimeStamp must be 8 bytes.");

} // namespace KzTimer