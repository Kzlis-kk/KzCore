#pragma once

#include "KzSTL/KzString.h"
#include <type_traits>
#include <cstring>
#include <string_view>
#include <vector>
#include <span>
#include <utility>
#include <iterator>

// 检查小端序 (Little Endian Only)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "KzRpc requires Little Endian architecture"
#endif

namespace KzRpc {

// 前向声明
template <bool UseVarint> class Deserializer;

/**
 * @brief 核心序列化类
 * @tparam UseVarint true=开启压缩(Base128+ZigZag), false=极致速度(定长+网络字节序)
 */
template <bool UseVarint = false>
class Serializer {
public:
    Serializer() noexcept {
        // 预留并填充 24 字节的 0，占住 Header 的位置
        _buffer.resize(KZ_RPC_HEADER_SIZE, '\0'); 
    }

    // === 获取结果 ===
    const KzSTL::KzString& str() const noexcept { return _buffer; }
    KzSTL::KzString& str() noexcept { return _buffer; }
    void clear() noexcept { _buffer.clear(); }


    // === 基础类型流式操作 ===
    // 1. Boolean (总是 1 字节)
    Serializer& operator<<(bool val) noexcept {
        _buffer.push_back(static_cast<char>(val));
        return *this;
    }

    // 2. 整型 (编译期分支)
    template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
    Serializer& operator<<(T val) noexcept {
        if constexpr (UseVarint) {
            write_varint(val);
        } else {
            write_fixed(val);
        }
        return *this;
    }

    // 3. 浮点型 (IEEE 754 标准，直接写入，不压缩)
    Serializer& operator<<(float val) noexcept {
        // 浮点数比较特殊，通常不建议做字节序转换，假设收发双方都是 IEEE 754
        // 如果要极其严格，需要 reinterpret_cast 为 uint32 后转字节序
        // 当然我们这里强制小端了
         _buffer.append(reinterpret_cast<const char*>(&val), sizeof(val));
        return *this;
    }
    Serializer& operator<<(double val) noexcept {
         _buffer.append(reinterpret_cast<const char*>(&val), sizeof(val));
        return *this;
    }

    // 4. 字符串标准入口：std::string_view
    Serializer& operator<<(std::string_view sv) noexcept {
        // 写入长度 (长度本身也遵循 UseVarint 策略)
        if constexpr (UseVarint) {
            write_varint(sv.size());
        } else {
            write_fixed(static_cast<uint32_t>(sv.size()));
        }
        // 写入内容
        _buffer.append(sv.data(), sv.size());
        return *this;
    }

    // 5. 安全重载：const char*
    // 必须存在，因为 string_view(nullptr) 是未定义行为
    Serializer& operator<<(const char* str) noexcept {
        if (str) [[likely]] {
            // 此时调用 string_view 版本，strlen 开销不可避免，但在构造 sv 时发生
            return (*this) << std::string_view(str);
        }
        return *this;
    }

    // === 容器操作 ===
    // 使用 span 语义清晰
    template <typename T>
    Serializer& operator<<(std::span<const T> items) noexcept {
        if (items.empty()) [[unlikely]] return *this;
        // 写入元素个数
        if constexpr (UseVarint) {
            write_varint(items.size());
        } else {
            write_fixed(static_cast<uint32_t>(items.size()));
        }
        
        // 编译期判断是否可以单次 memcpy 优化
        // 条件：定长编码 && 平凡可拷贝 && (单字节类型 OR 强制小端协议)，这里强制小端
        if constexpr (std::is_trivially_copyable_v<T> && !UseVarint) {
            // 警告：此路径假设收发双方字节序一致
            _buffer.append(reinterpret_cast<const char*>(items.data()), items.size_bytes());
        } else {
            // 通用路径：逐个元素递归序列化
            for (const auto& item : items) {
                *this << item;
            }
        }
        return *this;
    }

private:
    friend class Deserializer<UseVarint>;
    KzSTL::KzString _buffer;

