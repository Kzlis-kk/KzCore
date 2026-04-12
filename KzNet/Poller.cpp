#include "Poller.h"
#include "EventLoop.h"

namespace KzNet {

void Poller::updateChannel(Channel* channel) noexcept {
    // 必须在 loop 线程
    assert(_loop->isInLoopThread());
    
    const int index = channel->index();
    const int fd = channel->fd();

    // === 新增 (New) 或 已删除 (Deleted) ===
    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            // 扩容映射表 (如果 fd 超出当前范围)
            if (fd >= static_cast<int>(_channels.size())) [[unlikely]] {
                _channels.resize(fd + 16, nullptr);
            }
            // 注册映射
            assert(_channels[fd] == nullptr);
            _channels[fd] = channel;
        }
        else if (index == kDeleted) {
            assert(_channels[fd] == channel);
        }

        // 标记为已添加
        channel->setIndex(kAdded);

        // 执行 epoll_ctl ADD
        update(EPOLL_CTL_ADD, channel);
    }
    else if (index == kAdded) {
        assert(_channels[fd] == channel);
        assert(fd < static_cast<int>(_channels.size()));

        if (channel->isNoneEvent()) {
            // 优化：如果没有任何感兴趣的事件，从 epoll 中删除 (DEL)
            // 这样可以减少内核遍历红黑树的开销
            update(EPOLL_CTL_DEL, channel);
            channel->setIndex(kDeleted);
        }
        else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void Poller::removeChannel(Channel* channel) noexcept {
    // 必须在 loop 线程
    assert(_loop->isInLoopThread());

    int fd = channel->fd();
    assert(fd < static_cast<int>(_channels.size()));
    assert(_channels[fd] == channel);
    assert(channel->isNoneEvent());

    int index = channel->index();
    assert(index == kAdded || index == kDeleted);

    // 从 epoll 中删除
    if (index == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }

    // 标记为新
    channel->setIndex(kNew);

    // 从映射表中移除
    _channels[fd] = nullptr; 
}

bool Poller::hasChannel(Channel* channel) const noexcept {
    assert(_loop->isInLoopThread());
    
    int fd = channel->fd();
    if (fd >= static_cast<int>(_channels.size())) return false;
    return channel == _channels[fd];
}


} // namespace KzNet