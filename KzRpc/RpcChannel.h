#pragma once

#include "KzNet/TcpClient.h"
#include "KzNet/EventLoop.h"
#include "KzNet/EventLoopThreadPool.h"
#include "KzTimer/TimeStamp.h"
#include "KzAlloc/KzAllocator.h"
#include "KzSTL/Expected.h"
#include "KzSTL/SpinMutex.h"
#include "RpcProtocol.h"
#include "Serializer.h"

#include <mutex>
#include <coroutine>
#include <atomic>
#include <cstring>
#include <memory>
#include <array>
#include <new>

namespace KzRpc {
// 默认 RPC 超时时间 (5秒)
inline constexpr double kDefaultRpcTimeout = 5.0;

inline constexpr int kSlotExpandSize = 1024;

/**
 * @brief RPC 客户端通道
 * 
 * * 核心职责：
 *   1. 序列化请求并发送。
 *   2. 维护挂起的 RPC 调用 (Pending Calls)。
 *   3. 接收响应，匹配 Request ID，唤醒对应的协程。
 *   4. 处理超时和连接断开。
 * 
 * * 特性：
 *   1. 连接池：编译期确定大小，静态数组管理。
 *   2. 内存布局：连续内存分配，Cache Line 对齐，无伪共享。
 *   3. 协程支持：C++20 Awaitable 接口。
 *   4. 多路复用：基于 Slot Map 的 O(1) 请求匹配。
 * 
 */
class RpcChannel {
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLineSize = 64;
#endif
public:
    /**
     * @brief 构造函数
     * @param loop 主 EventLoop (用于定时器和 SlotMap 管理)
     * @param serverAddr 服务器地址
     * @param thread_pool IO 线程池 (用于负载均衡)
     */
    RpcChannel(KzNet::EventLoop* loop, const KzNet::InetAddress& serverAddr,
         KzNet::EventLoopThreadPool* thread_pool, int poolsize = 8) noexcept
        : _main_loop(loop), _poolsize(poolsize)
    {

        // 1. 计算内存大小与对齐
        // 确保每个 TcpClient 独占 Cache Line，防止 False Sharing
        size_t kAlignment = alignof(AlignedTcpClient);
        size_t total_size = sizeof(AlignedTcpClient) * _poolsize;

        // 2. 从内存池申请对齐内存
        _clients = static_cast<AlignedTcpClient*>(KzAlloc::malloc_aligned(total_size, kAlignment));
        if (!_clients) [[unlikely]] KzAlloc::handleOOM();

        // 3. 在连续内存上构造对象
        for (size_t i = 0; i < _poolsize; ++i) {
            // 负载均衡：将 Client 绑定到不同的 IO 线程
            KzNet::EventLoop* io_loop = thread_pool ? thread_pool->getNextLoop() : loop;
            char buf[32];
            int len = std::snprintf(buf, sizeof(buf), "RpcClient_%zu", i);

            // 指针算术：_clients + i 会自动按 sizeof(AlignedTcpClient) 偏移
            // 由于对齐，每个对象的地址都是 kAlignment (64) 的倍数
            KzNet::TcpClient* client = new (_clients + i) 
                AlignedTcpClient(io_loop, serverAddr, std::string_view(buf, len));

            // 绑定回调 (Lambda 捕获 this 是安全的，因为 RpcChannel 生命周期 > TcpClient)
            client->setConnectionCallback([this](const std::shared_ptr<KzNet::TcpConnection>& conn) mutable noexcept {
                this->onConnection(conn);
            });
            
            client->setMessageCallback(
                [this](const std::shared_ptr<KzNet::TcpConnection>& conn, KzNet::Buffer* buf, KzTimer::TimeStamp time) mutable noexcept {
                    this->onMessage(conn, buf, time);
                }
            );

            // client->enableRetry();
            client->connect();
        }

        // 预分配 SlotMap 空间client->enableRetry();
        _slots.reserve(4096);
    }

    ~RpcChannel() {
        // 1. 强制清理所有挂起的请求，防止协程泄漏
        forceFailAllPending(StatusCode::ERR_NETWORK_CLOSED);

        // 2. 手动析构对象 (逆序)
        for (size_t i = _poolsize; i > 0; --i) {
            _clients[i - 1].~AlignedTcpClient();
        }

        // 3. 归还内存给内存池
        // 必须传入与 malloc 时一致的 size 和 alignment
        if (_clients) {
            KzAlloc::free_aligned(_clients, sizeof(AlignedTcpClient) * _poolsize, alignof(AlignedTcpClient));
        }
    }