    // === 内部实现：Varint (Base128 + ZigZag) ===
    template <typename T>
    requires std::is_integral_v<T>
    void write_varint(T value) noexcept {
        using UnsignedT = typename std::make_unsigned_t<T>;
        UnsignedT n;
        
        // ZigZag 编码：将有符号数映射为无符号数 (-1->1, 1->2)
        if constexpr (std::is_signed_v<T>) {
            n = (static_cast<UnsignedT>(value) << 1) ^ (value >> (sizeof(T) * 8 - 1));
        } else {
            n = value;
        }

        while (n >= 0x80) [[unlikely]] {
            _buffer.push_back(static_cast<char>((n & 0x7F) | 0x80));
            n >>= 7;
        }
        _buffer.push_back(static_cast<char>(n));
    }

    // === 内部实现：Fixed (Network Byte Order) ===
    template <typename T>
    requires std::is_integral_v<T>
    void write_fixed(T value) noexcept {
        // 仅小端互传
        if constexpr (sizeof(T) == 1) {
            _buffer.push_back(static_cast<char>(value));
        } else if constexpr (sizeof(T) == 2) {
            _buffer.append(reinterpret_cast<const char*>(&value), 2);
        } else if constexpr (sizeof(T) == 4) {
            _buffer.append(reinterpret_cast<const char*>(&value), 4);
        } else if constexpr (sizeof(T) == 8) {
            _buffer.append(reinterpret_cast<const char*>(&value), 8);
        }
    }
};



enum class Error : uint8_t {
    Ok = 0,
    EndOfBuffer,    // 尝试读取时，缓冲区数据不足
    VarintOverflow // Varint 解码出的值超出了目标类型的范围
};
/**
 * @brief 核心反序列化类
 * @tparam UseVarint 必须与 Serializer 保持一致
 * 注意：不支持链式调用，每次使用完必须检查返回值
 */
template <bool UseVarint = false>
class Deserializer {
public:
    explicit Deserializer(std::string_view buffer) noexcept
    : _view(buffer), _pos(0), _error(Error::Ok) {}

    // === 基础类型流式操作 ===
    // 1. Boolean
    [[nodiscard]] bool operator>>(bool& val) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;
        if (!check_bounds(1)) [[unlikely]] {
            _error = Error::EndOfBuffer;
            return false;
        }
        val = (_view[_pos++] != 0);
        return true;
    }

    // 2. 整型
    template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
    [[nodiscard]] bool operator>>(T& val) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;
        if constexpr (UseVarint) {
            return read_varint(val);
        } else {
            return read_fixed(val);
        }
    }

    // 3. 浮点型
    [[nodiscard]] bool operator>>(float& val) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;
        if (!check_bounds(sizeof(float))) [[unlikely]] {
            _error = Error::EndOfBuffer;
            return false;
        }
        std::memcpy(&val, _view.data() + _pos, sizeof(float));
        _pos += sizeof(float);
        return true;
    }

    [[nodiscard]] bool operator>>(double& val) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;
        if (!check_bounds(sizeof(double))) [[unlikely]] {
            _error = Error::EndOfBuffer;
            return false;
        }
        std::memcpy(&val, _view.data() + _pos, sizeof(double));
        _pos += sizeof(double);
        return true;
    }

    // 4. 字符串 (零拷贝到 string_view)
    // sv 的生命周期绝对不能超过传入 Deserializer 的那个 buffer
    [[nodiscard]] bool operator>>(std::string_view& sv) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;
        
        size_t len = 0;
        if (!read_size(len)) return false;

        if (!check_bounds(len)) {
            _error = Error::EndOfBuffer;
            return false;
        }
        sv = std::string_view(_view.data() + _pos, len);
        _pos += len;
        return true;
    }

    // 5. 字符串 (拷贝到 string/KzString)
    template <typename T>
    requires std::is_same_v<T, std::string> || std::is_same_v<T, KzSTL::KzString>
    [[nodiscard]] bool operator>>(T& str) noexcept {
        std::string_view sv;
        if (!(*this >> sv)) return false; // 复用上面的零拷贝逻辑
        str.assign(sv.data(), sv.size()); // 发生拷贝
        return true;
    }

    // 6. vector
    template <typename T>
    [[nodiscard]] bool operator>>(std::vector<T>& vec) noexcept {
        if (_error != Error::Ok) [[unlikely]] return false;

        size_t size = 0;
        if (!read_size(size)) return false;

        vec.clear();
        if constexpr (!UseVarint && std::is_trivially_copyable_v<T>) {
            // Fast Path: 极速单次拷贝
            size_t bytes_to_copy = size * sizeof(T);
            if (!check_bounds(bytes_to_copy)) {
                _error = Error::EndOfBuffer;
                return false;
            }
            vec.resize(size); // 分配内存
            std::memcpy(vec.data(), _view.data() + _pos, bytes_to_copy);
            _pos += bytes_to_copy;
        } else {
            // Slow Path: 逐个反序列化
            // 防御过量 size
            if (size > _view.size() - _pos) [[unlikely]] {
                _error = Error::EndOfBuffer;
                return false;
            }
            vec.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                T item;
                if (!(*this >> item)) { 
                    return false;
                }
                vec.push_back(std::move(item));
            }
        }
        return true;
    }

    // 获取错误状态
    Error error() const noexcept { return _error; }
    bool ok() const noexcept { return _error == Error::Ok; }
    explicit operator bool() const noexcept { return _error == Error::Ok; }

