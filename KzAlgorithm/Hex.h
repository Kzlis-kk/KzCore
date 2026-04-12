#pragma once
#include <cstdint>
#include "Jeaiii.h"

namespace KzAlgorithm {
namespace Hex {
// 512 字节的查找表：索引 i 对应的就是 i 的十六进制字符串
// "00", "01", ... "FE", "FF"
// 包含了小写 hex，如果需要大写就把 a-f 改成 A-F
inline constexpr char g_hexDigits[] = 
    "000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f"
    "303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f"
    "505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f"
    "707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f"
    "909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
    "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
    "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

inline char* to_hex(char* buffer, uintptr_t p) noexcept {
    // 写入前缀 "0x"
    *buffer++ = '0';
    *buffer++ = 'x';

    if (p == 0) {
        *buffer++ = '0';
        return buffer;
    }

    // 计算有效位数
    // 指针通常是 64 位，但在用户态栈或堆上，高位通常有很多 0
    // 使用之前的 count_leading_zeros (在 Jeaiii.h 中定义的)
    // 比如 0x0000 7fff ... 前面有 17 个 0
    uint32_t zero_bits = Jeaiii::count_leading_zeros(p);
    
    // 向下取整到字节边界 (8 的倍数)，保证成对打印
    // 例如，如果有效位是 47 位，我们从第 48 位（第 6 个字节）开始打印
    uint32_t shift = 64 - zero_bits;
    // 调整为 8 的倍数，确保 shift >= 实际位数
    // (shift + 7) & ~7 是向上取整到 8 的倍数
    shift = (shift + 7) & ~7; 
    
    // 如果 shift > 64 (p=0的情况前面处理了)，这里安全
    
    // 循环处理每个字节
    while (shift > 0) {
        shift -= 8;
        // 取出当前最高有效字节
        uint8_t byte = (p >> shift) & 0xFF;
        
        // 查表写入 2 个字符
        // 现代编译器会将这个 memcpy 优化为一次 16-bit 写入 (mov word)
        ::memcpy(buffer, Hex::g_hexDigits + byte * 2, 2);
        buffer += 2;
    }

    return buffer;
}

} // namespace Hex
} // namespace KzAlgorithm