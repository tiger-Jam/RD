#include <cstring>

#include "test.h"

namespace rdtest {
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int g_failures = 0;
}  // namespace rdtest

int main(int argc, char** argv) {
    int ran = 0;
    for (const auto& c : rdtest::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        int before = rdtest::g_failures;
        c.fn();
        std::printf("%s %s\n", rdtest::g_failures == before ? "ok  " : "NG  ", c.name);
        ++ran;
    }
    std::printf("%d件中 失敗%d件\n", ran, rdtest::g_failures);
    return rdtest::g_failures == 0 ? 0 : 1;
}