    // ========================================================================
    // 核心接口：发起 RPC 调用 (协程版)
    // ========================================================================
    /**
     * @brief 发起异步 RPC 调用
     * 
     * @return RpcCallAwaiter<Resp> 一个可 await 的对象
     * 
     * @tparam UseVarint 本次调用是否启用 Varint 压缩 (Per-Call Strategy)
     * 使用示例:
     * LoginResp resp = co_await channel.callMethod<LoginReq, LoginResp>(1, 1, req);
     */
    template <typename Req, typename Resp, bool UseVarint = false>
    auto callMethod(uint16_t service_id, uint16_t method_id, Req&& req, double timeout_sec = kDefaultRpcTimeout) {
        // 返回一个 Awaiter，当 co_await 时才会真正执行发送逻辑
        return RpcCallAwaiter<Req, Resp, UseVarint>(this, service_id, method_id, std::move(req), timeout_sec);
    }
    template <typename Req, typename Resp, bool UseVarint = false>
    auto callMethod(uint16_t service_id, uint16_t method_id, const Req& req, double timeout_sec = kDefaultRpcTimeout) {
        // 返回一个 Awaiter，当 co_await 时才会真正执行发送逻辑
        return RpcCallAwaiter<Req, Resp, UseVarint>(this, service_id, method_id, req, timeout_sec);
    }


private:
    // 挂起的请求上下文
    struct PendingCall {
        // 1. 协程句柄：用于唤醒调用者
        std::coroutine_handle<> handle;
        
        // 2. 结果输出指针：指向 Awaiter 中的 Expected<Resp, Error>
        // 使用 void* 进行类型擦除，因为 map 只能存通用类型
        void* output_ptr;

        // 3. 成功时调用：反序列化回调
        // 这是一个闭包，捕获了具体的 Resp 类型信息
        bool (*deserializer)(std::string_view, void*);

        // 4. 失败时调用：设置错误码
        void (*setErrorFunc)(void*, uint8_t);

        // 5. 定时器 ID：用于取消超时定时器
        KzTimer::TimerId timer_id;

        // 6. 所属连接：用于定向清除连接
        std::shared_ptr<KzNet::TcpConnection> conn;
    };

    // 存储节点
    struct Slot {
        PendingCall call;
        uint32_t version = 1; // 版本号，用于解决 ABA 问题
        int32_t next_free = -1; // 空闲链表指针 (-1 表示被占用)
    };
    

    // 负载均衡
    std::shared_ptr<KzNet::TcpConnection> pickConnection() noexcept {
        // 使用 thread_local 彻底消灭多核竞争
        static thread_local uint64_t tls_idx = 0;
        uint64_t idx = tls_idx++;

        for (size_t i = 0; i < _poolsize; ++i) {
            size_t real_idx;
            if ((_poolsize & (_poolsize - 1)) == 0) {
                real_idx = (idx + i) & (_poolsize - 1); // 2的幂，极速位运算
            } 
            else {
                real_idx = (idx + i) % _poolsize;       // 非 2的幂，普通取模
            }
            auto conn = _clients[real_idx].connection();
            if (conn && conn->connected()) [[likely]] {
                return conn;
            }
        }
        return nullptr;
    }

    // Slot Map 核心逻辑 (O(1) 操作)
    // 分配 Request ID
    uint64_t allocateSlot(const PendingCall& call) noexcept {
        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
        
        if (_free_head == -1) [[unlikely]] {
            uint32_t old_size = static_cast<uint32_t>(_slots.size());
            
            // 一次性分配 kSlotExpandSize 个槽位
            // std::vector::resize 会调用 Slot 的默认构造函数
            // 此时：version = 1, next_free = -1
            _slots.resize(old_size + kSlotExpandSize);

            // 将新分配的槽位串联到空闲链表中
            // 只需要处理 [old_size, end - 1) 的范围
            // 最后一个元素的 next_free 默认为 -1，正好作为链表尾
            for (uint32_t i = old_size; i < _slots.size() - 1; ++i) {
                _slots[i].next_free = static_cast<int32_t>(i + 1);
            }

            // 将空闲链表头指向这一批新槽位的第一个
            _free_head = static_cast<int32_t>(old_size);
        }

        uint32_t index = static_cast<uint32_t>(_free_head);
        Slot& slot = _slots[index];
        _free_head = slot.next_free;
        slot.call = call;
        slot.next_free = -1; // 标记占用

        // 组装 ID: Version (32) | Index (32)
        return (static_cast<uint64_t>(slot.version) << 32) | index;
    }

