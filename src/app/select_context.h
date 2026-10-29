// 選曲スキンに渡す値（番号 → 値）。番号の意味は docs/lr2_select_spec.md 6章
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "skin/bmjoy_buttons.h"
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
    // フォルダにカーソルがあるときの、中の持っている譜面の記録の合計（スコアの欄に出す。LR2 はフォルダでは 0）。
    // EX・BP・回数は足したもの。ノーツ数・最大 EX・スコア率は遊んだ譜面のノーツ数で出す。判定は数を出さず、判定の数の合計に対する割合だけ（足して 100%）
    const ScoreRecord* folder_score = nullptr;
    int folder_notes = 0, folder_played_notes = 0;
    const Course* course = nullptr;      // コース（種類 8）のとき
    std::string folder_name;            // いまいるフォルダの名前（文字列30）
    std::string artist_note;            // 空でなければアーティスト（文字列 14）の左に出す（MOD のフォルダの譜面の確率など）
    std::string title_override;         // 空でなければ曲名の枠（文字列 10・12）に出す（MOD のバーのルーレット）
    // 曲でない要素（フォルダ・持っていない譜面など）の文字列 10〜15（タイトル、サブタイトル、フルタイトル、ジャンル、アーティスト、
    // サブアーティスト）。LR2 はフォルダにも同じ欄を持ち、カーソルの要素の欄をそのまま出す（フォルダの名前がタイトルの枠に出る）
    std::array<std::string, 6> bar_text;
    int edit_text = -1;                 // タグの欄（文字列 20〜29）を入力しているなら、その番号
    std::string edit_value;             // 入力している途中の文字
    int panel = -1;
    int difficulty_filter = 0, mode_filter = 0, sort_mode = 0;
    std::string mode_name = "ALL", sort_name = "OFF";  // beatoraja と同じ並びの名前（文字列 60 / 61）
    bool has_banner = false;  // バナーを読み込めた（op 193）
    bool has_stagefile = false;  // 決定画面：STAGEFILE を読み込めた（op 191）
    bool has_text = false;       // 曲のフォルダに .txt がある（op 174/175）
    PlayerStats player;          // プレイヤーの累計（数値 30〜37）
    std::string player_name;     // プレイヤーの名前（文字列 2。LR2 から引き継いだもの）
    const SkinCatalog* catalog = nullptr;  // スキンセレクト（文字列 50/51/100〜114、ボタン 170〜188、スライダー7）
    bool autoplay = false;
    bool windowed = true;
    std::string custom_gauge_name;  // カスタムゲージを選んでいればその名前
    int custom_gauge_frame = -1;    // そのときのゲージのボタンのコマ
    bool no_record = false;         // 記録を残さない MOD を使っている（メトロノーム）
    int judge_frame = 0;            // 判定の欄（BMjoy のボタン 900）のコマ：0 = OFF、1〜4 = VERY HARD … EASY、5〜13 = JUDGE1〜9
    int favorite = 0;            // カーソルの譜面のお気に入り（ボタン 90。0 / 1 = FAVORITE / 2 = IGNORE）
    bool replay_exists = false;  // カーソルの譜面のリプレイがある（op 196 / 197）
    // レベルバーの最大値（LR2: levelbarflash_5 / _7）。5 鍵の譜面は 5 鍵の値
    int levelbar_max() const { return chart && chart->keys == 5 ? cfg->levelbar_5 : cfg->levelbar_7; }
    int list_count = 0, list_pos = 0;  // 曲リストの数と、スクロール位置（1曲=1000。スライダー1）
    // 同じフォルダの難易度ごとの譜面（添字 0..4 = BEGINNER..INSANE）。レベル（無ければ -1）、数、ランプ
    std::array<int, 5> diff_level{-1, -1, -1, -1, -1}, diff_count{}, diff_lamp{};

    // 今の LN の種類で数えた総ノーツ数（CN・HCN なら、種類を決めていない LN を 1 本ぶん多く数える）
    int notes_of(const ChartRecord& c) const { return c.notes + (cfg && cfg->ln_mode > 0 ? c.ln_free : 0); }
    int course_notes() const;

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
