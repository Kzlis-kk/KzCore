#pragma once

#include <cstddef>
#include "Timer.h"

namespace KzTimer {

class TimerList {
public:
    explicit TimerList() noexcept {
        _root.prev = &_root;
        _root.next = &_root;
    }

    ~TimerList() {
        assert(empty()); // 确保所有定时器都已被上层逻辑移除并回收
    }

    TimerList(const TimerList&) = delete;
    TimerList& operator=(const TimerList&) = delete;

    TimerList(TimerList&& other) noexcept {
        if (other.empty()) [[unlikely]] {
            _root.prev = &_root;
            _root.next = &_root;
        } 
        else {
            _root.next = other._root.next;
            _root.prev = other._root.prev;
            _root.next->prev = &_root;
            _root.prev->next = &_root;
            other._root.prev = &other._root;
            other._root.next = &other._root;
        }
    }

    TimerList& operator=(TimerList&& other) noexcept {
        if (this != &other) [[likely]] {
            assert(empty());
            if (other.empty()) [[unlikely]] {
                _root.prev = &_root;
                _root.next = &_root;
            }
            else {
                _root.next = other._root.next;
                _root.prev = other._root.prev;
                _root.next->prev = &_root;
                _root.prev->next = &_root;
                other._root.prev = &other._root;
                other._root.next = &other._root;
            }
        }
        return *this;
    }

    bool empty() const noexcept { return _root.next == &_root; }

    void push_back(Timer* timer) noexcept {
        Timer::Node& node = timer->_node;
        node.next = _root.next;
        node.prev = _root.prev;
        _root.prev->next = &node;
        _root.prev = &node;
    }

    void erase(Timer* timer) noexcept {
        Timer::Node& node = timer->_node;
        if (node.prev != nullptr && node.next != nullptr) [[likely]] {
            node.prev->next = node.next;
            node.next->prev = node.prev;
            node.next = nullptr;
            node.prev = nullptr;
        }
    }

    Timer* pop_front() noexcept {
        if (empty()) return nullptr;
        Timer::Node* node = _root.next;
        _root.next = node->next;
        node->next->prev = &_root;

        node->next = nullptr;
        node->prev = nullptr;

        return Timer::fromNode(node);
    }

private:
    Timer::Node _root;
};

} // namespace KzTimer 