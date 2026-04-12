#pragma once

#include <vector>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <cassert>
#include <cerrno>

#include "Channel.h"
#include "KzAlloc/KzAllocator.h"
#include "KzTimer/TimeStamp.h"
#include "KzLog/Logger.h"

namespace KzNet {
class EventLoop;

/**
 * @brief IO 多路复用器
 *   数组映射：使用 vector<Channel*> 代替 map/unordered_map 管理 fd 到 Channel 的映射。
 *   利用 fd 是自增整数的特性，实现 O(1) 极速查找。
 */
class Poller {
public:
    static constexpr int kInitEventListSize = 16;
    using ChannelList = std::vector<Channel*, KzAlloc::KzAllocator<Channel*>>;

    Poller(EventLoop* loop) noexcept
    : _loop(loop),
      _epollfd(::epoll_create1(EPOLL_CLOEXEC)),
      _events(kInitEventListSize)
{
    if (_epollfd < 0) [[unlikely]] {
        LOG_SYSFATAL << "Poller::epoll_create1 failed";
    }
}
    ~Poller() {
        if (_epollfd > 0) [[likely]] {
            ::close(_epollfd);
        }
    }

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    /**
     * @brief 等待 IO 事件
     * @param timeoutMs 超时时间 (毫秒)
     * @param activeChannels [输出] 就绪的 Channel 列表
     * @return Timestamp 返回时的时刻
     */
    KzTimer::TimeStamp poll(int timeoutMs, ChannelList* activeChannels) noexcept;

    // 更新 Channel 状态
    // 对应 epoll_ctl 的 EPOLL_CTL_ADD / EPOLL_CTL_MOD
    void updateChannel(Channel* channel) noexcept;

    // 移除 Channel
    // 对应 epoll_ctl 的 EPOLL_CTL_DEL
    void removeChannel(Channel* channel) noexcept;

    // 判断 Channel 是否在当前 Poller 中
    bool hasChannel(Channel* channel) const noexcept;
     // 获取底层 epoll fd
    int epollFd() const { return _epollfd; }

private:

    // 填充活跃连接
    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const noexcept {
        for (int i = 0; i < numEvents; ++i) {
            // data.ptr 在 update() 时被设置为 Channel*
            Channel* channel = static_cast<Channel*>(_events[i].data.ptr);
            channel->set_revent(_events[i].events);
            activeChannels->push_back(channel);
        }
    }

    // 封装 epoll_ctl
    void update(int operation, Channel* channel) noexcept;

private:
    // Channel 在 Poller 中的状态 (存储在 Channel::_index 中)
    static constexpr int kNew = -1;     // 新增
    static constexpr int kAdded = 1;    // 已添加
    static constexpr int kDeleted = 2;  // 已删除

    EventLoop* _loop;
    int        _epollfd;

    // epoll_wait 返回的事件数组
    using EventList = std::vector<struct epoll_event, KzAlloc::KzAllocator<struct epoll_event>>;
    EventList _events;

    // 替代 std::map<int, Channel*>, 去除了大量复杂树操作，并有不错的 CPU Cache 效果
    // index 就是 fd。因为 fd 是内核分配的小整数，vector 扩容开销极低且缓存极其友好。
    ChannelList _channels;
};

inline void Poller::update(int operation, Channel* channel) noexcept {
    struct epoll_event event{};

    event.events = channel->events();
    // 将 Channel 指针存入 data
    event.data.ptr = channel;

    int fd = channel->fd();
    if (::epoll_ctl(_epollfd, operation, fd, &event) < 0) [[unlikely]] {
        if (operation == EPOLL_CTL_DEL) {
            // DEL 失败通常可以忽略 (比如 fd 已经被意外关闭)
        }
        else {
            LOG_SYSFATAL << "epoll_ctl op=" << operation << " fd=" << fd;
        }
    }
}

inline KzTimer::TimeStamp Poller::poll(int timeoutMs, ChannelList* activeChannels) noexcept {
    int numEvents = ::epoll_wait(
        _epollfd, _events.data(), static_cast<int>(_events.size()), timeoutMs);
    
    KzTimer::TimeStamp now = KzTimer::TimeStamp::now();
    
    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);

        // 如果活跃事件填满了 vector，说明负载很高，下次可能需要更多空间
        // 自动扩容 (x2)
        if (static_cast<size_t>(numEvents) == _events.size()) {
            _events.resize(_events.size() * 2);
        }
    }
    else if (numEvents < 0) [[unlikely]] {
        if (errno != EINTR) {
            LOG_SYSERR << "Poller::poll()";
        }
    }
    return now;
}


} // namesapce KzNet