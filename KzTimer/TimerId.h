#pragma once

#include "Timer.h"
#include <cstdint>

namespace KzTimer {

class TimerId {
public:
    TimerId() noexcept : _index(-1), _sequence(0) {}
    explicit TimerId(int32_t index, int64_t seq) noexcept : _index(index), _sequence(seq) {}

    int32_t _index;
    int64_t _sequence;
};

} // namespace KzTimer