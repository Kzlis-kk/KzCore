#pragma once

#include "KzAlloc/KzAllocator.h"

#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <iosfwd>
#include <new>       
#include <type_traits> 

// 检查小端序 (Little Endian Only)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "KzString optimized layout requires Little Endian architecture"
#endif

namespace KzSTL {

/**
 * @brief 高性能字符串类
 * * 特性：
 * 1. SSO (Small String Optimization): <= 15 字节存储在栈上。
 * 2. 内存池集成: 使用 KzAlloc::realloc 进行高效原地扩容。
 * 3. 零拷贝兼容: 支持隐式转换为 std::string_view。
 * 4. 异常安全: 强异常保证。
 */
class KzString {
public:
    // 标准 STL 容器类型定义
    using value_type      = char;
    using pointer         = char*;
    using const_pointer   = const char*;
    using reference       = char&;
    using const_reference = const char&;
    using iterator        = char*;
    using const_iterator  = const char*;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;

    // npos 定义
    static constexpr size_t npos = static_cast<size_t>(-1);

    // === 构造函数 ===

    // 默认构造: 空字符串，SSO 模式
    explicit KzString() noexcept {
        // 全零初始化恰好对应：Short模式，长度为 0，以 \0 结尾
        // _meta = 0 (LSB=0 -> Short, Size=0)
        // _data[0] = 0
        ::memset(this, 0, sizeof(KzString));
    }

    // 从指针 + 长度构造
    KzString(const char* str, size_t len) noexcept {
        assert(len > 0);
        initFromBytes(std::string_view(str, len));
    }

    // 从 C 风格字符串构造
    explicit KzString(const char* str) noexcept {
        assert(str != nullptr);
        initFromBytes(std::string_view(str, ::strlen(str)));
    }

    // 构造：支持 const char*, std::string, std::string_view 等一切可转 sv 的类型
    explicit KzString(std::string_view sv) noexcept {
        initFromBytes(sv);
    }

    // 拷贝构造 (深拷贝)
    KzString(const KzString& other) {
        initFromBytes(std::string_view(other));
    }

    // 移动构造 (零拷贝，窃取资源)
    KzString(KzString&& other) noexcept {
        // 直接拷贝内存 (24字节)，完全窃取状态
        // 无论是 Long 还是 Short，这种 memcpy 都是正确的
        ::memcpy(&_r, &other._r, sizeof(_r));
    
        // 将对方置为 Short 空状态 (全0)
        ::memset(&other._r, 0, sizeof(other._r));
    }


    // === 接管接口 (Adoption) ===
    /**
     * @brief 接管构造函数 (Adoption Constructor)
     * 要求：
     * 1. ptr 必须是由 KzAlloc::malloc (或兼容的 malloc) 分配的堆内存。
     * 2. 调用者交出所有权，调用后不得再访问 ptr。
     * 3. cap 必须是 ptr 实际可用的容量。
     */
    // 1. 接管 KzAlloc 内存 (高性能)
    static KzString adopt(char* ptr, size_t len, size_t cap) noexcept {
        return KzString(ptr, len, cap, false); // false = not malloc
    }

    // 2. 接管 System Malloc 内存 (兼容 C 接口)
    // std::string 通常也是 malloc 的，如果能拿到它的指针所有权，也能接管
    // 但 std::string 很少暴露释放接口，所以通常只用于接管 C 风格指针 (strdup等)
    static KzString adoptMalloc(char* ptr, size_t len, size_t cap) noexcept {
        return KzString(ptr, len, cap, true); // true = is malloc
    }
   

    // === 析构函数 ===
    ~KzString() {
        if (isLong()) {
            if (isMallocPtr()) {
                // 如果标记了 Bit 1，说明是外来户，走标准 free
                ::free(getLongPtr());
            } else {
                // 否则是自家兄弟，走 mmap 内存池回收
                KzAlloc::free(getLongPtr(), getLongCap() + 1);
            }
        }
    }

    // === 赋值 ===
    void assign(std::string_view sv) noexcept {
        assign_raw(sv);
    }

    void assign(const char* str, size_t len) noexcept {
        assert(len > 0);
        assign_raw(std::string_view(str, len));
    }

    void assign(const char* str) noexcept {
        assert(str != nullptr);
        assign_raw(std::string_view(str, ::strlen(str)));
    }

    void assign(const KzString& other) noexcept {
        if (this != &other) [[likely]] { assign_raw(std::string_view(other)); }
    }

