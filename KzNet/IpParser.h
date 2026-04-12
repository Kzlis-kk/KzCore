#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <arpa/inet.h>

namespace KzNet {
namespace IpParser {

// === 内部辅助：Hex 解码表 ===
// 用于快速将 hex 字符转换为数值，非法字符为 -1
// 空间换时间，消除分支
// 索引: ASCII 字符值
// 值: 0-15 (有效数值) 或 -1 (非法字符)
static constexpr int8_t kHexTable[256] = {
    // 0-15 (Control chars)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 16-31 (Control chars)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 32-47 (Space, !, ", #, $, %, &, ', (, ), *, +, ,, -, ., /)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 48-63 ('0'-'9' at 48-57, then :, ;, <, =, >, ?)
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
    // 64-79 (@, 'A'-'F' at 65-70, then G-O)
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 80-95 (P-Z, [, \, ], ^, _)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 96-111 (`, 'a'-'f' at 97-102, then g-o)
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    // 112-127 (p-z, {, |, }, ~, DEL)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    
    // 128-255 (Extended ASCII / High bit set -> All Invalid)
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

// ============================================================================
// 核心复用逻辑：解析 IPv4 字符串 -> uint32_t (Host Byte Order)
// ============================================================================
// 成功返回 true，结果写入 out_ip
// 失败返回 false
inline bool parseIPv4Raw(const char* p, const char* end, uint32_t& out_ip) noexcept {
    uint32_t val = 0;
    uint32_t ip = 0;
    int dots = 0;
    bool has_digit = false;
    int digits_in_part = 0; // 记录当前段输入的数字个数

    while (p < end) {
        char c = *p++;
        if (c >= '0' && c <= '9') {
            // 【安全防御：防 SSRF 八进制绕过】
            // 如果当前段已经输入了数字，且当前 val 依然是 0，
            // 说明出现了前导零 (例如 "01", "00")，严格拒绝！
            // 注意：单独的 "0" 是允许的 (digits_in_part == 0 时 val 也是 0)
            if (digits_in_part > 0 && val == 0) [[unlikely]] {
                return false; 
            }

            val = val * 10 + (c - '0');
            if (val > 255) return false;
            has_digit = true;
            digits_in_part++;
        } else if (c == '.') {
            if (!has_digit || dots >= 3) return false;
            ip = (ip << 8) | val;
            val = 0;
            has_digit = false;
            digits_in_part = 0;
            dots++;
        } else {
            return false; // 非法字符
        }
    }
    if (dots != 3 || !has_digit) return false;
    
    ip = (ip << 8) | val;
    out_ip = ip;
    return true;
}

// ============================================================================
// 对外接口
// ============================================================================

// 标准 IPv4 解析
inline bool parseIPv4(std::string_view s, struct in_addr* dst) noexcept {
    uint32_t ip;
    if (parseIPv4Raw(s.data(), s.data() + s.size(), ip)) [[likely]] {
        dst->s_addr = ::htonl(ip); // 转为网络字节序
        return true;
    }
    return false;
}

// 内嵌 IPv4 解析 (用于 IPv6)
// 将解析结果写入到 IPv6 buffer 的最后 4 字节 (dst[12]..dst[15])
inline bool parseIPv4Embedded(const char* p, const char* end, unsigned char* tp) noexcept {
    uint32_t ip;
    if (parseIPv4Raw(p, end, ip)) [[likely]] {
        ip = ::htonl(ip);
        // 写入最后 4 字节
        // 编译器会优化为一次 32位 内存写入 (MOV)
        std::memcpy(tp, &ip, sizeof(ip));
        return true;
    }
    return false;
}

// 极速 IPv6 解析
// 支持 :: 压缩，支持内嵌 IPv4 (::ffff:192.168.1.1)
inline bool parseIPv6(std::string_view s, struct in6_addr* dst) noexcept {
    const char* p = s.data();
    const char* end = s.end();

    unsigned char* buf = dst->s6_addr;
    unsigned char* tp = buf;            // 当前写入位置
    unsigned char* endp = buf + 16;     //  16bytes
    unsigned char* colonp = nullptr;   // 记录"::"位置

    // 处理开头的 ::
    if (p < end && *p ==':') {
        if (p + 1 < end && *(p + 1) == ':') {
            p += 2;
            colonp = tp;
            // 只有"::“
        } 
        else {
            return false; // ‘:'非法
        }
    }

    while (p < end && tp < endp) {
        // 1. 处理连续的 ':' (即 :: 压缩)
        if (*p == ':') {
            if (colonp) return false;   // 只能有一个 ::
            colonp = tp;
            p++;
            if (p == end) break;        // 结尾的 ::，跳出循环进入展开逻辑
            if (*p == ':') return false; // ::: 非法
            continue;
        }

        // 2. 记录当前段的起始位置
        const char* group_start = p;
        uint32_t val = 0;
        bool seen_digit = false;

        // 3. 解析当前段 (先假设它是 IPv6 的 Hex Group)
        int char_count = 0;
        while (p < end) {
            int digit = kHexTable[static_cast<uint8_t>(*p)];
            if (digit >= 0) {
                if (++char_count > 4) return false; // 严格限制最多 4 个字符
                val = (val << 4) | digit;
                // 绝对不会超过 0xFFFF，所以不用溢出检查
                seen_digit = true;
                p++;
            } else {
                break;  // 遇到 ':' 或 '.' 或非法字符，跳出内层循环
            }
        }

        if (!seen_digit) return false;  // 空段

        // 4. 状态转移：检测是否突变为 IPv4 (The Magic Happens Here)
        if (p < end && *p == '.') {
            // 我们遇到了 '.'！说明 group_start 到 end 是一个 IPv4 地址。
            // 检查剩余空间是否够放 4 字节的 IPv4
            if (endp - tp < 4) return false;  
            
            // 将指针回溯到当前段的开头，交给 IPv4 解析器
            if (!parseIPv4Embedded(group_start, end, tp)) return false;
            
            tp += 4;
            p = end;   // IPv4 必须是结尾，解析彻底完成
            break;  
        }

        // 5. 正常的 IPv6 Hex Group，写入 2 字节 (网络字节序)
        *tp++ = static_cast<unsigned char>((val >> 8) & 0xFF);
        *tp++ = static_cast<unsigned char>(val & 0xFF);

        // 6. 处理段尾的 ':'
        if (p < end && *p == ':') {
            p++;
            if (p == end) return false;  // 结尾单个 : 非法 (如 2001:db8:)
        }
    }

    // 处理 :: 的展开
    if (colonp) {
        // 将 colonp 之后的数据移到 buffer 尾部
        // 示例: [a, b, c, d, 0, 0, 0, 0]  colonp指向b之后
        // 需要变成 [a, b, 0, 0, 0, 0, c, d]
        size_t n = tp - colonp;  // :: 之后的数据长度
        if (tp == endp) return false;  // :: 没起到压缩作用。或者数据满了
        
        // 移动数据
        ::memmove(endp - n, colonp, n);
        // 中间填 0
        ::memset(colonp, 0, endp - n - colonp);
        return true;
    }

    // 如果没有 ::，必须填满 16 字节
    if (tp != endp) return false;

    // 必须确保整个字符串被完全解析，没有多余的垃圾字符
    if (p != end) return false; 

    return true;
}


} // namespace IpParser
} // namespace KzNet