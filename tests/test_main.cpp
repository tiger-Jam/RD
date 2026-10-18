#include <cstring>

#include "test.h"

namespace bmjtest {
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int g_failures = 0;
}  // namespace bmjtest

int main(int argc, char** argv) {
    int ran = 0;
    for (const auto& c : bmjtest::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        int before = bmjtest::g_failures;
        c.fn();
        std::printf("%s %s\n", bmjtest::g_failures == before ? "ok  " : "NG  ", c.name);
        ++ran;
    }
    std::printf("%d件中 失敗%d件\n", ran, bmjtest::g_failures);
    return bmjtest::g_failures == 0 ? 0 : 1;
}
