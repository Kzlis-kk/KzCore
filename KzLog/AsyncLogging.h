#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include "ThreadLocalBuffer.h"
#include "KzSTL/SpinMutex.h"
#include "KzAlloc/RAII_Utils.h"
#include "LogFile.h"
#include "KzThread/ThreadUtils.h"

namespace KzLog {

// 包装一下 Buffer，加入线程存活状态
struct ThreadBufferNode {
    ThreadLocalBuffer<> buffer; 
    std::atomic<bool> is_dead{false}; 
};

// 捕获线程死亡事件：利用 thread_local 的自定义析构函数来标记线程死亡
thread_local struct ThreadExiter {
    std::shared_ptr<ThreadBufferNode> node_ptr;
    ~ThreadExiter() {
        if (node_ptr) {
            // 线程退出时，标记为死亡 (Release 语义保证之前的写入可见)
            node_ptr->is_dead.store(true, std::memory_order_release);
        }
    }
} g_thread_exiter;

class AsyncLogging {
public:
    AsyncLogging(std::string_view basename, off_t rollsize, int flushInterval = 3, int checkEveryN = 1024) noexcept
        : _logfile(KzAlloc::allocate_unique<LogFile>(basename, rollsize, flushInterval, checkEveryN)) {
        // 启动后端线程，传入 stop_token
        _backend_thread = std::jthread([this](std::stop_token st) {
            this->backendThreadFunc(std::move(st)); // 全局无异常，不处理
        });
    }

    AsyncLogging(const AsyncLogging&) = delete;
    AsyncLogging& operator=(const AsyncLogging&) = delete;

    AsyncLogging(AsyncLogging&&) = delete;
    AsyncLogging& operator=(AsyncLogging&&) = delete;

    // 业务线程第一次写日志时调用
    ThreadLocalBuffer<>* registerThread() noexcept {
        auto node = std::allocate_shared<ThreadBufferNode>(KzAlloc::KzAllocator<ThreadBufferNode>());
        
        {
            std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
            _buffers.push_back(node);
        }
        _dirty.store(true, std::memory_order_release);

        g_thread_exiter.node_ptr = node;
        return &(node->buffer);
    }

    // 后端消费线程的主循环
    void backendThreadFunc(std::stop_token stoken) noexcept {
        KzThread::ThreadUtils::set_thread_name("Logger_backend");

        std::vector<std::shared_ptr<ThreadBufferNode>, KzAlloc::KzAllocator<std::shared_ptr<ThreadBufferNode>>> local_buffers;
        while (!stoken.stop_requested()) {
            bool has_data = false;

            // 拷贝一份注册表的快照
            if (_dirty.load(std::memory_order_acquire)) {
                std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
                local_buffers = _buffers;
                _dirty.store(false, std::memory_order_relaxed);
            }

            // 遍历所有线程的 Buffer
            for (auto it = local_buffers.begin(); it != local_buffers.end(); ) {
                auto& node = *it;
                
                // 消费数据，直接透传给 LogFile
                size_t consumed = node->buffer.consume([&](std::string_view data) {
                    _logfile->append(data);
                });

                if (consumed > 0) has_data = true;

                // 检查丢弃计数器
                uint64_t drops = node->buffer.fetch_and_clear_drop_count();
                if (drops > 0) [[unlikely]] {
                    char warn_buf[128];
                    int n = snprintf(warn_buf, sizeof(warn_buf), 
                        "[WARNING] Dropped %lu logs due to slow backend\n", drops);
                    _logfile->append(std::string_view(warn_buf, n));
                }

                // 垃圾回收
                // 如果线程死了，且 Buffer 里的数据被我们抽干了
                if (node->is_dead.load(std::memory_order_acquire) && consumed == 0) [[unlikely]] {
                    std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
                    // 从全局注册表中彻底移除，shared_ptr 引用计数归零，内存释放！
                    std::erase(_buffers, node);
                    it = local_buffers.erase(it);
                    // 后端单线程，自己改 buffers 就不需要改 _dirty
                } else {
                    ++it;
                }
            }

            // 检查是否有前端发起了 Flush 请求
            uint64_t req_seq = _flush_request_seq.load(std::memory_order_relaxed);
            uint64_t done_seq = _flush_done_seq.load(std::memory_order_relaxed);

            if (req_seq > done_seq) [[unlikely]] {
                // 强制将内核 Page Cache 刷入物理磁盘
                _logfile->flush();

                // 更新已完成序列号，并唤醒所有等待的前端线程
                _flush_done_seq.store(req_seq, std::memory_order_release);
                _flush_done_seq.notify_all();
            }

            // 自适应退避
            if (!has_data) {
                // 没数据时让出时间片
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                // 有数据就刷入磁盘
                _logfile->flush();
            }
        }
        for (auto& node : local_buffers) {
            node->buffer.consume([this](std::string_view data) {
                _logfile->append(data);
            });
        }
        _logfile->flush();
    }

    // 前端使用
    void flush() noexcept {
        // 提交一个新的 Flush 请求，获取目标序列号
        uint64_t target_seq = _flush_request_seq.fetch_add(1, std::memory_order_relaxed) + 1;

        // 唤醒可能正在 sleep 的后端线程 (如果有独立的唤醒机制，比如 eventfd，在这里触发)
        // 简单起见，如果后端是高频轮询，这里可以省略唤醒动作

        // 只要已完成的序列号 < 目标序列号，就一直等
        uint64_t done_seq = _flush_done_seq.load(std::memory_order_acquire);
        while (done_seq < target_seq) {
            // 陷入内核等待，直到后端调用 _flush_done_seq.notify_all()
            _flush_done_seq.wait(done_seq, std::memory_order_acquire);
            
            // 被唤醒后重新加载最新的 done_seq
            done_seq = _flush_done_seq.load(std::memory_order_acquire);
        }
    }


private:
    std::vector<std::shared_ptr<ThreadBufferNode>, KzAlloc::KzAllocator<std::shared_ptr<ThreadBufferNode>>> _buffers;
    mutable KzSTL::SpinMutex _mutex;
    std::atomic<bool> _dirty{false}; // 脏标记
    // Flush 序列号机制
    std::atomic<uint64_t> _flush_request_seq{0};
    std::atomic<uint64_t> _flush_done_seq{0};
    
    using LogFilePtr = std::unique_ptr<LogFile, KzAlloc::AllocatorDeleter<LogFile, KzAlloc::KzAllocator<LogFile>>>;
    LogFilePtr _logfile;
    std::jthread _backend_thread; // jthread退出时要使用 logfile，必须优先析构
};

} // namespace KzLog