#pragma once

#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include "KzAlgorithm/Jeaiii.h"

class TimeFormatter {
public:
    // 返回格式化后的时间字符串长度，并将结果写入 buf
    size_t format(time_t current_seconds, char* buf) noexcept {
        // 缓存命中：还是同一秒
        if (current_seconds == _last_second) [[likely]] {
            std::memcpy(buf, _time_str_cache, 15); // 拷贝 "YYYYMMDD-HHMMSS"
            return 15;
        }

        // 缓存未命中：跨秒了，重新计算
        _last_second = current_seconds;
        
        struct tm tm_time;
        ::localtime_r(&current_seconds, &tm_time); 

        // 使用你写的 Jeaiii 极速格式化
        int year = tm_time.tm_year + 1900;
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache, year / 100);
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 2, year % 100);
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 4, tm_time.tm_mon + 1);
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 6, tm_time.tm_mday);
        _time_str_cache[8] = '-';
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 9, tm_time.tm_hour);
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 11, tm_time.tm_min);
        KzAlgorithm::Jeaiii::write_pair(_time_str_cache + 13, tm_time.tm_sec);

        // 写入目标 buf
        std::memcpy(buf, _time_str_cache, 15);
        return 15;
    }

private:
    time_t _last_second = 0;
    char _time_str_cache[16]; // 缓存格式化好的时间字符串
};