#pragma once

#include <cstdint>
#include <cstring>

namespace KzRpc {

/**
 * @brief KzRPC 协议核心定义
 * 该文件定义了 RPC 协议的头部结构、魔数、版本号以及所有相关的元数据。
 * 它是客户端与服务器之间通信的唯一契约。
 */

// ============================================================================
// 1. 核心协议常量
// ============================================================================

// 魔数: 用于快速识别和校验数据包，"KZRP" 的 ASCII 表示
inline constexpr char KZ_RPC_MAGIC[4] = {'K', 'Z', 'R', 'P'}; // 使用字符数组，免疫大小端问题
// 当前协议版本号
inline constexpr uint8_t KZ_RPC_VERSION = 1;

// 安全限制：最大 RPC 包大小 (例如 64MB)
inline constexpr uint32_t KZ_RPC_MAX_LENGTH = 64 * 1024 * 1024;
inline constexpr uint32_t KZ_RPC_HEADER_SIZE = 24;

// ============================================================================
// 2. 协议头结构体 (24 字节)
// ============================================================================

// 内存对齐，保证硬件支持
struct RpcHeader {
    // offset: 0  (8字节对齐)
    uint64_t request_id; // 请求唯一 ID, 用于异步请求的响应匹配

    /// offset: 8  (4字节对齐)
    char     magic[4];      // 魔数, 固定为 KZ_RPC_MAGIC

    // offset: 12 (4字节对齐)
    uint32_t length;     // 消息总长度 (RpcHeader + Payload)

    // offset: 16 (4字节对齐)
    uint32_t method_id;  // 高 16 位为 Service ID, 低 16 位为 Method ID

    // offset: 20 (1字节对齐)
    uint8_t  version;    // 协议版本号, 固定为 KZ_RPC_VERSION

    // offset: 21
    // 标志位 (flags), 包含了 5 种不同的元数据
    // -----------------------------------------------------------------
    // | Bit 7 | Bit 6   | Bit 5 | Bit 4   | Bit 3  | Bit 2  | Bit 1 | Bit 0   |
    // |  SerType (2)  |  ComprType (2)  | Varint | Oneway | MsgType (2) |
    // -----------------------------------------------------------------
    uint8_t  flags;

    // offset: 22
    uint8_t  status;     // 状态码, 仅在响应包(Response)中有意义

    // offset: 23
    uint8_t  reserved;   // 保留/对齐字段，为未来扩展预留
};

// 编译期断言，确保 Header 大小是设计的 24 字节
static_assert(sizeof(RpcHeader) == 24, "RpcHeader size must be 24 bytes");


// ============================================================================
// 3. Flags 字段的枚举和位掩码
// ============================================================================

// 消息类型 (Bits 0-1)
enum class MessageType : uint8_t {
    Request   = 0, // 业务请求
    Response  = 1, // 业务响应
    Heartbeat = 2, // 心跳包
    // 3 is reserved
};

// 单向请求标志 (Bit 2)
// 1 = Oneway (客户端发送后不关心响应)
// 0 = Two-way (需要服务器响应)

// Varint 编码标志 (Bit 3)
// 1 = Payload 中的整数使用 Varint 压缩
// 0 = Payload 中的整数使用定长编码

// 压缩类型 (Bits 4-5)
enum class CompressionType : uint8_t {
    None   = 0,
    Gzip   = 1,
    Snappy = 2,
    // 3 is reserved
};

// 序列化类型 (Bits 6-7)
enum class SerializationType : uint8_t {
    Custom = 0, // 自定义二进制格式
    Proto  = 1, // Protobuf
    Json   = 2, // JSON
    // 3 is reserved
};

// --- 用于操作 flags 字段的常量 ---
// 消息类型: Bits 0-1
inline constexpr uint8_t MASK_MSG_TYPE = 0x03; // 0b00000011
inline constexpr int SHIFT_MSG_TYPE = 0;

// 单向请求: Bit 2
inline constexpr uint8_t MASK_ONEWAY = 0x04;   // 0b00000100
inline constexpr int SHIFT_ONEWAY = 2;

// Varint 编码: Bit 3
inline constexpr uint8_t MASK_VARINT = 0x08;   // 0b00001000
inline constexpr int SHIFT_VARINT = 3;

// 压缩类型: Bits 4-5
inline constexpr uint8_t MASK_COMPRESSION_TYPE = 0x30; // 0b00110000
inline constexpr int SHIFT_COMPRESSION_TYPE = 4;

// 序列化类型: Bits 6-7
inline constexpr uint8_t MASK_SERIALIZATION_TYPE = 0xC0; // 0b11000000
inline constexpr int SHIFT_SERIALIZATION_TYPE = 6;


// ============================================================================
// 4. Status 字段的枚举
// ============================================================================

enum class StatusCode : uint8_t {
    OK = 0,                 // 成功
    // 框架级错误 (1-127)
    ERR_INVALID_REQUEST = 1,  // 无效请求
    ERR_SERVICE_NOT_FOUND = 2,  // 找不到服务
    ERR_METHOD_NOT_FOUND = 3, // 找不到方法
    ERR_DESERIALIZE = 4,      // Payload 反序列化失败
    ERR_TIMEOUT = 5,          // 请求超时
    ERR_NETWORK_CLOSED = 6,   // 断开连接
    ERR_UNKNOWN = 7,          // 未知错误
    // 业务级错误 (128-255)
    ERR_BUSINESS = 128       // 通用业务错误
};


// ============================================================================
// 5. 序列化与校验辅助函数
// ============================================================================

/**
 * @brief 将网络字节流转换为本地 RpcHeader (处理大小端)
 * @param buf 指向网络缓冲区的指针 (至少 24 字节)
 * @param header [输出] 解析后的本地 Header
 */
inline void DecodeHeader(const char* buf, RpcHeader* header) noexcept {
    // 内存拷贝，防止 buf 本身地址未对齐导致的 SIGBUS
    std::memcpy(header, buf, KZ_RPC_HEADER_SIZE);
}

/**
 * @brief 将本地 RpcHeader 转换为网络字节流 (处理大小端)
 */
inline void EncodeHeader(const RpcHeader* header, char* buf) noexcept {
   std::memcpy(buf, header, KZ_RPC_HEADER_SIZE);
}

/**
 * @brief 校验接收到的 RPC 头部是否合法
 * @param header 指向接收到的 RpcHeader 的指针
 * @return true 如果魔数和版本号都正确
 */
inline bool IsHeaderValid(const RpcHeader* header) noexcept {
    if (!header) [[unlikely]] return false;

    // 1. 校验 Magic
    if (::memcmp(header->magic, KZ_RPC_MAGIC, 4) != 0) [[unlikely]] return false;

    // 2. 校验版本
    if (header->version != KZ_RPC_VERSION) [[unlikely]] return false;

    // 3. 校验长度防线 (防 OOM 和 越界)
    if (header->length < KZ_RPC_HEADER_SIZE || header->length > KZ_RPC_MAX_LENGTH) [[unlikely]] {
        return false;
    }

    return true;
}


} // namespace KzRpc