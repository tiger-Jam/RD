// MD5 と SHA-256（仕様書5章「ハッシュ」）。譜面ファイルの原本に対して計算する
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bmj {

// 16進の小文字で返す
std::string md5_hex(const void* data, size_t len);
std::string sha256_hex(const void* data, size_t len);
inline std::string md5_hex(std::string_view s) { return md5_hex(s.data(), s.size()); }
inline std::string sha256_hex(std::string_view s) { return sha256_hex(s.data(), s.size()); }

}  // namespace bmj
