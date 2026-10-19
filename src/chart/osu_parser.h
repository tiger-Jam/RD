// osu!mania（.osu）のインポーター（仕様書5章、docs/osu_bmson_spec.md Part 1）
#pragma once

#include <string>
#include <string_view>

#include "chart/bms_parser.h"

namespace bmj {

struct OsuParseOptions {
    // 8K の皿：0 = 左端を皿 1 = 右端を皿（譜面ごとに選んで記録する仕組みは未実装。仕様書2章）
    int scratch_side = 0;
};

// Mode: 3（mania）の 7K・8K だけを取り込む。それ以外は ok = false
BmsParseResult parse_osu_text(std::string_view text, const OsuParseOptions& opts = {});

}  // namespace bmj
