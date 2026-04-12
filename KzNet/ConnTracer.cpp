// DebugTracer.cpp
#include "ConnTracer.h"
#include "TcpConnection.h" // 在这里包含完整的 TcpConnection 定义

// --- ConnTracer 的实现 ---

void ConnTracer::log(const char* event) {
    if (conn_) {
        // 格式: [TRACE: 连接名] use_count=X | 事件 消息 @ 文件:行号
        std::cout << "[TRACE: " << conn_->getname() << "] use_count=" << conn_.use_count()
                  << " | " << event << " " << msg_ << " @ " << file_ << ":" << line_ << std::endl;
    }
}

ConnTracer::ConnTracer(const std::shared_ptr<KzNet::TcpConnection>& conn, std::string msg, const char* file, int line)
    : conn_(conn), msg_(std::move(msg)), file_(file), line_(line) {
    log("Enter");
}

ConnTracer::~ConnTracer() {
    log("Exit");
}

// --- LogConn 的实现 ---

void LogConn(const std::shared_ptr<KzNet::TcpConnection>& conn, const std::string& msg, const char* file, int line) {
    if (conn) {
        std::cout << "[LOG: " << conn->getname() << "] use_count=" << conn.use_count()
                  << " | " << msg << " @ " << file << ":" << line << std::endl;
    }
}