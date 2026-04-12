#pragma once

#include "KzNet/TcpServer.h"
#include "KzNet/EventLoop.h"
#include "KzThread/ThreadPool.h" // 包含 ResumeOnThreadPool
#include "KzSTL/Task.h"
#include "KzLog/Logger.h"
#include "KzAlloc/RAII_Utils.h"
#include "KzSTL/Expected.h"
#include "RpcProtocol.h"
#include "Serializer.h"

#include <array>
#include <vector>
#include <iostream>
#include <cstring>
#include <memory>

namespace KzRpc {

// 最大服务数量 (2^16)，支持 65536 个微服务
inline constexpr size_t MAX_SERVICE_COUNT = 65536;
// 通用的业务错误码类型
using RpcError = uint8_t;

// ============================================================================
// 1. 静态方法包装器 (MethodWrapper) - 协程版
// ============================================================================
/**
 * @brief 异步发送响应的辅助协程
 * @param is_varint 响应是否需要打上 Varint 标志 (继承自请求)
 */
inline KzSTL::Task<void> sendResponseAsync(std::shared_ptr<KzNet::TcpConnection> conn, 
                                              RpcHeader req_header, 
                                              uint8_t status,
                                              KzSTL::KzString resp_payload_with_header,
                                              bool is_varint)
{
    // 如果传入的是空字符串（例如错误响应），必须预留 Header 的空间
    if (resp_payload_with_header.size() < KZ_RPC_HEADER_SIZE) {
        resp_payload_with_header.resize(KZ_RPC_HEADER_SIZE, '\0');
    }
    RpcHeader resp_header{};
    std::memcpy(resp_header.magic, KZ_RPC_MAGIC, 4);
    resp_header.version = KZ_RPC_VERSION;
    
    // 组装 Flags：响应类型 + 继承请求的序列化/压缩方式
    uint8_t flags = 0;
    flags |= (static_cast<uint8_t>(MessageType::Response) << SHIFT_MSG_TYPE);
    flags |= (req_header.flags & MASK_SERIALIZATION_TYPE);
    flags |= (req_header.flags & MASK_COMPRESSION_TYPE);
    if (is_varint) flags |= MASK_VARINT;
    
    resp_header.flags = flags;
    resp_header.status = status;
    resp_header.request_id = req_header.request_id;
    resp_header.length = resp_payload_with_header.size();

    // 写入头部
    EncodeHeader(&resp_header, resp_payload_with_header.data());

    //LOG_INFO << "Server send header:" << KzSTL::KzString(resp_payload_with_header.data(), KZ_RPC_HEADER_SIZE);
    
    // 流控：异步发送。如果底层 Buffer 满，当前协程挂起，实现背压
    co_await conn->sendAsync(std::move(resp_payload_with_header));
}

/**
 * @brief 静态方法包装器 (类型擦除 + Out-Parameter 序列化)
 * 
 * @tparam UseVarint 编译期决定的编码策略
 * @tparam S         业务 Service 类型
 * @tparam Req       业务 Request 类型
 * @tparam Func      业务函数指针
 */
template <bool UseVarint, typename S, typename Req, 
          KzSTL::Expected<void, RpcError> (S::*Func)(const Req&, Serializer<UseVarint>&)>
KzSTL::Task<void> MethodWrapper(void* service, 
                                   std::string_view req_buf, 
                                   std::shared_ptr<KzNet::TcpConnection> conn,
                                   RpcHeader header) 
{
    S* s = static_cast<S*>(service);
    Req req{};

    // 1. 反序列化请求
    Deserializer<UseVarint> deserializer(req_buf);
    uint8_t out_status = static_cast<uint8_t>(StatusCode::OK);
    
    // 调用 operator>>，如果返回 false，说明数据损坏或不完整
    // 因为目前 C++ 不支持静态反射，这里的 >> 需要上层直接定义
    if (!(deserializer >> req)) [[unlikely]] {
        out_status = static_cast<uint8_t>(StatusCode::ERR_DESERIALIZE);
        co_await sendResponseAsync(conn, header, out_status, KzSTL::KzString(), UseVarint);
        co_return;
    }

    // 2. 准备序列化器 (Out-Parameter)
    Serializer<UseVarint> resp_serializer;

    // 3. 执行业务逻辑 (直接内联，零多态开销)
    auto result = (s->*Func)(req, resp_serializer);

    // 4. 处理业务结果
    if (result.has_value()) [[likely]] {
        // 业务成功，数据已经被业务函数写进 resp_serializer 了
        out_status = static_cast<uint8_t>(StatusCode::OK);
    } else {
        // 业务失败，清空可能写了一半的脏数据
        resp_serializer.clear();
        // 序列化错误码
        resp_serializer << result.error(); 
        out_status = static_cast<uint8_t>(StatusCode::ERR_BUSINESS);
    }

    // 5. 发送响应 (如果不是单向请求)
    if (!(header.flags & MASK_ONEWAY)) {
        co_await sendResponseAsync(conn, header, out_status, std::move(resp_serializer.str()), UseVarint);
    }
}

// ============================================================================
// 2. RpcServer 核心类
// ============================================================================

/**
 * @brief 高性能 RPC 服务器
 * 
 * * 线程模型 (Reactor + Thread Pool):
 *   1. IO 线程 (TcpServer): 负责连接接入、数据读取、协议头解析。
 *   2. Worker 线程 (ThreadPool): 负责反序列化、业务逻辑执行、序列化。
 * 
 */
class RpcServer {
private:
    // 类型擦除后的统一函数指针签名 (返回 Task<void>)
    using RpcHandlerFunc = KzSTL::Task<void> (*)(void* service, 
                                                    std::string_view req_buf, 
                                                    std::shared_ptr<KzNet::TcpConnection> conn,
                                                    RpcHeader header);
    // 内部数据结构
    struct MethodHandlers {
        RpcHandlerFunc fixed_handler = nullptr;
        RpcHandlerFunc varint_handler = nullptr;
    };