    void assign(KzString&& other) noexcept {
        if (this != &other) [[likely]] {
            // 1. 如果自己持有堆内存，先释放
            if (isLong()) {
                if (isMallocPtr()) {
                    ::free(getLongPtr());
                } 
                else {
                    KzAlloc::free(getLongPtr(), getLongCap() + 1);
                }
            }
            // 2. 窃取资源 (Bitwise Copy)
            std::memcpy(&_r, &other._r, sizeof(_r));
            // 3. 置空对方
            std::memset(&other._r, 0, sizeof(other._r));
        }
    }

    KzString& operator=(const KzString& other) noexcept {
        assign(other);
        return *this;
    }

    KzString& operator=(KzString&& other) noexcept {
        assign(std::move(other));
        return *this;
    }

    KzString& operator=(const char* str) noexcept {
        assign(str);
        return *this;
    }

    KzString& operator=(std::string_view sv) noexcept {
        assign(sv);
        return *this;
    }

    // === 迭代器支持 (零成本抽象) ===
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return begin() + size(); }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return begin() + size(); }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return cbegin() + size(); }
    
    // === 核心数据访问 ===
    const char* c_str() const noexcept { return isLong() ? getLongPtr() : getShortPtr(); }
    char* data() noexcept { return isLong() ? getLongPtr() : getShortPtr(); }
    const char* data() const noexcept { return c_str(); }
    size_t size() const noexcept { return isLong() ? getLongSize() : getShortSize(); }
    size_t length() const noexcept { return size(); }
    size_t capacity() const noexcept { return isLong() ? getLongCap() : kMaxShortSize; }
    bool empty() const noexcept { return size() == 0; }

    operator std::string_view() const noexcept {
        return std::string_view(data(), size());
    }

    char& operator[](size_t index) noexcept {
        assert(index <= size());
        return isLong() ? getLongPtr()[index] : getShortPtr()[index];
    }
    const char& operator[](size_t index) const {
        assert(index <= size());
        return data()[index];
    }

    // === 核心修改接口 ===

    void clear() noexcept {
        if (isLong()) {
            setLongSize(0);
            getLongPtr()[0] = '\0';
        } else {
            setShortSize(0);
            getShortPtr()[0] = '\0';
        }
    }

    // 追加操作
    void append(const char* str, size_t len) noexcept {
        assert(len > 0);
        append_raw(std::string_view(str, len));
    }
    
    // 针对空char*的兼容，如果上层保证不传入空指针const char* 也能使用下面的模版且不报错
    void append(const char* str) noexcept {
        assert(str != nullptr);
        append_raw(std::string_view(str, ::strlen(str)));
    }

    void append(std::string_view sv) noexcept {
        append_raw(sv);
    }

    KzString& operator+=(std::string_view sv) noexcept { 
        append(sv);
        return *this;
    }

    KzString& operator+=(const char* str) noexcept { 
        append(str); 
        return *this;
    }

    void push_back(char c) noexcept;
    KzString& operator+=(char c) noexcept {
        push_back(c);
        return *this;
    }

    KzString operator+(std::string_view sv) const noexcept { 
        KzString result;
        result.reserve(this->size() + sv.size()); // 一次性分配到位
        result.append_raw(*this);
        result.append_raw(sv);
        return result;
    }

    KzString operator+(const char* str) const noexcept { 
        KzString result; 
        size_t len = ::strlen(str);
        result.reserve(this->size() + len); // 一次性分配到位
        result.append_raw(*this);
        result.append_raw(std::string_view(str, len));
        return result;
    }

    KzString operator+(char c) const noexcept { 
        KzString result;
        result.reserve(this->size() + 1); // 一次性分配到位
        result.append_raw(*this);
        result.push_back(c);
        return result; 
    }
    
    // 预留空间
    void reserve(size_t newCap) noexcept;

    // 调整大小
    void resize(size_t n, char c = '\0') noexcept;

    // 交换
    void swap(KzString& other) noexcept {
        std::swap(_r._long.cap, other._r._long.cap);
        std::swap(_r._long.size, other._r._long.size);
        std::swap(_r._long.ptr, other._r._long.ptr);
    }

    // === 比较操作 ===
    // 利用 string_view 实现所有比较，减少代码重复
    int compare(std::string_view sv) const noexcept {
        return std::string_view(*this).compare(sv);
    }
    bool operator==(std::string_view sv) const noexcept { return compare(sv) == 0; }
    bool operator!=(std::string_view sv) const noexcept { return compare(sv) != 0; }
    bool operator<(std::string_view sv) const noexcept  { return compare(sv) < 0; }
    bool operator>(std::string_view sv) const noexcept  { return compare(sv) > 0; }

    bool operator==(const char* str) const noexcept { 
        assert(str != nullptr);
        return compare(std::string_view(str)) == 0;
    }
    bool operator!=(const char* str) const noexcept { 
        assert(str != nullptr);
        return compare(std::string_view(str)) != 0;
    }
    bool operator<(const char* str) const noexcept { 
        assert(str != nullptr);
        return compare(std::string_view(str)) < 0;
    }
    bool operator>(const char* str) const noexcept { 
        assert(str != nullptr);
        return compare(std::string_view(str)) > 0;
    }

