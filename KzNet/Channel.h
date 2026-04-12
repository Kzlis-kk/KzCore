#pragma once

#include <memory>
#include <sys/epoll.h>
#include "KzSTL/Function.h"
#include "KzTimer/TimeStamp.h"
#include "KzSTL/KzString.h"
#include "KzAlgorithm/Jeaiii.h"
#include <cassert>
#include <cstring>

namespace KzNet {

class EventLoop;

/**
 * @brief IO 事件分发器
 * 
 * * 核心职责：
 *   1. 封装文件描述符 (fd) 及其感兴趣的事件 (events)。
 *   2. 绑定回调函数 (Read/Write/Close/Error)。
 *   3. 当 Poller 返回就绪事件 (revents) 时，执行对应回调。
 * 
 * * 特点：
 *   Channel 不拥有 fd，也不负责关闭 fd。
 *   它只属于一个 EventLoop，且所有操作必须在该 Loop 线程中执行。
 * 
 * * 优化：
 *   移除 tie() 机制，消除了每次事件处理时的 weak_ptr::lock() 原子开销。
 *   依赖上层 (TcpConnection) 采用 "延迟销毁 (queueInLoop)" 策略保证生命周期。
 */
class Channel {
public:
    using ReadEventCallback = KzSTL::Function<void(KzTimer::TimeStamp)>;
    using EventCallback = KzSTL::Function<void()>;

    // Epoll 事件常量
    static constexpr int kNoneEvent = 0;
    static constexpr int kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
    static constexpr int kWriteEvent = EPOLLOUT;

    Channel(EventLoop* loop, int fd) noexcept
    : _loop(loop),
      _fd(fd),
      _events(0),
      _revents(0),
      _index(-1) 
{}

    ~Channel() {
        assert(isNoneEvent());
    }

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /**
     * @brief 处理事件 (由 EventLoop 调用)
     * @param receiveTime 事件触发的时间戳 (用于计算延迟)
     */
    void handleEvent(KzTimer::TimeStamp receiveTime) noexcept;

    // === 回调注册 (支持 Move 语义) ===
    void setReadCallback(ReadEventCallback&& cb) noexcept { _readCallback = std::move(cb); }
    void setReadCallback(const ReadEventCallback& cb) noexcept { _readCallback = cb; }
    void setWriteCallback(EventCallback&& cb) noexcept { _writeCallback = std::move(cb); }
    void setWriteCallback(const EventCallback& cb) noexcept { _writeCallback = cb; }
    void setCloseCallback(EventCallback&& cb) noexcept { _closeCallback = std::move(cb); }
    void setCloseCallback(const EventCallback& cb) noexcept { _closeCallback = cb; }
    void setErrorCallback(EventCallback&& cb) noexcept { _errorCallback = std::move(cb); }\
    void setErrorCallback(const EventCallback& cb) noexcept { _errorCallback = cb; }

    // 成员状态
    void changeFd(int fd) noexcept { _fd = fd; }
    int fd() const noexcept { return _fd; }
    int events() const noexcept { return _events; }

    // 由 Poller 设置实际触发的事件
    void set_revent(int revt) noexcept { _revents = revt; }

    // 判断当前是否没有任何感兴趣的事件
    bool isNoneEvent() const noexcept { return _events == kNoneEvent; }

    // === 事件控制 ===
    void enableReading() noexcept { _events |= kReadEvent; update(); }
    void disableReading() noexcept { _events &= ~kReadEvent; update(); }
    void enableWriting() noexcept { _events |= kWriteEvent; update(); }
    void disableWriting() noexcept { _events &= ~kWriteEvent; update(); }
    void disableAll() noexcept { _events = kNoneEvent; update(); }

    bool isWriting() const noexcept { return _events & kWriteEvent; }
    bool isReading() const noexcept { return _events & kReadEvent; }

    // Poller交互
    // index 在 Poller 中的状态 (-1: 新增, 1: 已添加, 2: 已删除)
    int index() noexcept { return _index; }
    void setIndex(int idx) noexcept { _index = idx; }

    // 从 Loop 中移除此 Channel
    void remove() noexcept;

    // 向 Loop 更新事件状态
    void update() noexcept;

    EventLoop* ownerLoop() noexcept { return _loop; }

    // 调试辅助
    KzSTL::KzString reventsToString() const noexcept {
        return eventsToStringHelper(_revents);
    }
    KzSTL::KzString eventsToString() const noexcept {
        return eventsToStringHelper(_events);
    }

private:

    EventLoop* _loop;
    int  _fd;
    int  _events;  // 用户关心的事件
    int  _revents; // Poller 返回的就绪事件
    int  _index;   // Poller 中的状态索引

    // 回调函数
    ReadEventCallback _readCallback;
    EventCallback _writeCallback;
    EventCallback _closeCallback;
    EventCallback _errorCallback;

    KzSTL::KzString eventsToStringHelper(int ev) const noexcept;
};

inline KzSTL::KzString Channel::eventsToStringHelper(int ev) const noexcept {
    char buf[64];
    char* p = KzAlgorithm::Jeaiii::to_chars(buf, _fd);
    *p++ = ':'; *p++ = ' '; // 极速写入 ": "
    
    if (ev & EPOLLIN)    { ::memcpy(p, "IN ", 3); p += 3; }
    if (ev & EPOLLPRI)   { ::memcpy(p, "PRI ", 4); p += 4; }
    if (ev & EPOLLOUT)   { ::memcpy(p, "OUT ", 4); p += 4; }
    if (ev & EPOLLHUP)   { ::memcpy(p, "HUP ", 4); p += 4; }
    if (ev & EPOLLRDHUP) { ::memcpy(p, "RDHUP ", 6); p += 6; }
    if (ev & EPOLLERR)   { ::memcpy(p, "ERR ", 4); p += 4; }

    return KzSTL::KzString(buf, p - buf);
}

inline void Channel::handleEvent(KzTimer::TimeStamp receiveTime) noexcept {
    // 上层只会使用 queueInLoop 延迟销毁，故保证执行期间的生命周期

    // 对端挂起 (EPOLLHUP)
    // 当对端关闭，会收到 HUP。但如果还有数据没读完 (EPOLLIN)，则先不处理 close，
    // 而是去读数据，读到 0 字节时由 ReadCallback 处理关闭。
    if ((_revents & EPOLLHUP) && !(_revents & EPOLLIN)) {
        if (_closeCallback) _closeCallback();
        return;
    }

    // 错误 (EPOLLERR)
    if (_revents & EPOLLERR) {
        // 获取具体的错误信息用于日志
        if (_errorCallback) _errorCallback();
        return;
    }

    // 可读 (EPOLLIN | EPOLLPRI | EPOLLRDHUP)
    // 这里，没有区分PRI(紧急指针数据)，仅当作正常数据处理
    // 这里，没有区分RDHUP(对端关闭，但是缓冲区可能还有数据)，虽然可以避开通过read() == 0来判断，但是减少一次系统调用不如绝不丢数据好
    if (_revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (_readCallback) _readCallback(receiveTime);
    }

    // 可写 (EPOLLOUT)
    if (_revents & EPOLLOUT) {
        if (_writeCallback) _writeCallback();
    }
}

} // namespace KzNet