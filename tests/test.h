// 最小限のテスト枠組み
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace bmjtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
extern int g_failures;

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace bmjtest

#define BMJ_CAT2(a, b) a##b
#define BMJ_CAT(a, b) BMJ_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static bmjtest::Register BMJ_CAT(reg_, name)(#name, name);                  \
    static void name()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::printf("  失敗 %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            ++bmjtest::g_failures;                                                           \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        auto va_ = (a);                                                                      \
        auto vb_ = (b);                                                                      \
        if (!(va_ == vb_)) {                                                                 \
            std::printf("  失敗 %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b,    \
                        std::to_string(va_).c_str(), std::to_string(vb_).c_str());           \
            ++bmjtest::g_failures;                                                           \
        }                                                                                    \
    } while (0)
