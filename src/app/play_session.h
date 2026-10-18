// 演奏シーン（読み込み → READY → 演奏 → 終了 → リザルト）。場面の進み方はLR2に合わせている
// （参照: OpenLR2 LR2/Scene04_Play.cpp:1748-1768）
#pragma once

#include <cstdint>
#include <string>

#include "app/app.h"
#include "app/course.h"
#include "game/judge.h"

namespace bmj {

// コースの進み具合。run_play が 1 ステージ遊ぶたびに書き換え、次のステージへ持ち越す（LR2: bmsload:537）
struct CourseRun {
    const Course* course = nullptr;
    int stage = 0;                          // いまのステージ（0 始まり）
    std::array<double, kGaugeCount> hp{};   // ステージの終わりの全ゲージ
    int gauge_type = kGroove;
    int combo = 0, max_combo = 0;           // コース通しのコンボ
    PlayStats total;                        // コース通しの判定数・処理したノーツ数・総ノーツ数
    bool alive = false;      // このステージを生きて終えた（次があれば進める）
    bool finished = false;   // このステージのノーツを全部処理した
    bool abandoned = false;  // リザルトを出さずに選曲へ戻る（1 ノーツも判定しなかったなど）
};

struct PlayOptions {
    bool autoplay = false;
    uint32_t seed = 0;
    std::string capture_dir;  // 開発用：描画結果を2秒ごとにBMPで保存する
    double quit_after = 0;    // 開発用：指定秒数で終了する
    bool save_score = true;
    bool decide_sound = true;  // 決定の音を鳴らす（決定画面を出したときは、そちらで鳴らしているので鳴らさない）
    CourseRun* course = nullptr;  // コースの 1 ステージとして遊ぶ
};

enum class PlayExit { Back, Quit };

// 譜面のキー音のファイル名を、実在するファイルに解決する（見つからないものは missing に入れる）
std::vector<std::string> resolve_chart_sounds(const Chart& chart, std::vector<std::string>& missing);

// 譜面を1つ遊ぶ。リザルトで F5 を押すと同じ譜面をもう一度遊ぶ
PlayExit run_play(App& app, const std::string& chart_path, PlayOptions opt);

// コースを遊ぶ。ステージごとにプレイとリザルト、最後にコースリザルト
PlayExit run_course(App& app, const Course& course, PlayOptions opt);

}  // namespace bmj
