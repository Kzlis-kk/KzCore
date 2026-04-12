#pragma once

#include <cstdint>
#include <type_traits>
#include <cassert>
#include <cstring> // for memcpy

// 平台兼容性处理：CLZ (Count Leading Zeros)
#if defined(_MSC_VER)
    #include <intrin.h>
    #pragma intrinsic(_BitScanReverse64)
#endif

namespace KzAlgorithm {
// 简单的Jeaiii实现
namespace Jeaiii {

// =============================================================================
// 基础设施：10的幂次表 & 编译器内建指令封装
// =============================================================================

// 用于位数计算的边界修正
// 必须覆盖 uint64_t 的最大范围 (1.84e19)
inline constexpr uint64_t g_powers_of_10[] = {
    0,
    10,
    100,
    1000,
    10000,
    100000,
    1000000,
    10000000,
    100000000,
    1000000000,
    10000000000,
    100000000000,
    1000000000000,
    10000000000000,
    100000000000000,
    1000000000000000,
    10000000000000000,
    100000000000000000,
    1000000000000000000,
    10000000000000000000
};

// 获取 64 位整数的前导零个数
// n 必须 > 0，否则行为未定义
inline uint32_t count_leading_zeros(uint64_t n) noexcept {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse64(&index, n);
    return 63 - index;
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_clzll(n));
#else
    // Fallback (极少走到，除非是很老的编译器)
    uint32_t bits = 0;
    while (n >>= 1) bits++;
    return 63 - bits;
#endif
}

// =============================================================================
// 核心算法 1：快速计算整数位数 (O(1))
// =============================================================================
// 原理：log10(n) = log2(n) * log10(2)
// log10(2) ≈ 0.30103
// 我们用定点数乘法模拟： bits * 1233 / 4096 ≈ bits * 0.301025
inline uint32_t count_digits(uint64_t n) noexcept {
    if (n == 0) return 1;

    // 1. 硬件加速求 log2(n)
    uint32_t bits = 64 - count_leading_zeros(n);

    // 2. 定点数乘法估算 log10(n)
    // 1233 / 4096 是对 log10(2) 的高效逼近
    uint32_t t = (bits * 1233) >> 12;

    // 3. 查表修正
    // 估算值 t 可能会比真实位数少 1，通过比较 powers_of_10[t] 来修正
    // 利用 bool 转 int (0 或 1) 来消除分支
    // 注意：g_powers_of_10[t] 实际上是 10^t
    // 如果 n < 10^t，说明 n 是 t 位数 (注意 t 是从 0 开始的 log10，位数是 t+1? 
    // 不，这里的逻辑是 t 已经很接近位数了，细节是 Jeaiii 调优过的魔法)
    // 简单理解：t 是一个保守估计的 log10 值。
    return t + (n >= g_powers_of_10[t]);
}

// =============================================================================
// 核心算法 2：生成两个字符 (0-99)
// =============================================================================
// 将 0-99 的整数转换为小端序的 2 字节 ASCII 码
// 例如：42 -> 0x3234 (内存中是 '4', '2')
// 关键优化：完全消除整数除法 (div) 指令
inline void write_pair(char* p, uint32_t n) noexcept {
    // 断言 n 在 0-99 之间
    assert(n < 100);

    // 1. 定点数计算 n / 10
    // 103 / 1024 ≈ 1 / 10 (0.10058...)
    // 对于 0-99 的数，结果是精确的
    uint32_t q = (n * 103) >> 10; 

    // 2. 计算 n % 10
    uint32_t r = n - q * 10; 

    // 3. 拼接 '0' + q 和 '0' + r
    // 0x3030 是两个 '0'
    // 小端序机器上，低字节在前。
    // 我们构造 (q + '0') | ((r + '0') << 8)
    uint16_t val = (0x3030 | (q) | (r << 8));

    // 4. SWAR (SIMD Within A Register) 写入
    // 使用 memcpy 防止对齐问题（现代编译器会优化为单条 mov word 指令）
    // 在 x86 上直接 *(uint16_t*)p = val 也是安全的且极快
    ::memcpy(p, &val, 2);
}

// =============================================================================
// 对外接口：整数转字符串
// =============================================================================

// 将无符号整数转换为字符串，返回指向字符串结尾的指针
// buffer 必须有足够空间 (最大 20 字节)
inline char* to_chars(char* buffer, uint64_t n) noexcept {
    if (n == 0) {
        *buffer++ = '0';
        return buffer;
    }

    // 1. 预先计算位数，避免 buffer 移动和反转
    uint32_t len = count_digits(n);
    char* pos = buffer + len; // pos 指向结束位置之后
    
    // 2. 倒序写入，每次处理 2 位
    while (n >= 100) {
        // 编译器会优化 /100 和 %100 为乘法+移位
        // 现代 CPU 做一次 /100 比做两次 /10 快得多
        uint32_t remainder = static_cast<uint32_t>(n % 100);
        n /= 100;
        
        pos -= 2;
        write_pair(pos, remainder);
    }

    // 3. 处理剩余的 < 100 的部分
    if (n < 10) {
        *--pos = static_cast<char>('0' + n);
    } else {
        pos -= 2;
        write_pair(pos, static_cast<uint32_t>(n));
    }

    return buffer + len;
}

// 有符号整数重载
inline char* to_chars(char* buffer, int64_t n) noexcept {
    if (n < 0) {
        *buffer++ = '-';
        // 处理 INT64_MIN 的特殊情况 (取反会溢出)
        // 但转为 uint64_t 后取反是定义良好的行为
        return to_chars(buffer, static_cast<uint64_t>(0 - static_cast<uint64_t>(n)));
    }
    return to_chars(buffer, static_cast<uint64_t>(n));
}

// 适配 int
inline char* to_chars(char* buffer, int n) noexcept {
    return to_chars(buffer, static_cast<int64_t>(n));
}

} // namespace Jeaiii
} // namespace KzAlgorithm