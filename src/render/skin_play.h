// プレイ画面のゲームの状態を、LR2スキンのオプション・数値・文字列・タイマーとして見せる。
// 対応表はOpenLR2の GetOptionFlag_dst / SetObjectValue_Num / SetObjectValue_Bargraph（LR2/LR2_skinobject.cpp）
#pragma once

#include <array>
#include <string>

#include "chart/chart.h"
#include "game/ghost.h"
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
    int lane_cover = 0;          // レーンカバーの量（0〜100）
    bool lane_cover_on = false;  // レーンカバーを使っていて、いま出している
    bool cover_adjusting = false;  // START か SELECT を押している（スキンの270：カバーの調整中）
    bool in_result = false;      // リザルト画面を描いている（タイマー40 は立たないので op80 が真）
    bool result_flip = false;    // リザルトで 1P と 2P を入れ替える（#FLIPRESULT。入れ替えは未実装なので今は常に偽）
    bool assist = false;         // 皿の自動演奏（op54/55）
    bool has_stagefile = false, has_backbmp = false;  // 画像を読めたか（op 190/191・194/195）
    // 表示用に補間した値（LR2: score_print / HP_print）。負なら補間せず実値を使う（リザルトなど）
    double score_print = -1, hp_print = -1;
    int hs_fix = 0;              // ハイスピード固定（ボタン55）
    std::array<int, 5> diff_level{-1, -1, -1, -1, -1};  // 同じフォルダの難易度ごとのレベル（数値45〜49）
    int fps = 0;
    int loaded = 0, load_total = 1;
    int judge_draw = 0;
    double last_miss_ms = -1e9;
    // 自己ベストとターゲットのゴースト（LR2: highScore / targetScore）。2P側の表示はターゲットの種類で決まる
    // （LR2の既定は target=0：2P側は更新しない、defaulttarget=90%）
    Ghost best, target;
    int target_type = 0;  // 0=更新しない, 1=自己ベスト, 2=ターゲット
    int best_clear = 0;   // この譜面の自己ベストのランプ（LR2の番号）
    int best_exscore = 0;
    // リザルト画面（LR2: sSelect.old は、このプレイの前の自己ベスト）
    int result_clear = 0;  // このプレイのランプ
    bool quit_early = false;
    int old_exscore = 0, old_maxcombo = 0, old_minbp = -1, old_total_notes = 0;
    int bp_now() const;    // このプレイのBP（途中でやめたときは残りのノーツも含む）
    int last_offset_ms = 0;
    int last_fast_slow = 0;  // OpenLR2の拡張：0=PGREAT, 1=FAST, 2=SLOW
    // 自分のノーツが1つ判定されたとき（空POORは除く）にゴーストを進める（LR2: JudgeToScore）
    void on_judged() {
        if (autoplay) return;
        best.deal();
        target.deal();
    }

    bool option(int op) const;
    int number(int id) const;
    bool bargraph(int type, double& val, double& max) const;
    bool slider(int type, int& val, int& lo, int& hi) const;
    std::string text(int id) const;
    int button(int type) const;
    // スキン読み込み時に #IF の判定に使う値（LR2: Main.cpp:1041 で全番号を評価している）
    std::array<bool, 1000> load_options() const;
};

}  // namespace bmj
