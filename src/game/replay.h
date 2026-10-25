// リプレイ（仕様書6章「リプレイ（入力ログ）」）。鍵盤の入力を曲の時刻つきで残し、同じ乱数の種とオプションで
// 譜面を作り直して、判定をもう一度動かす。LR2 と同じく譜面ごとに 1 つ（LR2: LR2files/Replay/<ID>/<md5>.lr2rep）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

struct ReplayEvent {
    int64_t time_us = 0;  // 曲の時刻
    uint8_t lane = 0;
    bool down = false;
};

struct Replay {
    static constexpr int kVersion = 1;  // 書き方を変えたら上げる（古い版も読めるようにしておく）
    std::string sha256;
    uint32_t seed = 0;  // 譜面の #RANDOM とレーンの並べ替えの乱数の種
    int random_version = 1;  // 配置の作り方の版（kRandomAlgorithmVersion。古い記録には無いので 1）
    int judge_timing = 0;    // 判定タイミングの設定（ms。描画だけをずらすので判定には効かないが、残しておく）
    int64_t played_at = 0;   // 遊んだ日時（UNIX 秒）
    // 譜面と判定に効くオプション（再生のときはこれに合わせ、終わったら元に戻す）
    int random = 0, gauge = 0, ln_mode = 0, hs_fix = 0, effect = 0;
    bool seven_key = false, gas = false;
    // 見た目のオプション（再生でも同じ見え方にする）
    int hi_speed = 100, lane_cover = 0, lane_cover_v = 0;
    // MOD：プレーに影響する MOD の組み合わせ（play_mod_key）と、遊んだカスタムゲージの鍵（無ければ空）
    std::string mods, custom_gauge;
    std::vector<ReplayEvent> events;
};

bool save_replay(const std::string& path, const Replay& r);
bool load_replay(const std::string& path, Replay& r);

}  // namespace bmj