private:
    std::string_view _view;
    size_t _pos;
    Error _error;

    bool check_bounds(size_t len) const noexcept {
        return _pos + len <= _view.size();
    }

    // 读取长度 (分发 Varint/Fixed)
    bool read_size(size_t& len) noexcept {
        if constexpr (UseVarint) {
            uint64_t tmp;
            if (!read_varint(tmp)) return false;
            len = static_cast<size_t>(tmp);
        } else {
            uint32_t tmp;
            if (!read_fixed(tmp)) return false;
            len = static_cast<size_t>(tmp);
        }
        return true;
    }

    // === Read Varint ===
    template <typename T>
    bool read_varint(T& value) noexcept {
        using UnsignedT = typename std::make_unsigned_t<T>;
        UnsignedT result = 0;
        int shift = 0;
        while (true) {
            if (!check_bounds(1)) {
                _error = Error::EndOfBuffer;
                return false;
            }
            uint8_t byte = _view[_pos++];
            // 64位最多10个字节，最大shift为63。如果超过说明溢出
            if (shift >= sizeof(T) * 8) [[unlikely]] {
                _error = Error::VarintOverflow;
                return false;
            }
            result |= static_cast<UnsignedT>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        if constexpr (std::is_signed_v<T>) {
            // 标准 ZigZag 解码：(n >> 1) XOR -(n & 1)
            value = static_cast<T>((result >> 1) ^ (-(result & 1)));
        } else {
            value = static_cast<T>(result);
        }
        return true;
    }

    // === Read Fixed ===
    template <typename T>
    bool read_fixed(T& value) noexcept {
        if (!check_bounds(sizeof(T))) { _error = Error::EndOfBuffer; return false; }
        
        if constexpr (sizeof(T) == 1) {
            value = static_cast<T>(_view[_pos]);
        } else if constexpr (sizeof(T) == 2) {
             std::memcpy(&value, _view.data() + _pos, 2);
        } else if constexpr (sizeof(T) == 4) {
             std::memcpy(&value, _view.data() + _pos, 4);
        } else if constexpr (sizeof(T) == 8) {
             std::memcpy(&value, _view.data() + _pos, 8);
        }
        _pos += sizeof(T);
        return true;
    }

};

// 定义两个别名，方便用户使用
using SerializerFixed   = Serializer<false>;
using SerializerVarint  = Serializer<true>;
using DeserializerFixed = Deserializer<false>;
using DeserializerVarint= Deserializer<true>;

} // namespace KzRpc