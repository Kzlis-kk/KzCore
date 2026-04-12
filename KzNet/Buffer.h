#pragma once 

#include <cstring>
#include <unistd.h>
#include <cstdint>
#include <string_view>
#include <cassert>
#include "KzSTL/KzString.h"

namespace KzNet {

/**
 * @brief 连续缓冲区
 * 
 * * 内存布局：
 *   [ Readable (Content) ] [ Writable (Free) ]
 *   0                  readerIndex           writerIndex
 * 
 * * 核心特性：
 *   1. 连续内存：对解析极其友好，Cache Hit 率高。
 *   2. 零拷贝读取：readFd 利用 Thread Local Extrabuf + readv，减少系统调用。
 *   3. 占位符写入：支持 appendPlaceholder，避免 prepend 带来的 memmove 开销。
 */
class Buffer {
public:
    static constexpr size_t kInitialSize = 1024;
    static constexpr int kReadOnce = 1;        // 标准 LT
    static constexpr int kReadLoopHybrid = 16; // 混合 LT (推荐且默认)
    static constexpr int kReadUntilEAGAIN = -1;// 标准 ET (读干)

    explicit Buffer(size_t initialSize = kInitialSize) noexcept
    : _buffer(nullptr),
      _capacity(initialSize),
      _readerIndex(0),
      _writerIndex(0)
{
    _buffer = static_cast<char*>(KzAlloc::malloc(initialSize));
    if (!_buffer) [[unlikely]] KzAlloc::handleOOM();
}
    ~Buffer() {
        if (_buffer) [[likely]] {
            KzAlloc::free(_buffer, _capacity);
        }
    }

    // 不允许拷贝
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Move语义
    Buffer(Buffer&& other) noexcept
    : _buffer(other._buffer),
      _capacity(other._capacity),
      _readerIndex(other._readerIndex),
      _writerIndex(other._writerIndex)
{
    other._buffer = nullptr;
    other._capacity = 0;
    other._readerIndex = 0;
    other._writerIndex = 0;
}
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) [[likely]] {
            if (_buffer) {
                KzAlloc::free(_buffer, _capacity);
            }
            _buffer = other._buffer;
            _capacity = other._capacity;
            _readerIndex = other._readerIndex;
            _writerIndex = other._writerIndex;

            other._buffer = nullptr;
            other._capacity = 0;
            other._readerIndex = 0;
            other._writerIndex = 0;
        }
        return *this;
    }

    // 状态查询
    size_t readableBytes() const noexcept { return _writerIndex - _readerIndex; }
    size_t writableBytes() const noexcept { return _capacity - _writerIndex; }

    // 返回可读数据的起始地址
    const char* peek() const noexcept { return _buffer + _readerIndex; }
    // 返回可写空间的起始地址
    char* beginWrite() noexcept { return _buffer + _writerIndex; }
    const char* beginWrite() const noexcept { return _buffer + _writerIndex; }


    // 移动读指针
    void retrieve(size_t len) noexcept {
        assert(len <= readableBytes());
        if (len < readableBytes()) {
            _readerIndex += len;
        }
        else {
            retrieveAll();
        }
    }

    // 重置缓冲区 (读写指针归位)
    void retrieveAll() noexcept {
        _readerIndex = 0;
        _writerIndex = 0;
    }


    // 提交写入 (外界手动移动写指针)
    void hasWritten(size_t len) noexcept {
        assert(len <= writableBytes());
        _writerIndex += len;
    }

    // 类型适配写入
    void append(std::string_view sv) noexcept {
        if (writableBytes() >= sv.size()) [[likely]] {
            // 空间足够，直接拷贝
            ::memcpy(_buffer + _writerIndex, sv.data(), sv.size());
            _writerIndex += sv.size();
        }
        else {
            // 扩容或整理
            appendSlow(sv);
        }
    }

    /**
     * @brief 占位符写入 (Placeholder) 
     * * 场景：先占坑，写完 Body 后算出长度，再回头填坑。
     * * 优势：纯 append 操作，无 memmove，Cache 友好。
     * @return 指向占位内存的指针
     */
    char* appendPlaceholder(size_t len) noexcept {
        ensureWritableBytes(len);
        char* ptr = _buffer + _writerIndex;
        _writerIndex += len;
        return ptr;
    }

    // 强制都用小端
    static void pokeInt32(char* ptr, int32_t value) noexcept {
        ::memcpy(ptr, &value, sizeof(int32_t));
    }

    
    /**
     * @brief 从 fd 读取数据
     * @param maxLoopCount 
     *        > 0 : 最多循环 N 次 (LT / Hybrid LT)
     *        < 0 : 循环直到 EAGAIN (ET)
     * @return
     * > 0 : 读取的总字节数
     * 0   : 对端关闭 (EOF)
     * -1  : 错误 (检查 errno)
     */
    ssize_t readFd(int fd, int maxLoopCount = kReadLoopHybrid) noexcept;

    // 扩容逻辑
    void ensureWritableBytes(size_t len) noexcept {
        if (writableBytes() < len) {
            makeSpace(len);
        }
    }

    KzSTL::KzString retrieveAllAsKzString() noexcept {
        KzSTL::KzString str(peek(), readableBytes());
        retrieveAll();
        return str;
    }

private:
    char* _buffer;
    size_t _capacity;
    size_t _readerIndex;
    size_t _writerIndex;

    // 慢路径分离，利于指令缓存
    void appendSlow(std::string_view sv) noexcept {
        makeSpace(sv.size());
        ::memcpy(_buffer + _writerIndex, sv.data(), sv.size());
        _writerIndex += sv.size();
    }
    void makeSpace(size_t len) noexcept;

};

} // namespace KzNet