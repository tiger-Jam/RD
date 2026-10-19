// プレイスキンの位置の調整（LR2: skstruct.adjust。演奏中に数字キー＋矢印で変え、スキンごとに残す）
#pragma once

namespace bmj {

struct SkinAdjust {
    int shift_x = 0, shift_y = 0;    // 1: スキン全体の位置（10 ずつ）
    int rate_x = 100, rate_y = 100;  // 2: スキン全体の拡大率（%。5 ずつ）
    int judge_x = 0, judge_y = 0;    // 3: 判定の表示の位置（10 ずつ）
    int size_x = 0, size_y = 0;      // 4: ノーツの大きさ（1 ずつ）
    int dark = 0;                    // 5: 描画の制限 0=OFF 1=DARK 1 2=DARK 2
    int note_x = 0, note_y = 0;      // 6: 1P のノーツの位置（10 ずつ。7 の 2P は BMjoy に無い）
    bool is_default() const {
        return shift_x == 0 && shift_y == 0 && rate_x == 100 && rate_y == 100 && judge_x == 0 && judge_y == 0 && size_x == 0 &&
               size_y == 0 && dark == 0 && note_x == 0 && note_y == 0;
    }
};

}  // namespace bmj
