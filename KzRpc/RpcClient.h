#pragma once

#include "RpcChannel.h"
#include "KzNet/EventLoopThread.h"
#include "KzNet/EventLoopThreadPool.h"
#include "KzAlloc/RAII_Utils.h"

#include <memory>
#include <thread>
#include <atomic>

namespace KzRpc {

/**
 * @brief RPC 客户端配置选项
 */
struct RpcClientOptions {
    double timeout = 5.0;       // 默认 RPC 超时时间
    // 待加入
};

/**
 * @brief 高性能 RPC 客户端
 * 
 * * 设计哲学：
 *   1. 极薄封装：所有接口均为内联，无虚函数，无运行时开销。
 *   2. 资源独占：默认拥有独立的 EventLoop 和 ThreadPool，保证隔离性。
 *   3. 内存对齐：内部核心组件全部使用 KzAlloc 对齐分配。
 * 
 */
class RpcClient {
public:
    /**
     * @brief 构造函数
     * @param serverAddr 服务器地址
     * @param options 配置选项
     */
    RpcClient(KzNet::EventLoop* loop, const KzNet::InetAddress& serverAddr, int poolsize = 8, int first_cpu = 0, const RpcClientOptions& options = RpcClientOptions())
        : _options(options),
          _main_loop(loop),
          _io_thread_pool(loop, poolsize)
    {
        // 在函数体中启动线程池
        _io_thread_pool.start(first_cpu + 1);
        // 必须先启动 IO 线程池才能构造 channel
        _channel = KzAlloc::allocate_unique<RpcChannel>(
            loop,
            serverAddr, 
            &_io_thread_pool, 
            poolsize
        );
    }

    ~RpcClient() {
        // 停止 IO 线程池 (TcpClient 会断开连接)
        // 注意析构顺序：Channel -> ThreadPool -> Loop，这是安全的
    }

    // 禁止拷贝和移动 (资源独占)
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    /**
     * @brief 发起 RPC 调用 (协程入口)
     * 
     * @tparam Req 请求类型
     * @tparam Resp 响应类型
     * @tparam UseVarint 是否使用 Varint 压缩 (默认 false)
     * 
     * @param service_id 服务 ID
     * @param method_id 方法 ID
     * @param req 请求对象
     * @return Awaitable 对象 (co_await 使用)
     */
    template <typename Req, typename Resp, bool UseVarint = false>
    auto call(uint16_t service_id, uint16_t method_id, Req&& req) {
        // 直接穿透调用 Channel，编译器会内联此函数
        return _channel->callMethod<Req, Resp, UseVarint>(
            service_id, method_id, std::move(req), _options.timeout
        );
    }
    template <typename Req, typename Resp, bool UseVarint = false>
    auto call(uint16_t service_id, uint16_t method_id, const Req& req) {
        // 直接穿透调用 Channel，编译器会内联此函数
        return _channel->callMethod<Req, Resp, UseVarint>(
            service_id, method_id, req, _options.timeout
        );
    }

private:
    RpcClientOptions _options;

    // 资源管理顺序至关重要：
    // Loop 必须最后析构 (因为它被 Channel 和 Pool 依赖)
    // 1. 主 Loop
    KzNet::EventLoop* _main_loop;
    // 2. IO 线程池
    KzNet::EventLoopThreadPool _io_thread_pool;
    // 3. RpcChannel
     using RpcChannelPtr = std::unique_ptr<RpcChannel,
        KzAlloc::AllocatorDeleter<RpcChannel, KzAlloc::KzAllocator<RpcChannel>>>;
    RpcChannelPtr _channel;
};

} // namespace KzRPC