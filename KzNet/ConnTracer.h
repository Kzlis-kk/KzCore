// DebugTracer.h
#pragma once

#include <memory>
#include <string>
#include <iostream>

// --- 核心：前向声明，打破循环依赖 ---
namespace KzNet {
class TcpConnection;
}
// ------------------------------------

// RAII 追踪器
class ConnTracer {
public:
    // 只保留构造和析构的声明
    ConnTracer(const std::shared_ptr<KzNet::TcpConnection>& conn, std::string msg, const char* file, int line);
    ~ConnTracer();

private:
    // 将实现移到 .cpp 文件
    void log(const char* event);

    std::shared_ptr<KzNet::TcpConnection> conn_;
    std::string msg_;
    const char* file_;
    int line_;
};

// 静态日志函数也只保留声明
void LogConn(const std::shared_ptr<KzNet::TcpConnection>& conn, const std::string& msg, const char* file, int line);

// 宏定义保持不变，因为它们只依赖于上面的声明
#define TRACE_CONN(conn, msg) ConnTracer tracer_##__LINE__(conn, msg, __FILE__, __LINE__)
#define LOG_CONN(conn, msg) LogConn(conn, msg, __FILE__, __LINE__)