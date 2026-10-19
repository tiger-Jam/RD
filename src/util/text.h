#pragma once

#include <string>
#include <string_view>

namespace bmj {

bool is_valid_utf8(std::string_view s);
// Shift_JIS（CP932）をUTF-8に変換する。変換できない文字は '?' にする。
std::string cp932_to_utf8(std::string_view s);
// UTF-8 を Shift_JIS（CP932）に変換する。変換できない文字は '?' にする（LR2 と同じ並び順にするため）
std::string utf8_to_cp932(std::string_view s);
// UTF-8ならそのまま、そうでなければCP932とみなして変換する。
std::string to_utf8_auto(std::string_view s);

std::string to_upper_ascii(std::string_view s);
std::string_view trim(std::string_view s);

}  // namespace bmj