    struct ServiceEntry {
        void* service_instance = nullptr;         // 服务对象指针
        std::vector<MethodHandlers, KzAlloc::KzAllocator<MethodHandlers>> handlers;     // 方法分发表 (下标即 MethodID)
    };

    using ServicePtr = std::unique_ptr<std::array<ServiceEntry, MAX_SERVICE_COUNT>, 
                KzAlloc::AllocatorDeleter<std::array<ServiceEntry, MAX_SERVICE_COUNT>,
            KzAlloc::KzAllocator<std::array<ServiceEntry, MAX_SERVICE_COUNT>>>>;

    // 一级路由表: ServiceID -> ServiceEntry
    ServicePtr _services;

    KzNet::TcpServer _server;
    KzThread::ThreadPool _worker_pool;
public:


    /**
     * @brief 构造函数
     * @param loop 主 EventLoop (Acceptor 所属)
     * @param addr 监听地址
     */
    RpcServer(KzNet::EventLoop* loop, const KzNet::InetAddress& addr,
         int server_threadNum = 8, int worker_threadNum = 8,
         int server_first_cpu = 0, int worker_first_cpu = 0) noexcept
        : _server(loop, addr, server_threadNum, true, "RpcServer"sv),
          _worker_pool(worker_threadNum),
          _services(KzAlloc::allocate_unique<std::array<ServiceEntry, MAX_SERVICE_COUNT>>())
    {
        // 设置 IO 回调 (运行在 IO 线程)
       _server.setMessageCallback(
            [this](const std::shared_ptr<KzNet::TcpConnection>& conn, KzNet::Buffer* buf, KzTimer::TimeStamp time) mutable noexcept {
                this->onMessage(conn, buf, time);
            }
        );

        // 启动计算线程池
        _worker_pool.start(worker_first_cpu);
        _server.start(server_first_cpu);
    }

    ~RpcServer() {
        // 停止顺序：先停网络入口，再停业务处理
        // TcpServer 析构会自动停止 IO 线程
        _worker_pool.stop();
    }


