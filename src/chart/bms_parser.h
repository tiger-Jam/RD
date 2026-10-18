// BMS系（.bms / .bme / .bml）のインポーター（仕様書5章）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chart/chart.h"

namespace bmj {

struct BmsParseOptions {
    uint32_t random_seed = 0;  // #RANDOM の評価に使うシード
};

struct BmsParseResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<int> random_values;  // 評価した #RANDOM の値（出現順）。リプレイで再現するために保存する
    Chart chart;
};

// text はUTF-8に変換済みのもの
BmsParseResult parse_bms_text(std::string_view text, const BmsParseOptions& opts);
// ファイルを読み、文字コードを判定してパースする。chart.base_dir にファイルのあるフォルダを入れる
BmsParseResult parse_bms_file(const std::string& path, const BmsParseOptions& opts);

}  // namespace bmj
