// 起動にかかった時間をログに残す（どこで待たされているかを、遊んだあとのログから分かるように）。
// 時刻はプロセスが始まってから（main の最初で startup_clock_start を呼ぶ）
#pragma once

#include <chrono>
#include <cstdio>

namespace bmj {

inline std::chrono::steady_clock::time_point& startup_clock_origin() {
    static std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    return t;
}

inline void startup_clock_start() { startup_clock_origin() = std::chrono::steady_clock::now(); }

// 「[起動 1234 ms] what」をログに出す
inline void startup_mark(const char* what) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startup_clock_origin()).count();
    std::printf("[起動 %lld ms] %s\n", static_cast<long long>(ms), what);
}

}  // namespace bmj
