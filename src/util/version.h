// RD の版。配る版は CMake の RD_VERSION（年.月.日.番号）を入れてビルドする。開発中は "dev"（自動の更新はしない）
#pragma once

#ifndef RD_VERSION
#define RD_VERSION "dev"
#endif

namespace rd {

inline constexpr const char* kRdVersion = RD_VERSION;

}  // namespace rd
