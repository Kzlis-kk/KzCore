#include "Channel.h"
#include "EventLoop.h"

namespace KzNet {

void Channel::update() noexcept {
    _loop->updateChannel(this);
}

void Channel::remove() noexcept {
    assert(isNoneEvent());
    _loop->removeChannel(this);
}

} // namespace KzNet


