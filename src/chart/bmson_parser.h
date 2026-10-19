// bmson のインポーター（仕様書5章、docs/osu_bmson_spec.md Part 2）。1.0 と、version の無い 0.21 を読む
#pragma once

#include <string_view>

#include "chart/bms_parser.h"

namespace bmj {

// beat-7k（と mode_hint の無いもの）だけを取り込む。それ以外は ok = false
BmsParseResult parse_bmson_text(std::string_view text);

}  // namespace bmj
