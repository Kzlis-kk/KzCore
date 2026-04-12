#include "Buffer.h"
#include <sys/uio.h>
#include <errno.h>
#include "KzAlloc/ConcurrentAlloc.h"

namespace KzNet {

// 线程局部缓冲区：避免栈溢出，避免每次分配
// 64KB 足够覆盖大多数网络包
static thread_local char t_extrabuf[65536];

void Buffer::makeSpace(size_t len) noexcept {
    // 优先 realloc，其次 memmove
    if (writableBytes() + _readerIndex < len) {
        size_t readable = readableBytes();
        // 扩容
        size_t propSize = readable + len;
        // 配合 KzAlloc 的 SizeClass 进行对齐优化
        size_t newCapacity = _capacity * 2;
        if (newCapacity < propSize) newCapacity = propSize;

        char* newBuf = static_cast<char*>(KzAlloc::malloc(newCapacity));
        if (!newBuf) [[unlikely]] KzAlloc::handleOOM();

        // 只拷贝有效数据，且直接拷贝到头部
        std::memcpy(newBuf, _buffer + _readerIndex, readable);
        // 释放旧内存
        KzAlloc::free(_buffer, _capacity);

        _buffer = newBuf;
        _capacity = KzAlloc::RoundUp(newCapacity);
        _readerIndex = 0;          // 读指针归零
        _writerIndex = readable;   // 写指针紧跟有效数据
    }
    else {
        // 内部搬运
        size_t readable = readableBytes();
        ::memmove(_buffer, _buffer + _readerIndex, readable);
        _readerIndex = 0;
        _writerIndex = readable;
    }
}

ssize_t Buffer::readFd(int fd, int maxLoopCount) noexcept {
    ssize_t totalN = 0;
    int loop = 0;

    // 如果 maxLoopCount < 0，则条件永远为真 (直到内部 break)
    while (maxLoopCount < 0 || loop < maxLoopCount) {
        const size_t writable = writableBytes();

        struct iovec vec[2];
        vec[0].iov_base = _buffer + _writerIndex;
        vec[0].iov_len = writable;
        vec[1].iov_base = t_extrabuf;
        vec[1].iov_len = sizeof(t_extrabuf);

        // 当 writable 较大时，额外的 t_extrabuf 基本无法减少系统调用的次数
        // 并且双内存块在readv时会有跨内存块逻辑，虽然开销非常非常低，但是没必要的可以省略
        // 实际上，差别更大的是减少了 t_extrabuf 到 Buffer 的拷贝
        // 实际上，写死 2 也可以，反正对性能可以说完全没影响
        const int iovcnt = (writable < sizeof(t_extrabuf)) ? 2 : 1;

        const ssize_t n = ::readv(fd, vec, iovcnt);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 读空了，这是 ET 模式退出的唯一正常路径
                // 返回已读字节数 (可能为0)
                // 如果 totalN == 0，上层会看到 -1 且 errno == EAGAIN
                return (totalN > 0) ? totalN : -1;
            }
            if (errno == EINTR) continue;

            // 真正的错误
            return (totalN > 0) ? totalN : -1;
        }
        else if (n == 0) {
            // EOF
            return totalN;
        }
        else {
            totalN += n;
            if (static_cast<size_t>(n) <= writable) {
                _writerIndex += n;
            }
            else {
                _writerIndex = _capacity;
                append(std::string_view(t_extrabuf, n - writable));
            }

            // 如果没读满，说明内核空了，无论是 ET 还是 LT 都可以提前退出了
            // 这省去了最后一次必然返回 EAGAIN 的 syscall
            size_t maxLen = writable + (iovcnt == 2 ? sizeof(t_extrabuf) : 0);
            if (static_cast<size_t>(n) < maxLen) {
                break;
            }
        }
        loop++;
    }
    return totalN;
}

} // namespace KzNet