private:

    // 掩码定义
    static constexpr unsigned char kLongFlag   = 1; // Bit 0: 1=Long, 0=Short
    static constexpr unsigned char kMallocFlag = 2; // Bit 1: 1=Malloc, 0=KzAlloc
    static constexpr unsigned char kCapMask    = ~(kLongFlag | kMallocFlag); // 用于提取真实 Capacity

    // === 成员变量 ===
    // === 核心内存布局 (24 字节) ===
    // 假设 Little Endian, 64-bit
    struct LongLayout {
        size_t cap;  // 最低位为 1 标记 Long 模式
        size_t size;
        char* ptr;
    };

    static constexpr size_t kMaxShortSize = 22; // 24 - 1(meta) - 1(\0) = 22

    struct ShortLayout {
        unsigned char meta; // 存储 (size << 1) 保证最低位为 0。最低位为 0 标记 Short 模式
        char data[23]; // 23 bytes
    };

    union Rep {
        LongLayout  _long;
        ShortLayout _short;
    } _r;

    // === 内部辅助函数 (Bit Twiddling) ===
    // 私有构造，仅供 adopt 使用
    KzString(char* ptr, size_t len, size_t cap, bool is_malloc) {
        // 同样先判断是否值得 SSO
        if (cap <= kMaxShortSize) {
            initFromBytes(std::string_view(ptr, len));
            // 谁申请谁释放
            if (is_malloc) ::free(ptr);
            else KzAlloc::free(ptr, cap + 1);
        } else {
            setLongPtr(ptr);
            setLongSize(len);
            // 设置 Capacity 并打上 Tag
            _r._long.cap = cap | static_cast<size_t>(kLongFlag) | (is_malloc ? static_cast<size_t>(kMallocFlag) : 0);
        }
    }

    bool isLong() const noexcept { return _r._short.meta & kLongFlag; }
    bool isMallocPtr() const noexcept {
        assert(isLong() && "isMallocPtr called in Short mode!");
        return _r._short.meta & kMallocFlag;
     }

    // Long Accessors
    
    size_t getLongCap() const noexcept { 
        // 右移 2 位，还原真实容量。不再需要掩码！
        return _r._long.cap >> 2; 
    }
    size_t getLongSize() const noexcept { return _r._long.size; }
    char* getLongPtr() const noexcept { return _r._long.ptr; }
    void setLongCap(size_t c) noexcept {
        // 左移 2 位腾出空间，c 可以是任何值（比如 31），不再要求 4 字节对齐
        // 64位系统下，最大容量从 16EB 变成了 4EB，完全够用。
        _r._long.cap = (c << 2) | static_cast<size_t>(kLongFlag);
    }
    // setLongCapWithFlag 用于保留之前的 flag (如果有必要)
    void setLongCapWithMalloc(size_t c, bool is_malloc) noexcept {
        _r._long.cap = (c << 2) | static_cast<size_t>(kLongFlag) | 
                   (is_malloc ? static_cast<size_t>(kMallocFlag) : 0);
    }
    void setLongSize(size_t s) noexcept { _r._long.size = s; }
    void setLongPtr(char* p) noexcept { _r._long.ptr = p; }

    // Short Accessors
    size_t getShortSize() const noexcept { return _r._short.meta >> 1; }
    const char* getShortPtr() const noexcept { return _r._short.data; }
    char* getShortPtr() noexcept { return _r._short.data; }
    void setShortSize(size_t s) noexcept { _r._short.meta = static_cast<unsigned char>(s << 1); }
    void resetToShort() noexcept { std::memset(&_r, 0, sizeof(_r)); }

    void initFromBytes(std::string_view sv) noexcept;
    void append_raw(std::string_view sv) noexcept;
    void assign_raw(std::string_view sv) noexcept;

};

// 流输出支持
inline std::ostream& operator<<(std::ostream& os, const KzString& str) noexcept {
    return os.write(str.data(), str.size());
}

// 全局 swap
inline void swap(KzString& lhs, KzString& rhs) noexcept {
    lhs.swap(rhs);
}

} // namespace KzSTL



namespace std {
    // 特化 std::hash，使其可以作为 unordered_map 的 Key
    template<> struct hash<KzSTL::KzString> {
        size_t operator()(const KzSTL::KzString& s) const noexcept {
            return hash<string_view>{}(s);
        }
    };
}