    /**
     * @brief 注册 RPC 方法
     * 
     * @tparam UseVarint 业务方明确指定该方法处理的编码类型
     * @tparam S Service 类型
     * @tparam Req Request 类型
     * @tparam Func 成员函数指针
     * 
     * 注意：这里要求业务方明确指定这个方法是处理 Varint 还是 Fixed 的。
     * 如果业务方想同时支持两种，需要注册两次（指向不同的重载函数）.
     */
    template <bool UseVarint, typename S, typename Req,
              KzSTL::Expected<void, RpcError> (S::*Func)(const Req&, Serializer<UseVarint>&)>
    void registerMethod(uint16_t service_id, uint16_t method_id, S* service) {
        // 1. 获取或初始化 ServiceEntry
        ServiceEntry& entry = (*_services)[service_id];
        entry.service_instance = service;

        // 2. 确保 Method Vector 足够大
        if (method_id >= entry.handlers.size()) {
            entry.handlers.resize(method_id + 16);
        }

        // 3. 实例化模版 Wrapper 并存储函数指针
        // 编译器生成了特定的 MethodWrapper 代码，这里将指针存入数组
        // 将 Varint 和 Fixed 的 Handler 分开存储，实现双轨路由
        if constexpr (UseVarint) {
            entry.handlers[method_id].varint_handler = &MethodWrapper<true, S, Req, Func>;
        } else {
            entry.handlers[method_id].fixed_handler = &MethodWrapper<false, S, Req, Func>;
        }

        LOG_TRACE << "[RpcServer] Registered Service:" << service_id << " Method:" << method_id << " Varint:" << UseVarint;
    }

private:
    // 核心消息处理 (运行在 IO 线程)
    void onMessage(const std::shared_ptr<KzNet::TcpConnection>& conn,
                   KzNet::Buffer* buf,
                   KzTimer::TimeStamp time)
    {
        while (buf->readableBytes() >= KZ_RPC_HEADER_SIZE) {
            // 1. 预读 Header (零拷贝)

            // const RpcHeader* header = reinterpret_cast<const RpcHeader*>(buf->peek());
            // 轻量拷贝头部而不是直接使用指针，避免头部未对齐而导致在 ARM 架构处理器上出错，也就是 SIGBUS
            // 当然其实这里不拷贝后面切线程也要拷贝
            RpcHeader header;
            DecodeHeader(buf->peek(), &header);
            // LOG_INFO << "req_id::" << header.request_id;
            // LOG_INFO << "Server get header:" << KzSTL::KzString(buf->peek(), KZ_RPC_HEADER_SIZE);
            // 2. 快速校验 (Magic & Version)
            if (!IsHeaderValid(&header)) [[unlikely]] {
                // 严重错误：断开连接
                LOG_ERROR << "Invalid Magic/Version. Closing connection";
                conn->forceClose();
                return;
            }

            // 3. 检查包是否完整 (粘包处理)
            // header->length 包含了 Header 本身的大小
            if (buf->readableBytes() < header.length) {
                return; // 数据不够，等待下次
            }

            // 4. 提取 Payload
            // 此时包是完整的，直接获取 Payload 的 string_view，避免拷贝
            // buf->peek() + sizeof(RpcHeader) 是 Payload 的起始地址
            // Payload 长度 = total_len - sizeof(RpcHeader)
            const char* payload_ptr = buf->peek() + KZ_RPC_HEADER_SIZE;
            uint32_t payload_len = header.length - KZ_RPC_HEADER_SIZE;

            // 5. 路由分发
            uint16_t svc_id = header.method_id >> 16;
            uint16_t mtd_id = header.method_id & 0xFFFF;
            // 运行时动态分发：读取 Header 中的 Varint 标志位
            bool is_varint_req = (header.flags & MASK_VARINT) != 0;

            // 6. 查找服务
            ServiceEntry& entry = (*_services)[svc_id];
            RpcHandlerFunc handler = nullptr;
            void* service = nullptr;
            uint8_t status = static_cast<uint8_t>(StatusCode::OK);

            
            if (entry.service_instance) [[likely]] {
                if (mtd_id < entry.handlers.size()) [[likely]] {
                    // 根据请求的标志位，选择对应的 Handler
                    handler = is_varint_req ? entry.handlers[mtd_id].varint_handler 
                                            : entry.handlers[mtd_id].fixed_handler;
                    
                    if (handler) {
                        service = entry.service_instance;
                    } else {
                        // 客户端请求了 Varint，但服务端没注册 Varint 版本的处理函数
                        status = static_cast<uint8_t>(StatusCode::ERR_METHOD_NOT_FOUND);
                    }
                } else {
                    status = static_cast<uint8_t>(StatusCode::ERR_METHOD_NOT_FOUND);
                }
            } else {
                status = static_cast<uint8_t>(StatusCode::ERR_SERVICE_NOT_FOUND);
            }

            // 必须拷贝 Payload.因为 buf 是 IO 线程复用的，切到 Worker 线程后 buf 可能变了。
            // 如果 Payload 很大，可以考虑使用引用计数的 Buffer (如 std::shared_ptr<string>)
            KzSTL::KzString payload_copy(payload_ptr, payload_len);
            
            // 7. 任务投放到 workerpool
            if (handler && service) [[likely]] {
                // 启动一个调度协程
                auto dispatch_coro = dispatch_coro_func(&_worker_pool, handler, service, std::move(payload_copy), conn, header);
                dispatch_coro.start_detached();
            }
            else {
                // 找不到服务，直接在 IO 线程回包错误
                auto err_coro = err_coro_func(conn, header, status, is_varint_req);
                err_coro.start_detached();
            }
            // 8. 移动 Buffer 指针，处理下一个包
            buf->retrieve(header.length);
        }
    }

    static KzSTL::Task<void> dispatch_coro_func(KzThread::ThreadPool* pool, 
            RpcHandlerFunc h, 
            void* s, 
            KzSTL::KzString p, 
            std::shared_ptr<KzNet::TcpConnection> c, 
            RpcHeader hdr) 
    {
        co_await KzThread::ResumeOnThreadPool(pool);
        co_await h(s, std::string_view(p), c, hdr);
    }

    static KzSTL::Task<void> err_coro_func(std::shared_ptr<KzNet::TcpConnection> conn,
            RpcHeader header,
            uint8_t status,
            bool is_varint_req) 
    {
        co_await sendResponseAsync(conn, header, status, KzSTL::KzString(), is_varint_req);
    }

};

} // namespace KzRpc