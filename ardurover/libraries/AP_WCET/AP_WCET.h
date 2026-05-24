#pragma once

#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdint.h>

#if defined(__linux__)
#include <time.h>
#endif

namespace AP_WCET {

static inline uint64_t now_us() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
#endif
}

}

class TimingLogger {
public:
    static constexpr int BUFFER_SIZE = 256;
    static constexpr const char* FILE_PATH = "timing_log.txt";

    TimingLogger(const char* task_name) {
        strncpy(name_, task_name, sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
    }

    void log(unsigned long long duration_us) {
        entries_[count_].duration_us = duration_us;
        strncpy(entries_[count_].name, name_, sizeof(entries_[count_].name) - 1);
        count_++;
        if (count_ >= BUFFER_SIZE) {
            flush();
        }
    }

    ~TimingLogger() {
        flush();
    }

private:
    struct Entry {
        char name[64];
        unsigned long long duration_us;
    };

    void flush() {
        if (count_ == 0) return;
        FILE* fp = fopen(FILE_PATH, "a");
        if (fp) {
            for (int i = 0; i < count_; i++) {
                fprintf(fp, "%s %llu\n", entries_[i].name, entries_[i].duration_us);
            }
            fclose(fp);
        }
        count_ = 0;
    }

    char name_[64];
    Entry entries_[BUFFER_SIZE];
    int count_ = 0;
};
