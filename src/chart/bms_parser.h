// BMS系（.bms / .bme / .bml）のインポーター（仕様書5章）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/lr2_random.h"

#include "chart/chart.h"

namespace bmj {

struct BmsParseOptions {
    uint32_t random_seed = 0;  // #RANDOM の評価に使うシード
    // 選曲用（曲データベース）の読み込み。LR2 の ParseBMSMETA と同じく #RANDOM を評価せず、いつも「#IF 1」の枝を読む
    bool select_meta = false;
};

struct BmsParseResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<int> random_values;  // 評価した #RANDOM の値（出現順）。リプレイで再現するために保存する
    Lr2Random rng;                   // #RANDOM を評価したあとの乱数（レーンのランダムはこの続きを使う。LR2 と同じ）
    Chart chart;
};

// LR2 の選曲用の読み込みの、#TITLE・#GENRE からの難易度の推定（LR2: fileutil:287-367 GetDifficulty）。
// 末尾の HARD などや、末尾の括弧の中の言葉で difficulty を書き換える（当てはまらなければそのまま）。
// title・subtitle を渡すと、末尾の括弧（(…)・[…]・-…-・"…"・<…>・～…～・【…】。開きが先頭のものは除く）を
// 副題に分ける。分けられなければ title は text のまま、subtitle は空にする
void guess_difficulty_lr2(std::string_view text, int& difficulty, std::string* title, std::string* subtitle);

// text はUTF-8に変換済みのもの
BmsParseResult parse_bms_text(std::string_view text, const BmsParseOptions& opts);
// ファイルを読み、文字コードを判定してパースする。chart.base_dir にファイルのあるフォルダを入れる
BmsParseResult parse_bms_file(const std::string& path, const BmsParseOptions& opts);

}  // namespace bmj
