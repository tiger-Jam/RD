// 仮のプレイ画面。LR2スキンの読み込み（ロードマップ4段階目）までのつなぎ。
// 配置はLR2の既定スキン（Theme/LR2/Play/7keys/7_LL0.csv、640x480）に合わせている
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "chart/chart.h"
#include "game/judge.h"

namespace rd {

struct PlayViewState {
    double song_ms = 0.0;    // 判定に使う時刻
    double render_beat = 0.0; // 描画に使うスクロール位置（判定タイミングの調整を足した時刻から求める）
    int hi_speed = 100;      // LR2のハイスピ（100=1.00倍）
    double speed_multiplier = 1.0;
    int last_judge = -1;
    int last_offset_ms = 0;
    bool last_fast = false;
    bool last_has_offset = false;
    double last_judge_ms = -1e9;
    int combo = 0;
    bool autoplay = false;
    bool failed = false;
    bool finished = false;
    std::string status;  // 画面下に出す一行
};

void draw_play(SDL_Renderer* r, const Chart& chart, const JudgeEngine& judge, const PlayViewState& v);
void draw_loading(SDL_Renderer* r, const Chart& chart, int done, int total);
void draw_result(SDL_Renderer* r, const Chart& chart, const JudgeEngine& judge);

}  // namespace rd
