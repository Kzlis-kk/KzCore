#pragma once
#include "KzSTL/Job.h"
#include "TimeStamp.h"

namespace KzTimer {
struct TimerRequest {
    using Job = KzSTL::Job<void(), 40>;
    Job cb;
    TimeStamp when;
    int64_t intervalNs;
    bool isStrict;

    TimerRequest(Job _cb, TimeStamp _when, int64_t _intervalNs, bool _strict = false) noexcept
        : cb(std::move(_cb)), when(_when), intervalNs(_intervalNs), isStrict(_strict) {}

    TimerRequest(const TimerRequest&) = delete;
    TimerRequest& operator=(const TimerRequest&) = delete;

    TimerRequest(TimerRequest&&) noexcept = default;
    TimerRequest& operator=(TimerRequest&&) noexcept = default;
};
} // namespace KzTimer