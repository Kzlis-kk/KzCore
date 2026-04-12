#pragma once

#include <thread>
#include <string_view>
#include <new>
#include <semaphore>
#include <cassert>
#include <cstddef>

#include "KzThread/ThreadUtils.h"
#include "KzSTL/Function.h"
#include "KzSTL/KzString.h"
#include "EventLoop.h"

using namespace std::string_view_literals;

namespace KzNet {

/**
 * @brief IO 线程封装 (Header-only 实现)
 * 
 * * 核心职责：
 *   1. 启动一个新线程 (std::jthread)。
 *   2. 在该线程栈/预留内存上创建一个 EventLoop。
 *   3. 运行 loop() 循环。
 *   4. 提供同步机制，确保 startLoop() 返回时 Loop 已就绪。
 */
class EventLoopThread {
public:
    using ThreadInitCallback = KzSTL::Function<void(EventLoop*)>;

    explicit EventLoopThread() noexcept = default;

    ~EventLoopThread() {
        if (_thread.joinable()) {
            std::launder(reinterpret_cast<EventLoop*>(_loop))->quit();
        }
    }

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    /**
     * @brief 启动线程并运行 Loop
     * * 同步方法：会阻塞直到子线程的 EventLoop 初始化完成。
     * @return EventLoop* 指向子线程预留内存上的 Loop 对象
     */
    EventLoop* startLoop(int thread_id, ThreadInitCallback _callback = ThreadInitCallback(), std::string_view name = "EventLoopThread"sv) noexcept {
        // 防止重复启动
        if (_thread.joinable()) {
            return std::launder(reinterpret_cast<EventLoop*>(_loop));
        }

        // 初始为 0 (红灯)
        std::binary_semaphore sem{0};

        // 启动子线程，noexcept
        _thread = std::jthread([this, id = thread_id, &sem, cb = std::move(_callback), n = KzSTL::KzString(name)]() mutable {
            this->threadFunc(id, sem, std::move(cb), std::string_view(n));
        });

        // 等待子线程初始化完成
        sem.acquire();

        return std::launder(reinterpret_cast<EventLoop*>(_loop));
    }

    EventLoop* getLoop() noexcept { return std::launder(reinterpret_cast<EventLoop*>(_loop)); }

private:
    void threadFunc(int id, std::binary_semaphore& sem, ThreadInitCallback _callback, std::string_view name) noexcept {
        // 设置线程名
        const unsigned int num_cores = std::thread::hardware_concurrency();
        KzThread::ThreadUtils::set_thread_name(name);
        KzThread::ThreadUtils::pin_to_core(id % num_cores);
        KzThread::ThreadUtils::enable_local_memory_policy();

        new (&_loop) EventLoop();

        // 执行用户注入的初始化回调
        if (_callback) {
            _callback(std::launder(reinterpret_cast<EventLoop*>(_loop)));
        }
        
        // 唤醒主线程
        sem.release();

        // 开始事件循环 (线程将阻塞在这里，直到 quit() 被调用)
        std::launder(reinterpret_cast<EventLoop*>(_loop))->loop();
        
        // loop() 退出了，说明线程即将结束。
        // 安全地手动调用析构函数，释放 FD 和内存
        std::launder(reinterpret_cast<EventLoop*>(_loop))->~EventLoop();
    }

private:
    // 使用对齐存储来预留 EventLoop 的内存空间
    // 保证内存布局的绝对连续性
    alignas(EventLoop) std::byte _loop[sizeof(EventLoop)];
    std::jthread _thread;
};

} // namespace KzNet