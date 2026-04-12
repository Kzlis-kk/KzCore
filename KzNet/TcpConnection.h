#pragma once

#include <memory>
#include <string_view>
#include <any>
#include <atomic>
#include <bit>
#include <concepts>
#include <sys/uio.h>
#include "Socket.h"
#include "Channel.h"
#include "Buffer.h"
#include "InetAddress.h"
#include "EventLoop.h"
#include "KzSTL/KzAny.h"
#include "KzTimer/TimeStamp.h"
#include "KzSTL/Function.h"
#include "KzSTL/Task.h"
#include "KzAlloc/KzAllocator.h"
#include "KzSTL/KzString.h"
#include "KzLog/Logger.h"
#include "ConnTracer.h"

namespace KzNet {

namespace detail {
class CoroutineQueue {
public:
    using Handle = std::coroutine_handle<>;

    CoroutineQueue(const CoroutineQueue&) = delete;
    CoroutineQueue& operator=(const CoroutineQueue&) = delete;

    CoroutineQueue(size_t initCap = 16) noexcept
        : _head(0), _tail(0), _size(0) 
    {
        // 向上取整，保证 capacity 是 2 的幂
        _capacity = std::bit_ceil(initCap);
        
        _buf = static_cast<Handle*>(KzAlloc::malloc(sizeof(Handle) * _capacity));
    }

    ~CoroutineQueue() {
        if (_buf) [[likely]] KzAlloc::free(_buf, sizeof(Handle) * _capacity);
    }

    void push(Handle h) noexcept {
        if (_size == _capacity) [[unlikely]] {
            grow();
        }
        _buf[_tail] = h;
        _tail = (_tail + 1) & (_capacity - 1); // 环形递增
        _size++;
    }

    Handle pop() noexcept {
        if (_size == 0) return nullptr;
        
        Handle h = _buf[_head];
        _head = (_head + 1) & (_capacity - 1); // 环形递增
        _size--;
        return h;
    }

    bool empty() const noexcept { return _size == 0; }
    size_t size() const noexcept { return _size; }

private:
    void grow() {
        size_t newCap = _capacity * 2;
        Handle* newBuf = static_cast<Handle*>(KzAlloc::malloc(sizeof(Handle) * newCap));
        
        // 搬运数据：因为是环形的，可能分两段
        // [ ... tail ... head ... ]
        if (_head < _tail) {
            // 若没有回绕，直接拷贝
            ::memcpy(newBuf, _buf + _head, _size * sizeof(Handle));
        } else {
            // 回绕了就拷贝两段
            // head 到末尾
            size_t firstPart = _capacity - _head;
            ::memcpy(newBuf, _buf + _head, firstPart * sizeof(Handle));
            // 开头 到 tail
            ::memcpy(newBuf + firstPart, _buf, _tail * sizeof(Handle));
        }

        KzAlloc::free(_buf, sizeof(Handle) * _capacity);
        _buf = newBuf;
        _capacity = newCap;
        _head = 0;
        _tail = _size;
    }

    Handle* _buf;
    size_t _capacity;
    size_t _head;
    size_t _tail;
    size_t _size;
};

struct StringPiece {
    KzSTL::KzString str;
    size_t offset = 0;
};

class WriteQueue {
public:
    WriteQueue(size_t initCap = 16) noexcept
        : _head(0), _tail(0), _size(0), _totalBytes(0) 
    {
        _capacity = std::bit_ceil(initCap);
        _buf = static_cast<StringPiece*>(KzAlloc::malloc(sizeof(StringPiece) * _capacity));
    }

    ~WriteQueue() {
        if (_buf) [[likely]] {
            clear(); // 显式调用析构函数释放 KzString
            KzAlloc::free(_buf, sizeof(StringPiece) * _capacity);
        }
    }

    // 禁用拷贝
    WriteQueue(const WriteQueue&) = delete;
    WriteQueue& operator=(const WriteQueue&) = delete;

    // 核心逻辑：直接消费 N 个字节，自动处理跨块和出队
    void consume(size_t n) noexcept {
        _totalBytes -= n;
        while (n > 0 && _size > 0) {
            StringPiece& piece = _buf[_head];
            size_t pieceLen = piece.str.size() - piece.offset;
            
            if (n >= pieceLen) {
                n -= pieceLen;
                piece.~StringPiece(); // 显式析构，释放 KzString 内存
                _head = (_head + 1) & (_capacity - 1);
                _size--;
            } else {
                piece.offset += n;
                n = 0;
            }
        }
    }

