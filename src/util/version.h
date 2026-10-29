// BMjoy の版。配る版は CMake の BMJ_VERSION（年.月.日.番号）を入れてビルドする。開発中は "dev"（自動の更新はしない）
#pragma once

#ifndef BMJ_VERSION
#define BMJ_VERSION "dev"
#endif

namespace bmj {

inline constexpr const char* kBmjVersion = BMJ_VERSION;

}  // namespace bmj
