#pragma once

#include "KzLog/Logger.h"
#include "CurrentThread.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string_view>
#include <cstring>
#include <algorithm>

// 手动定义必要的内核常量，避免引入 <linux/mempolicy.h> 导致的头文件冲突
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif

namespace KzThread {
namespace ThreadUtils {

    inline void pin_to_core(size_t core_id) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) [[unlikely]] {
            fprintf(stderr, "[ERROR] Failed to set thread affinty to core : %ul\n", core_id);
        }
    }

    inline void enable_local_memory_policy() noexcept {
        if (::syscall(SYS_set_mempolicy, MPOL_PREFERRED, nullptr, 0) != 0) [[unlikely]] {
            fprintf(stderr, "[ERROR] Failed to set local memory policy \n");
        }
    }

    inline void set_thread_name(const std::string_view name) noexcept {
        if (name.empty()) [[unlikely]] return;

        char buf[16];
        size_t n = std::min(name.size(), sizeof(buf) - 1);
        std::memcpy(buf, name.data(), n);
        buf[n] = '\0';

        pthread_setname_np(pthread_self(), buf);
        ::memcpy(CurrentThread::_threadName, buf, n + 1);
        CurrentThread::_threadNameLength = static_cast<int>(n);
    }

    inline int get_current_core() {
        unsigned int core;
        if (::syscall(SYS_getcpu, &core, nullptr, nullptr) == 0) [[likely]] {
            return static_cast<int>(core);
        }
        return -1;
    }

} // namespace ThreadUtils
} // namespace KzThread