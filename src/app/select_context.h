// 選曲スキンに渡す値（番号 → 値）。番号の意味は docs/lr2_select_spec.md 6章
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/course.h"
#include "app/skin_catalog.h"
#include "db/song_db.h"
#include "skin/lr2_skin.h"
#include "skin/skin_renderer.h"

namespace bmj {

// LR2files/Config/optionstr.csv（オプションの表示名。#GAUGE,GROOVE,SURVIVAL,... の形）
struct OptionNames {
    std::map<std::string, std::vector<std::string>> rows;
    void load(const std::string& lr2_root);
    std::string get(const std::string& key, int i) const;
};

struct SelectContext {
    const Lr2Skin* skin = nullptr;
    const AppConfig* cfg = nullptr;
    const OptionNames* names = nullptr;
    SkinTimers timers;
    // カーソルの要素
    int type = 1;                       // バーの種類（0=曲 1=フォルダ …）
    const ChartRecord* chart = nullptr;  // 曲のとき
    const ScoreRecord* score = nullptr;  // 記録があるとき（コースのときはコースの記録）
    const Course* course = nullptr;      // コース（種類 8）のとき
    std::string folder_name;            // いまいるフォルダの名前（文字列30）
    int panel = -1;
    int difficulty_filter = 0, mode_filter = 0, sort_mode = 0;
    std::string mode_name = "ALL", sort_name = "OFF";  // beatoraja と同じ並びの名前（文字列 60 / 61）
    bool has_banner = false;  // バナーを読み込めた（op 193）
    bool has_stagefile = false;  // 決定画面：STAGEFILE を読み込めた（op 191）
    bool has_text = false;       // 曲のフォルダに .txt がある（op 174/175）
    PlayerStats player;          // プレイヤーの累計（数値 30〜37）
    const SkinCatalog* catalog = nullptr;  // スキンセレクト（文字列 50/51/100〜114、ボタン 170〜188、スライダー7）
    bool autoplay = false;
    bool windowed = true;
    int list_count = 0, list_pos = 0;  // 曲リストの数と、スクロール位置（1曲=1000。スライダー1）
    // 同じフォルダの難易度ごとの譜面（添字 0..4 = BEGINNER..INSANE）。レベル（無ければ -1）、数、ランプ
    std::array<int, 5> diff_level{-1, -1, -1, -1, -1}, diff_count{}, diff_lamp{};

    bool option(int op) const;
    int number(int id) const;
    std::string text(int id) const;
    int button(int type) const;
    bool slider(int type, int& val, int& lo, int& hi) const;
    bool bargraph(int type, double& val, double& max) const;
};

// クリアの記録から LR2 のランク（0=F .. 8=AAA を返す。記録が無ければ -1）
int select_rank(int exscore, int notes);

}  // namespace bmj
