// 演奏シーン（読み込み → READY → 演奏 → 終了 → リザルト）。場面の進み方はLR2に合わせている
// （参照: OpenLR2 LR2/Scene04_Play.cpp:1748-1768）
#pragma once

#include <cstdint>
#include <string>

#include "app/app.h"

namespace bmj {

struct PlayOptions {
    bool autoplay = false;
    uint32_t seed = 0;
    std::string capture_dir;  // 開発用：描画結果を2秒ごとにBMPで保存する
    double quit_after = 0;    // 開発用：指定秒数で終了する
    bool save_score = true;
    bool decide_sound = true;  // 決定の音を鳴らす（決定画面を出したときは、そちらで鳴らしているので鳴らさない）
};

enum class PlayExit { Back, Quit };

// 譜面のキー音のファイル名を、実在するファイルに解決する（見つからないものは missing に入れる）
std::vector<std::string> resolve_chart_sounds(const Chart& chart, std::vector<std::string>& missing);

// 譜面を1つ遊ぶ。リザルトで F5 を押すと同じ譜面をもう一度遊ぶ
PlayExit run_play(App& app, const std::string& chart_path, PlayOptions opt);

}  // namespace bmj
