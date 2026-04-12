#include "Socket.h"

using namespace std::string_view_literals;
namespace KzNet {

bool Socket::getTcpInfoString(char* buf, size_t len) const noexcept {
    assert(len > 0 && buf != nullptr);
    struct tcp_info tcpi{};
    if (!getTcpInfo(&tcpi)) [[unlikely]] {
        return false;
    }

    char* p = buf;
    char* end = buf + len;

    // 辅助 Lambda：拼接 "key=value "
    // 强制内联以减少调用开销
    auto append = [&](std::string_view key, uint64_t val) {
        // 边界检查：keyLen + 最大整数长度(20) + 空格(1) + 结束符(1)
        if (p + key.size() + 22 > end) [[unlikely]] {
            return; // 空间不足，截断
        }
        
        // 拷贝 Key
        std::memcpy(p, key.data(), key.size());
        p += key.size();
        
        // 转换 Value (Jeaiii)
        p = KzAlgorithm::Jeaiii::to_chars(p, val);
        
        // 添加分隔符
        *p++ = ' ';
    };
    // === 核心指标 ===
    // 连接状态 (1: ESTABLISHED, ...)
    append("state="sv, tcpi.tcpi_state);
    
    // 拥塞控制状态 (0: Open, 1: Disorder, 2: CWR, 3: Recovery, 4: Loss)
    append("ca_state="sv, tcpi.tcpi_ca_state);
    
    // 当前重传队列长度 (未被确认的包)
    append("retrans="sv, tcpi.tcpi_retransmits);
    
    // KeepAlive 探测次数
    append("probes="sv, tcpi.tcpi_probes);
    
    // 退避指数
    append("backoff="sv, tcpi.tcpi_backoff);
    
    // 启用选项 (SACK, Timestamps, ECN 等位掩码)
    append("options="sv, tcpi.tcpi_options);

    // === 拥塞控制与延迟 (微秒) ===
    
    // 平滑 RTT (Round Trip Time)
    append("rtt="sv, tcpi.tcpi_rtt);
    
    // RTT 波动值
    append("rttvar="sv, tcpi.tcpi_rttvar);
    
    // 慢启动阈值
    append("ssthresh="sv, tcpi.tcpi_snd_ssthresh);
    
    // 拥塞窗口大小 (Congestion Window) - 吞吐量的关键
    append("cwnd="sv, tcpi.tcpi_snd_cwnd);
    
    // 通告 MSS (Maximum Segment Size)
    append("advmss="sv, tcpi.tcpi_advmss);
    
    // 乱序度
    append("reordering="sv, tcpi.tcpi_reordering);

    // === 统计总数 ===
    // 总重传次数 (判断网络质量的重要指标)
    append("total_retrans=", tcpi.tcpi_total_retrans);

    // 收尾：将最后一个空格替换为 \0
    if (p > buf) {
        *(p - 1) = '\0';
    } else {
        *p = '\0';
    }
    return true;
}

} // namespace KzNet