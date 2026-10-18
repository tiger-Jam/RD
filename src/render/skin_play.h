// プレイ画面のゲームの状態を、LR2スキンのオプション・数値・文字列・タイマーとして見せる。
// 対応表はOpenLR2の GetOptionFlag_dst / SetObjectValue_Num / SetObjectValue_Bargraph（LR2/LR2_skinobject.cpp）
#pragma once

#include <array>
#include <string>

#include "chart/chart.h"
#include "game/judge.h"
#include "skin/lr2_skin.h"
#include "skin/skin_renderer.h"

namespace bmj {

struct PlayContext {
    const Chart* chart = nullptr;
    const JudgeEngine* judge = nullptr;
    const Lr2Skin* skin = nullptr;
    SkinTimers timers;
    double now_ms = 0;       // シーンの時刻
    double song_ms = -1;     // タイマー41の経過（曲の時刻）
    bool autoplay = false;
    int gauge_type = kGroove;
    int hi_speed = 100;
    int judge_timing = 0;
    int lane_cover = 0;
    int fps = 0;
    int loaded = 0, load_total = 1;
    int judge_draw = 0;
    double last_miss_ms = -1e9;

    bool option(int op) const;
    int number(int id) const;
    bool bargraph(int type, double& val, double& max) const;
    bool slider(int type, int& val, int& lo, int& hi) const;
    std::string text(int id) const;
    // スキン読み込み時に #IF の判定に使う値（LR2: Main.cpp:1041 で全番号を評価している）
    std::array<bool, 1000> load_options() const;
};

}  // namespace bmj