    // 查找并移除 Slot (返回 PendingCall 以便在锁外执行)
    bool findAndRemoveSlot(uint64_t req_id, PendingCall& out_call) noexcept {
        uint32_t index = static_cast<uint32_t>(req_id & 0xFFFFFFFF);
        uint32_t ver = static_cast<uint32_t>(req_id >> 32);

        std::lock_guard<KzSTL::SpinMutex> lock(_mutex);

        // LOG_INFO << "inter1";
        // LOG_INFO << "index:" << index << "size:" << _slots.size();
        // 1. 边界检查
        if (index >= _slots.size()) [[unlikely]] return false;
        // LOG_INFO << "inter2";

        Slot& slot = _slots[index];
        

        // 2. 版本检查 (关键：防止处理过期包)
        // 如果 slot 空闲 (next_free != -1) 或者版本不匹配，说明 ID 无效
        if (slot.next_free != -1 || slot.version != ver) {
            return false;
        }

        // LOG_INFO << "inter3";

        // 3. 提取数据
        out_call = slot.call;

        // 4. 回收槽位
        slot.version++; // 版本自增，废止旧 ID
        slot.next_free = _free_head; // 头插法归还
        _free_head = static_cast<int32_t>(index);

        return true;
    }


    // Awaiter 实现
    template <typename Req, typename Resp, bool UseVarint>
    struct RpcCallAwaiter {
        RpcChannel* channel;
        uint16_t service_id;
        uint16_t method_id;
        Req req;
        double timeout;
        
        // 存放结果的地方 (协程栈变量)
        KzSTL::Expected<Resp, uint8_t> result;

        RpcCallAwaiter(RpcChannel* c, uint16_t s, uint16_t m, Req&& r, double t)
            : channel(c), service_id(s), method_id(m), req(std::move(r)), timeout(t), 
              result(KzSTL::make_unexpected<uint8_t>(static_cast<uint8_t>(StatusCode::ERR_UNKNOWN))) {}

        RpcCallAwaiter(RpcChannel* c, uint16_t s, uint16_t m, const Req& r, double t)
            : channel(c), service_id(s), method_id(m), req(r), timeout(t), 
              result(KzSTL::make_unexpected<uint8_t>(static_cast<uint8_t>(StatusCode::ERR_UNKNOWN))) {}

        // 1. 总是挂起，等待网络 IO
        bool await_ready() const noexcept { return false; }

