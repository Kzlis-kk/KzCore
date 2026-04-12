#include "KzString.h"
#include <iostream>
#include <algorithm>

namespace KzSTL {

static_assert(sizeof(KzString) == 24, "KzString size mismatch, expected 24 bytes");

// === 辅助初始化 ===
void KzString::initFromBytes(std::string_view sv) noexcept {
    if (sv.size() <= kMaxShortSize) {
        // Short 模式
        setShortSize(sv.size());
        std::memcpy(_r._short.data, sv.data(), sv.size());
        _r._short.data[sv.size()] = '\0';
        // 填充剩余字节为 0，保证 24 字节完全确定
        std::memset(_r._short.data + sv.size(), 0, kMaxShortSize - sv.size() + 1);
    } 
    else {
        // Long 模式
        char* ptr = static_cast<char*>(KzAlloc::malloc(sv.size() + 1));
        if (!ptr) [[unlikely]] KzAlloc::handleOOM();
        
        std::memcpy(ptr, sv.data(), sv.size());
        ptr[sv.size()] = '\0';

        setLongPtr(ptr);
        setLongSize(sv.size());
        setLongCap(sv.size());
    }
}


// === 内部核心：Assign ===
void KzString::assign_raw(std::string_view sv) noexcept {    
    // 如果当前容量足够，直接复用
    if (sv.size() <= capacity()) {
        // 使用 memmove 处理可能的自赋值 (Aliasing)
        // 虽然 operator= 检查了 this != &other，但 other 可能是 sub-string
        char* dest = const_cast<char*>(data());
        ::memmove(dest, sv.data(), sv.size());
        dest[sv.size()] = '\0';
        
        if (isLong()) setLongSize(sv.size());
        else setShortSize(sv.size());
    } else {
        // 容量不足：释放旧的，分配新的
        char* newPtr = static_cast<char*>(KzAlloc::malloc(sv.size() + 1));
        if (!newPtr) [[unlikely]] KzAlloc::handleOOM();
    
        // 拷贝数据
        ::memcpy(newPtr, sv.data(), sv.size());
        newPtr[sv.size()] = '\0';

        // 再释放旧内存
        if (isLong()) {
            if (isMallocPtr()) {
                ::free(getLongPtr());
            } else {
                KzAlloc::free(getLongPtr(), getLongCap() + 1);
            }
        }

        // 最后更新状态
        setLongPtr(newPtr);
        setLongSize(sv.size());
        setLongCap(sv.size());
    }
}

// === 核心逻辑：Reserve (利用 realloc) ===

void KzString::reserve(size_t newCap) noexcept {
    newCap = (newCap + 3) & ~size_t(3); // 强制 4 字节对齐
    size_t currentCap = capacity();
    if (newCap <= currentCap) [[unlikely]] return;

    if (isLong()) {
        if (isMallocPtr()) {
            // === Case: Malloc -> KzAlloc (同化) ===
            // 不能用 realloc，必须搬家
            char* newPtr = static_cast<char*>(KzAlloc::malloc(newCap + 1)); // 申请新家
            if (!newPtr) [[unlikely]] KzAlloc::handleOOM();
        
            std::memcpy(newPtr, getLongPtr(), size() + 1); // 搬运行李
            
            ::free(getLongPtr()); // 退掉旧房 (System Free)
            
            setLongPtr(newPtr);
            setLongCap(newCap); // 此时 Bit 1 自动变 0，变成 KzAlloc 内存
        } else {
            // === Case: KzAlloc -> KzAlloc (正常 Realloc) ===
            void* newPtr = KzAlloc::realloc(getLongPtr(), currentCap + 1, newCap + 1);
            if (!newPtr) KzAlloc::handleOOM();
            setLongPtr(static_cast<char*>(newPtr));
            setLongCap(newCap);
        }
    } else {
        // Short -> Long (Malloc)
        char* newPtr = static_cast<char*>(KzAlloc::malloc(newCap + 1));
        if (!newPtr) [[unlikely]] KzAlloc::handleOOM();
        
        std::memcpy(newPtr, getShortPtr(), getShortSize()); // Copy from stack
        newPtr[getShortSize()] = '\0';
        
        size_t oldSize = getShortSize();
        setLongPtr(newPtr);
        setLongSize(oldSize);
        setLongCap(newCap);
    }
}

// === 核心逻辑：Append ===

void KzString::append_raw(std::string_view sv) noexcept {
    size_t sz = size();
    size_t cap = capacity();
    size_t req = sz + sv.size();

    // Fast Path: 容量足够
    if (req <= cap) [[likely]] {
        char* dest = const_cast<char*>(data());
        // 向后添加的逻辑不可能重叠，使用memcpy即可
        ::memcpy(dest + sz, sv.data(), sv.size());
        dest[req] = '\0';
        
        if (isLong()) setLongSize(req);
        else setShortSize(req);
        return;
    }

    // Slow Path: 扩容
    // 检测 Aliasing: str 是否在当前 buffer 范围内
    const char* start = data();
    bool isAliasing = (sv.data() >= start && sv.data() < start + sz);
    ptrdiff_t offset = 0;
    if (isAliasing) [[unlikely]] {
        offset = sv.data() - start;
    }

    size_t newCap = std::max(cap << 1, req);
    newCap = (newCap + 3) & ~size_t(3); // 强制 4 字节对齐

    if (isLong()) {
        bool isSystemMalloc = isMallocPtr(); // 检查 Bit 1

        // 场景一：持有的是 System Malloc 指针 (必须迁移)
        // 场景二：发生自引用 (不能用 realloc，因为 realloc 可能 free 掉源数据)
        if (isSystemMalloc || isAliasing) [[unlikely]] {
            
            // 步骤 A: 向 KzAlloc 申请新内存
            char* newPtr = static_cast<char*>(KzAlloc::malloc(newCap + 1));
            if (!newPtr) [[unlikely]] KzAlloc::handleOOM();

            // 步骤 B: 拷贝旧数据
            std::memcpy(newPtr, start, sz);

            // 步骤 C: 拷贝追加数据 (处理 Aliasing)
            // 如果 Aliasing，从新内存的旧数据区读；或者直接用 offset 计算
            // 这里最简单的是：由于 start 还没释放，直接用 start + offset 依然有效
            if (isAliasing) {
                std::memcpy(newPtr + sz, start + offset, sv.size());
            } else {
                std::memcpy(newPtr + sz, sv.data(), sv.size());
            }
            newPtr[req] = '\0';

            // 步骤 D: 释放旧内存 (关键差异点)
            if (isSystemMalloc) {
                ::free(getLongPtr()); // 调用系统 free
            } else {
                KzAlloc::free(getLongPtr(), cap + 1); // 调用内存池 free
            }

            // 步骤 E: 更新状态 (注意：isMallocFlag 自动消失，变为 KzAlloc)
            setLongPtr(newPtr);
            
        } else {
            // 场景三：KzAlloc + 无自引用 (最快路径，直接 Realloc)
            // 注意：传入 realloc 的 old_size 是 cap + 1
            void* newPtr = KzAlloc::realloc(getLongPtr(), cap + 1, newCap + 1);
            if (!newPtr) [[unlikely]] KzAlloc::handleOOM();
            
            char* newChar = static_cast<char*>(newPtr);
            
            // 追加数据
            std::memcpy(newChar + sz, sv.data(), sv.size());
            newChar[req] = '\0';
            
            setLongPtr(newChar);
        }
    } else {
        // Short -> Long: 总是 Malloc
        char* newPtr = static_cast<char*>(KzAlloc::malloc(newCap + 1));
        if (!newPtr) [[unlikely]] KzAlloc::handleOOM();
        
        std::memcpy(newPtr, getShortPtr(), sz);
        // 如果 Aliasing，str 指向的是 stack buffer，malloc 后 stack 依然有效，直接读即可
        std::memcpy(newPtr + sz, sv.data(), sv.size()); 
        newPtr[req] = '\0';
        
        setLongPtr(newPtr);
    }
    
    setLongSize(req);
    setLongCap(newCap);
}

void KzString::push_back(char c) noexcept {
    size_t sz = size();
    size_t cap = capacity();
    if (sz < cap) [[likely]] {
        if (isLong()) {
            _r._long.ptr[sz] = c;
            _r._long.ptr[sz + 1] = '\0';
            setLongSize(sz + 1);
        } else {
            _r._short.data[sz] = c;
            _r._short.data[sz+1] = '\0';
            setShortSize(sz + 1);
        }
    } else {
        append_raw(std::string_view(&c, 1));
    }
}

void KzString::resize(size_t n, char c) noexcept {
    size_t currentSize = size();

    if (n < currentSize) {
        // === Case 1: 缩小 (Truncate) ===
        // 无论 Short 还是 Long，只需要改 Size 和加 \0
        // 不释放内存 (Shrink to fit 是 reserve 的事)
        if (isLong()) {
            setLongSize(n);
            getLongPtr()[n] = '\0';
        } else {
            setShortSize(n);
            getShortPtr()[n] = '\0';
        }
    } else if (n > currentSize) {
        // === Case 2: 扩大 (Expand) ===
        // 1. 确保容量足够 (这一步处理了所有的扩容/Short转Long逻辑)
        reserve(n);
        
        // 2. 填充新区域
        // reserve 后，data() 永远指向有效内存
        // 注意：memset 需要从 currentSize 开始填
        char* ptr = data();
        std::memset(ptr + currentSize, c, n - currentSize);
        ptr[n] = '\0';
        
        // 3. 更新 Size
        if (isLong()) {
            setLongSize(n);
        } else {
            // reserve 保证了如果 n > kMaxShortSize，这里已经是 Long 模式了
            // 所以这里只有当 n 还很小的时候才会走
            setShortSize(n);
        }
    }
}

} // namespace KzSTL