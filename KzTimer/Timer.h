#pragma once

#include "TimeStamp.h"
#include "KzSTL/Job.h"
#include <atomic>

namespace KzTimer {

struct Timer {
    using Job = KzSTL::Job<void(), 40>;

    struct Node {
        Node* prev{nullptr};
        Node* next{nullptr};
    };

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    Timer(Job&& cb, TimeStamp when, int64_t intervalNs, bool strict) noexcept
        : _callback(std::move(cb)), _expiration(when),
          _intervalNs(intervalNs),
          _repeat(intervalNs > 0), _isStrict(strict), 
          _sequence(_numCreated.fetch_add(1, std::memory_order_relaxed)) {}

    ~Timer() {
        _callback = Job();
    }

    void run() {
       _callback();
    }

    void restart(TimeStamp now) noexcept {
        assert(_repeat);
        if (_isStrict) {
            _expiration = _expiration.addNano(_intervalNs);
        } else {
            _expiration = now.addNano(_intervalNs);
        }
    }

    static int64_t numCreated() noexcept { return  _numCreated.load(); }

    // 标准布局可以直接强转
    static Timer* fromNode(Node* node) noexcept {
        return reinterpret_cast<Timer*>(node); 
    }

    Node _node;             // 16B
    Job _callback;          // 56B
    TimeStamp _expiration;  // 8B
    const int64_t _intervalNs; // 8B
    const int64_t _sequence;   // 8B
    
    int32_t _slotIndex{-1};    // 4B
    int32_t _rotationCount{0}; // 4B
    int32_t _handleIndex{-1};  // 4B
    
    const bool _repeat;        // 1B
    const bool _isStrict;      // 1B
    bool _canceled{false};     // 1B

    inline static std::atomic<int64_t> _numCreated{0};
};

// 静态断言
static_assert(std::is_standard_layout_v<Timer>, "Timer POD check");
static_assert(offsetof(Timer, _node) == 0, "Timer::_node offset mismatch");

} // namespace KzTimer 