        // 2. 挂起时执行：序列化 -> 注册 Pending -> 发送
        // 返回 bool: true 表示挂起成功，false 表示立即恢复 (发送失败)
        bool await_suspend(std::coroutine_handle<> h) {
            // 1. 负载均衡选择连接
            auto conn = channel->pickConnection();
            if (!conn) [[unlikely]] {
                result = KzSTL::make_unexpected<uint8_t>(static_cast<uint8_t>(StatusCode::ERR_NETWORK_CLOSED));
                return false; // 不挂起，直接 resume 抛出错误
            }


            // 2. 准备 PendingCall
            PendingCall call{};
            call.handle = h;
            call.conn = conn;
            call.output_ptr = &result; // 指向当前 Awaiter 的 result 成员
            // 设置反序列化适配器 (Type Erasure Magic)
            call.deserializer = [](std::string_view payload, void* out) -> bool {
                auto* typed_out = static_cast<KzSTL::Expected<Resp, uint8_t>*>(out);
                
                Deserializer<UseVarint> deserializer(payload);
                Resp resp_obj{};
                if (deserializer >> resp_obj) {
                    // 成功：构造 Success Expected (Placement New 在 Expected 内部处理)
                    *typed_out = KzSTL::Expected<Resp, uint8_t>(std::move(resp_obj));
                    return true;
                } else {
                    // 失败：构造 Error Expected
                    *typed_out = KzSTL::make_unexpected<uint8_t>(static_cast<uint8_t>(StatusCode::ERR_DESERIALIZE));
                    return false;
                }
            };
            // 设置错误回调
            call.setErrorFunc = [](void* out, uint8_t code) {
                auto* typed_out = static_cast<KzSTL::Expected<Resp, uint8_t>*>(out);
                *typed_out = KzSTL::make_unexpected<uint8_t>(code);
            };

            // 3. 分配 Slot (第一次加锁)
            uint64_t req_id = channel->allocateSlot(call);
            
            /*
            // 4. 启动定时器 (锁外操作，避免系统调用阻塞 SpinMutex)
            // 注意：必须捕获 channel 指针和 req_id
            // 实际上，timer_id 仅用于取消。如果 timer 触发了，它会拿着 req_id 去查找。
            // 只要 req_id 是对的，timer_id 晚一点设置没关系。
            auto timer_coro =[](RpcChannel* c, uint64_t id, double t) -> KzSTL::Task<void> {
                // 计算超时时间
                auto when = KzTimer::TimeStamp::now().addNano(t * KzTimer::TimeStamp::kNanoSecondsPerSecond);
                
                // 异步等待定时器添加完成，并获取 TimerId
                // 传入 nullptr 表示不需要切换到计算线程池，直接在 EventLoop 线程恢复即可
                KzTimer::TimerId tid = co_await c->_main_loop->addTimerAsync(
                    nullptr,[c, id]() mutable noexcept {
                        c->handleTimeout(id);
                    }, 
                    when
                );

                // 此时已经安全地在 EventLoop 线程中恢复，进行 TimerID 回填
                // 第二次加锁：回填 TimerID 并校验版本 (Fix Race Condition)
                // 必须校验版本，防止在定时器启动间隙，请求已经完成且 Slot 被复用
                std::lock_guard<KzSTL::SpinMutex> lock(c->_mutex);
                uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFF);
                uint32_t ver   = static_cast<uint32_t>(id >> 32);
                
                // 严谨的检查：
                // 1. 索引是否越界 (理论上不会，但为了安全)
                // 2. 版本是否匹配 (防止 Slot 被复用)
                // 3. Slot 是否被占用 (防止 Slot 刚被释放但未被复用)
                if (index < c->_slots.size() && 
                    c->_slots[index].version == ver && 
                    c->_slots[index].next_free == -1) {
                    c->_slots[index].call.timer_id = tid;
                } else {
                    // 竞态发生：响应在定时器启动前就已经回来了，并且 Slot 可能已经被回收。
                    // 此时这个定时器已经没有意义了，必须立即取消，否则超时触发时会找不到 Slot (虽然无害，但浪费资源)
                    c->_main_loop->cancel(tid);
                }
            }(channel, req_id, timeout);

            // 点火并分离定时器协程
            timer_coro.start();
            timer_coro.detach();
            */
            
            // 5. 序列化与发送
            // 序列化 Request
            Serializer<UseVarint> serializer;
            serializer << req;

            // 构造 Header
            RpcHeader header{};
            std::memcpy(header.magic, KZ_RPC_MAGIC, 4);
            header.version = KZ_RPC_VERSION;
            header.flags = (static_cast<uint8_t>(MessageType::Request) << SHIFT_MSG_TYPE);
            if constexpr (UseVarint) header.flags |= MASK_VARINT;
            
            header.request_id = req_id;
            header.method_id = (static_cast<uint32_t>(service_id) << 16) | method_id;
            header.length = serializer.str().size();

            // 发送数据 (TcpConnection 是线程安全的)
            EncodeHeader(&header, serializer.str().data());
            // LOG_INFO << "Client send header:" << KzSTL::KzString(serializer.str().data(), KZ_RPC_HEADER_SIZE);
            // LOG_INFO << "req_id::" << header.request_id;
            // 启动一个独立的协程来发送数据，不阻塞当前的 await_suspend
            // conn->send(std::move(serializer.str()));
            auto send_coro = RpcChannel::send_coro_func(conn, std::move(serializer.str()));
            send_coro.start_detached();

            return true; // 成功挂起
        }

        // 3. 恢复时执行：返回结果
        KzSTL::Expected<Resp, uint8_t> await_resume() {
            return result; // RVO 优化
        }
    };

    static KzSTL::Task<void> send_coro_func(std::shared_ptr<KzNet::TcpConnection> c, KzSTL::KzString data) {
        co_await c->sendAsync(std::move(data));
    }
    // 网络事件处理
    void onConnection(const std::shared_ptr<KzNet::TcpConnection>& conn) {
        if (!conn->connected()) {
            // 连接断开：清理所有挂起的请求
            failPendingOnConnection(conn, StatusCode::ERR_NETWORK_CLOSED);
        }
    }