    // 填充 iovec 数组，返回填充的数量
    int fillIov(struct iovec* iov, int maxIov) const noexcept {
        int count = 0;
        size_t idx = _head;
        for (size_t i = 0; i < _size && count < maxIov; ++i) {
            iov[count].iov_base = _buf[idx].str.data() + _buf[idx].offset;
            iov[count].iov_len = _buf[idx].str.size() - _buf[idx].offset;
            count++;
            idx = (idx + 1) & (_capacity - 1);
        }
        return count;
    }

    void push_or_append(KzSTL::KzString&& msg, size_t offset) noexcept {
        size_t len = msg.size() - offset;
        if (len == 0) [[unlikely]] return;

        // 尝试尾部聚合
        if (_size > 0) {
            // 获取队尾元素，下溢时自动回绕
            size_t tailIdx = (_tail - 1) & (_capacity - 1);
            StringPiece& tail = _buf[tailIdx];
            size_t tailLen = tail.str.size() - tail.offset;

            // 聚合条件：队尾是小包，且新来的也是小包 (阈值 4KB)
            if (tailLen < 4096 && len < 4096) {
                // 直接追加到队尾的 KzString 中
                tail.str.append(msg.data() + offset, len);
                _totalBytes += len;
                return; // 聚合成功，直接返回
            }
        }

        // 无法聚合（大包，或者队列为空），作为新节点压入
        if (_size == _capacity) [[unlikely]] grow();
    
        new (&_buf[_tail]) StringPiece{std::move(msg), offset};
        _totalBytes += len;
    
        _tail = (_tail + 1) & (_capacity - 1);
        _size++;
    }

    void push_or_append(std::string_view msg) noexcept {
        size_t len = msg.size();
        if (len == 0) [[unlikely]] return;

        // 尝试尾部聚合
        if (_size > 0) {
            // 获取队尾元素
            size_t tailIdx = (_tail - 1) & (_capacity - 1);
            StringPiece& tail = _buf[tailIdx];
            size_t tailLen = tail.str.size() - tail.offset;

            // 聚合条件：队尾是小包，且新来的也是小包 (阈值 4KB)
            if (tailLen < 4096 && len < 4096) {
                // 直接将 string_view 追加到队尾的 KzString 中
                tail.str.append(msg.data(), len);
                _totalBytes += len;
                return; // 聚合成功，直接返回
            }
        }

        // 无法聚合（大包，或者队列为空），作为新节点压入
        if (_size == _capacity) [[unlikely]] grow();
    
        // 在这里才真正触发 KzString 的构造（1次内存分配 + 1次拷贝）
        new (&_buf[_tail]) StringPiece{KzSTL::KzString(msg), 0};
        _totalBytes += len;
    
        _tail = (_tail + 1) & (_capacity - 1);
        _size++;
    }

    bool empty() const noexcept { return _size == 0; }
    size_t totalBytes() const noexcept { return _totalBytes; }

private:
    void clear() noexcept {
        while (_size > 0) {
            _buf[_head].~StringPiece();
            _head = (_head + 1) & (_capacity - 1);
            _size--;
        }
    }

    void grow() {
        size_t newCap = _capacity * 2;
        StringPiece* newBuf = static_cast<StringPiece*>(KzAlloc::malloc(sizeof(StringPiece) * newCap));
        
        // 环形数组搬运，必须使用 std::move 和 placement new
        size_t idx = _head;
        for (size_t i = 0; i < _size; ++i) {
            new (&newBuf[i]) StringPiece{std::move(_buf[idx].str), _buf[idx].offset};
            _buf[idx].~StringPiece();
            idx = (idx + 1) & (_capacity - 1);
        }

        KzAlloc::free(_buf, sizeof(StringPiece) * _capacity);
        _buf = newBuf;
        _capacity = newCap;
        _head = 0;
        _tail = _size;
    }

    StringPiece* _buf;
    size_t _capacity;
    size_t _head;
    size_t _tail;
    size_t _size;
    size_t _totalBytes; // 缓存总字节数，O(1) 获取水位
};

} // namespace detail

/**
 * @brief TCP 连接封装
 * 
 * * 职责：
 *   1. 封装 Socket 和 Channel，管理连接的生命周期。
 *   2. 维护 Input/Output Buffer。
 *   3. 处理粘包/拆包 (通过 Buffer)，向用户回调完整的 Message。
 *   4. 提供高性能发送接口 (Direct Write)。
 * 
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    // 回调定义
    using ConnectionCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&,
        Buffer*, KzTimer::TimeStamp)>;
    using WriteCompleteCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&, size_t)>;
    using CloseCallback = KzSTL::Function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpConnection(EventLoop* loop, int sockfd, const InetAddress& localAddr,
        const InetAddress& peerAddr, std::string_view name) noexcept;
    ~TcpConnection() {
        assert(_state.load(std::memory_order_acquire) == StateE::DISCONNECTED);
    }

    // === 基础信息 ===
    EventLoop* getLoop() const noexcept { return _loop; }
    std::string_view getname() const noexcept { return std::string_view(_name); }
    const InetAddress& localAddress() noexcept { return _localAddr; }
    const InetAddress& peerAddress() noexcept { return _peerAddr; }
    int fd() const noexcept { return _socket.fd(); }
    bool connected() const noexcept { return _state == StateE::CONNECTED; }
    bool disconnected() const noexcept { return _state == StateE::DISCONNECTED; }


    // === 发送接口 ===
    // 非阻塞发送 (Fire and Forget)
    // 如果缓冲区满，会触发 HighWaterMarkCallback，但不会阻塞。
    void send(KzSTL::KzString&& message) noexcept {
        if (_state.load(std::memory_order_acquire) == StateE::CONNECTED) [[likely]] {
            if (_loop->isInLoopThread()) {
                sendInLoop(std::move(message));
            } else {
                // 跨线程
                _loop->runInLoop([self = shared_from_this(), str = std::move(message)]() mutable noexcept {
                    self->sendInLoop(std::move(str));
                });
            }
        }
    }
    void send(std::string_view message) noexcept {
        if (_state.load(std::memory_order_acquire) == StateE::CONNECTED) [[likely]] {
            if (_loop->isInLoopThread()) {
                // 同线程：直接走快速路径，可能完全不需要拷贝
                sendInLoop(message);
            } else {
                // 跨线程：必须立刻拷贝，因为 string_view 指向的内存可能在回调执行前被销毁
                _loop->runInLoop([self = shared_from_this(), str = KzSTL::KzString(message)]() mutable noexcept {
                    self->sendInLoop(std::move(str));
                });
            }
        }
    }
// === 协程流控实现 ===
struct SendAwaiter {
    std::shared_ptr<TcpConnection> conn;
    KzSTL::KzString msg;

    bool await_ready() const { 
        // 如果连接已经断了，直接不挂起，让它去 await_resume 处理
        // 如果当前 Buffer 没满，直接发送，不挂起
        return conn->disconnected() || conn->_outputQueue.totalBytes() < conn->_highWaterMark; 
    }

    bool await_suspend(std::coroutine_handle<> h) {
        // 将协程句柄加入等待队列
        if (conn->_loop->isInLoopThread()) {
            // 这里不用检查水位，因为 IO 线程同步执行中途不可能水位变化
            if (conn->disconnected()) {
                return false; // 返回 false：取消挂起，立刻执行 await_resume
            } 
            else {
                conn->_blockedCoroutines.push(h);
            }
            return true; // 返回 true：真正挂起
        } else {
            // 必须捕获成员而不是 this,避免任务还未执行但是协程帧已经销毁
            conn->_loop->runInLoop([c = this->conn, h]() mutable noexcept {
                // 再次检查水位，如果还是高，入队；否则直接恢复
                if (c->disconnected() || c->_outputQueue.totalBytes() < c->_highWaterMark) {
                    h.resume();
                } else {
                    c->_blockedCoroutines.push(h);
                }
            });
             return true; // 当前计算线程的协程真正挂起
        }
    }

    void await_resume() { 
        // 恢复后检查连接状态
        if (conn->connected()) {
            conn->send(std::move(msg));
        } else {
            // 连接已断开，消息丢弃
            // 协程正常结束，Task 正常析构，完美闭环。
            LOG_WARN << "Connection closed, drop async message.";
        }
    }
};

    // 协程流控发送
    // 如果缓冲区超过高水位，协程挂起；直到数据排空，协程恢复。
    // 实现了天然的"生产者-消费者"平衡。
    SendAwaiter sendAsync(KzSTL::KzString message) {
        return SendAwaiter{shared_from_this(), std::move(message)};
    }
    SendAwaiter sendAsync(std::string_view message) {
        return SendAwaiter{shared_from_this(), KzSTL::KzString(message)};
    }

    // === 连接控制 ===
    // 优雅关闭 (发送 FIN)
    void shutdown() noexcept;

    // 强制关闭 (RST / Close)
    void forceClose() noexcept;

    // Socket 选项
    void setTcpNoDelay(bool on) noexcept { _socket.setTcpNoDelay(on); }
    void setKeepAlive(bool on) noexcept { _socket.setKeepAlive(on); }

    // 设置高水位阈值 (默认 64MB)
    void setHighWaterMarkCallback(size_t highWaterMark) noexcept {
        _highWaterMark = highWaterMark;
    }

    // === 上下文管理 (用于存 HttpContext 等) ===
    void setContext(KzSTL::KzAny&& context) noexcept { _context = std::move(context); }
    const KzSTL::KzAny& getContext() noexcept { return _context; }
    KzSTL::KzAny* getMutableContext() noexcept { return &_context; }


    // === 内部生命周期回调 (由 TcpServer 调用) ===
    void connectEstablished() noexcept; // 连接建立
    void connectDestroyed() noexcept;   // 连接销毁
    
    
    // === 设置用户回调 ===
    void setConnectionCallback(ConnectionCallback&& cb) noexcept { _connectionCallback = std::move(cb); }
    void setConnectionCallback(const ConnectionCallback& cb) noexcept { _connectionCallback = cb; }
    void setMessageCallback(MessageCallback&& cb) noexcept { _messageCallback = std::move(cb); }
    void setMessageCallback(const MessageCallback& cb) noexcept { _messageCallback = cb; }
    void setWriteCompleteCallback(WriteCompleteCallback&& cb) noexcept { _writeCompleteCallback = std::move(cb); }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) noexcept { _writeCompleteCallback = cb; }
    void setCloseCallback(CloseCallback&& cb) noexcept { _closeCallback = std::move(cb); }
    void setCloseCallback(const CloseCallback& cb) noexcept { _closeCallback = cb; }


private:
    enum class StateE { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING };

    // 内部事件处理
    void handleRead(KzTimer::TimeStamp) noexcept;
    void handleWrite() noexcept;
    void handleClose() noexcept;
    void handleError() noexcept;

    // 内部发送逻辑 (必须在 Loop 线程)
    void sendInLoop(std::string_view message) noexcept;
    void sendInLoop(KzSTL::KzString&& data) noexcept;
    void shutdownInLoop() noexcept;
    void forceCloseInLoop() noexcept;

    void setState(StateE s) noexcept { _state.store(s, std::memory_order_release); }
    std::string_view stateToString() const noexcept;

    // 协程唤醒辅助
    void checkCoroutineResume() noexcept;
private:
    EventLoop* _loop;
    KzSTL::KzString _name;

    // 状态机 (原子变量用于简单的跨线程状态检查，但核心逻辑依然在 Loop 内)
    std::atomic<StateE> _state;
    bool _reading;

    // 核心组件
    Socket _socket;
    Channel _channel;

    // 地址信息
    InetAddress _localAddr;
    InetAddress _peerAddr;

    // 缓冲区
    Buffer _inputBuffer;

    // 用户上下文
    KzSTL::KzAny _context;

    // 流控相关
    size_t _highWaterMark;

    // 协程挂起队列
    // 当 Buffer 满时，sendAsync 的协程句柄会存在这里
    detail::CoroutineQueue _blockedCoroutines;
    detail::WriteQueue _outputQueue;

    // 回调
    ConnectionCallback _connectionCallback;
    MessageCallback _messageCallback;
    WriteCompleteCallback _writeCompleteCallback;
    CloseCallback _closeCallback;
};

inline void TcpConnection::handleError() noexcept {
    int err = _socket.getSocketError();
    LOG_ERROR << "TcpConnection::handleError [" << _name 
              << "] - SO_ERROR = " << err << " " << strerror(err);
    forceCloseInLoop();
}


namespace callbacks {

// 默认连接回调：仅打印 TRACE 级别的日志
inline void defaultConnectionCallback(const std::shared_ptr<TcpConnection>& conn) noexcept {
    LOG_TRACE << conn->localAddress().toIpPortString() << " -> "
              << conn->peerAddress().toIpPortString() << " is "
              << (conn->connected() ? "UP" : "DOWN");
}

// 默认消息回调：必须清空 Buffer，否则会导致内存泄漏
inline void defaultMessageCallback(const std::shared_ptr<TcpConnection>& conn, 
                            Buffer* buf, 
                            KzTimer::TimeStamp receiveTime) noexcept {
    // 丢弃所有收到的数据
    buf->retrieveAll(); 
}

// 默认发送完成回调：什么都不做
inline void defaultWriteCompleteCallback(const std::shared_ptr<TcpConnection>& conn) {
    // No-op
}

// 默认高水位回调：打印一条警告日志
inline void defaultHighWaterMarkCallback(const std::shared_ptr<TcpConnection>& conn, size_t mark) {
    LOG_WARN << "High water mark triggered on connection[" << conn->getname() 
             << "], current buffer size: " << mark << " bytes.";
}

} // namespace callbacks

} // namespace KzNet