    void handleTimeout(uint64_t req_id) {
        PendingCall call;
        // 尝试移除 Slot。如果已经被 onMessage 移除了，这里会返回 false
        if (findAndRemoveSlot(req_id, call)) {
            call.setErrorFunc(call.output_ptr, static_cast<uint8_t>(StatusCode::ERR_TIMEOUT));
            if (call.handle) call.handle.resume();
        }
    }

    void forceFailAllPending(StatusCode code) {
        std::vector<PendingCall> to_fail;
        {
            std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
            // 遍历所有 Slot，回收被占用的
            for (size_t i = 0; i < _slots.size(); ++i) {
                if (_slots[i].next_free == -1) { // 被占用
                    to_fail.push_back(_slots[i].call);
                    
                    // 重置 Slot
                    _slots[i].version++;
                    _slots[i].next_free = _free_head;
                    _free_head = static_cast<int32_t>(i);
                }
            }
        }

        // 锁外唤醒，避免死锁
        for (auto& call : to_fail) {
            _main_loop->cancel(call.timer_id);
            if (call.setErrorFunc) call.setErrorFunc(call.output_ptr, static_cast<uint8_t>(code));
            if (call.handle) call.handle.resume();
        }
    }

    void failPendingOnConnection(const std::shared_ptr<KzNet::TcpConnection>& broken_conn, StatusCode code) {
        std::vector<PendingCall> to_fail;
        {
            std::lock_guard<KzSTL::SpinMutex> lock(_mutex);
            for (size_t i = 0; i < _slots.size(); ++i) {
                if (_slots[i].next_free == -1 && _slots[i].call.conn == broken_conn) { // 只杀对应的连接
                    to_fail.push_back(_slots[i].call);
                    _slots[i].version++;
                    _slots[i].next_free = _free_head;
                    _free_head = static_cast<int32_t>(i);
                }
            }
        }
        for (auto& call : to_fail) {
            _main_loop->cancel(call.timer_id);
            if (call.setErrorFunc) call.setErrorFunc(call.output_ptr, static_cast<uint8_t>(code));
            if (call.handle) call.handle.resume();
        }
    }

    void onMessage(const std::shared_ptr<KzNet::TcpConnection>& conn, KzNet::Buffer* buf, KzTimer::TimeStamp time) {
        while (buf->readableBytes() >= KZ_RPC_HEADER_SIZE) {
            // 使用 DecodeHeader 安全解析
            RpcHeader header;
            DecodeHeader(buf->peek(), &header);

            // LOG_INFO << "req_id2:" << header.request_id;
            // LOG_INFO << "Client get header:" << KzSTL::KzString(buf->peek(), KZ_RPC_HEADER_SIZE);

            // 1. 校验 Magic
            if (!IsHeaderValid(&header)) [[unlikely]] {
                conn->forceClose(); // 协议错误，直接断开
                return;
            }

            // 2. 校验长度
            if (buf->readableBytes() < header.length) return; // 粘包，等待

            // 3. 提取 Payload
            const char* payload_ptr = buf->peek() + KZ_RPC_HEADER_SIZE;
            uint32_t payload_len = header.length - KZ_RPC_HEADER_SIZE;
            std::string_view resp_payload(payload_ptr, payload_len);

            // 4. 查找 PendingCall
            PendingCall call;
            if (findAndRemoveSlot(header.request_id, call)) {
                // 取消超时定时器
                // _main_loop->cancel(call.timer_id);

                // 6. 处理结果 (锁外执行，安全)
                if (header.status == static_cast<uint8_t>(StatusCode::OK)) {
                    call.deserializer(resp_payload, call.output_ptr);
                } else {
                    // 服务器返回错误
                    call.setErrorFunc(call.output_ptr, header.status);
                }

                // 6. 唤醒协程
                if (call.handle) call.handle.resume();
            }

            // 7. 移动 buffer
            buf->retrieve(header.length);
        }
    }


private:
    struct alignas(kCacheLineSize) AlignedTcpClient : public KzNet::TcpClient {
        // 继承构造函数
        using KzNet::TcpClient::TcpClient;
    };
    KzNet::EventLoop* _main_loop;
    int32_t _free_head = -1;
    int _poolsize;
    mutable KzSTL::SpinMutex _mutex; // 自旋锁保护 _slots 和 _free_head
    std::vector<Slot, KzAlloc::KzAllocator<Slot>> _slots;
    // 这里存裸指针，生命周期由 RpcChannel 严格管理
    AlignedTcpClient* _clients = nullptr;
};

} // namespace KzRpc