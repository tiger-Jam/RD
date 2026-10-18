#include "app/select_context.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

#include "game/gauge.h"
#include "util/text.h"

namespace bmj {

void OptionNames::load(const std::string& lr2_root) {
    std::string path = resolve_lr2_path(lr2_root, "LR2files/Config/optionstr.csv", false);
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        line = cp932_to_utf8(line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '#') continue;
        std::vector<std::string> cells;
        std::istringstream in(line);
        std::string c;
        while (std::getline(in, c, ',')) cells.push_back(c);
        if (cells.empty()) continue;
        std::string key = cells[0].substr(1);
        cells.erase(cells.begin());
        while (!cells.empty() && cells.back().empty()) cells.pop_back();
        rows[key] = cells;
    }
}

std::string OptionNames::get(const std::string& key, int i) const {
    auto it = rows.find(key);
    if (it == rows.end() || i < 0 || i >= static_cast<int>(it->second.size())) return {};
    return it->second[i];
}

int select_rank(int exscore, int notes) {
    if (notes <= 0) return -1;
    int r = std::min(8, exscore * 9 / (notes * 2));
    if (exscore > 0 && r < 2) r = 1;
    return r;
}

namespace {
bool is_song(int type) { return type == 0 || type == 5; }
bool is_folder(int type) { return type == 1 || type == 2 || type == 3 || type == 4 || type == 6; }
// ゲージで付くランプ（仕様 6.2 の 62..66）：1=EASY 2=NORMAL 3=HARD 4=EX HARD 相当
int gauge_lamp(int gauge) {
    switch (gauge) {
    case kEasy: return 1;
    case kHard: case kGAttack: return 3;
    case kDeath: case kPAttack: return 4;
    default: return 2;
    }
}
}  // namespace

bool SelectContext::option(int op) const {
    if (op < 0) return !option(-op);
    const bool song = chart && is_song(type);
    const int lamp = song && score ? score->clear : 0;
    switch (op) {
    case 0: return true;
    case 1: return is_folder(type);
    case 2: return is_song(type);
    case 3: return type == 8;
    case 4: return type == 7 || type == 9;
    case 5: return is_song(type) || type == 8 || type == 9;
    case 20: return panel <= 0;
    case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: return panel == op - 20;
    case 32: return !autoplay;
    case 33: return autoplay;
    case 34: return true;   // ゴースト OFF
    case 39: return true;   // スコアグラフ ON（プレイでは描いている）
    case 80: return true;   // 選曲ではタイマー40 が立たないので「読み込み中」が真（LR2 と同じ）
    case 41: return true;   // BGA ON
    case 46: return true;   // 難易度フィルタ有効
    case 50: return true;   // オフライン
    case 52: return true;   // EXTRA モード OFF
    case 54: return !cfg->assist;
    case 55: return cfg->assist;
    case 56: return true;  // 2P のアシスト OFF
    case 61: return true;   // スコアが保存される
    case 62: case 63: case 64: case 65: case 66: {
        // SCATTER と皿の自動演奏は EASY 止まり
        int lamp = gauge_lamp(cfg->gauge);
        if ((cfg->assist && cfg->random != 5) || cfg->random == 4) lamp = std::min(lamp, 1);
        return lamp == op - 62;
    }
    case 100: case 101: case 102: case 103: case 104: case 105: return song && lamp == op - 100;
    case 110: case 111: case 112: case 113: case 114: case 115: case 116: case 117: {
        if (!song || !score || score->clear <= 0) return false;
        return select_rank(score->exscore, chart->notes) == 8 - (op - 110);
    }
    case 150: return song && (chart->difficulty <= 0 || chart->difficulty > 5);
    case 151: case 152: case 153: case 154: case 155: return song && chart->difficulty == op - 150;
    case 160: return song;  // BMjoy は 7 鍵だけ
    case 170: return true;  // BGA の有無は選曲では見ない
    case 172: return song && chart->ln == 0;
    case 173: return song && chart->ln > 0;
    case 174: return !has_text;
    case 175: return has_text;
    case 176: return song && chart->bpm_min == chart->bpm_max;
    case 177: return song && chart->bpm_min != chart->bpm_max;
    case 178: return true;
    case 180: return song && chart->rank <= 0;
    case 181: return song && chart->rank == 1;
    case 182: return song && chart->rank == 2;
    case 183: return song && chart->rank >= 3;
    case 185: return song && chart->playlevel <= cfg->levelbar_7;
    case 186: return song && chart->playlevel > cfg->levelbar_7;
    case 190: return !has_stagefile;
    case 191: return has_stagefile;
    case 192: return !has_banner;
    case 193: return has_banner;
    case 194: return !(song && !chart->backbmp.empty());
    case 195: return song && !chart->backbmp.empty();
    case 196: return true;  // リプレイ無し
    case 572: return true;  // コースを作っていない
    case 500: case 501: case 502: case 503: case 504: return song && diff_count[op - 500] == 0;
    case 505: case 506: case 507: case 508: case 509: return song && diff_count[op - 505] > 0;
    case 510: case 511: case 512: case 513: case 514: return song && diff_count[op - 510] == 1;
    case 515: case 516: case 517: case 518: case 519: return song && diff_count[op - 515] > 1;
    case 600: return true;  // IR 未接続
    case 620: case 622: case 624: return true;
    default: break;
    }
    if (op >= 118 && op <= 149) return song && score && (score->op_history >> (op - 118) & 1);  // クリア時のオプション履歴
    if (op >= 70 && op <= 79) {
        // 同じフォルダの難易度 0..4 のレベルがレベルバーの上限以下（70〜74）／超える（75〜79）
        int d = (op - 70) % 5;
        if (!song || diff_count[d] == 0) return false;
        return op < 75 ? diff_level[d] <= cfg->levelbar_7 : diff_level[d] > cfg->levelbar_7;
    }
    if (op >= 520 && op <= 565) {
        // 難易度 d のランプが v（op = 520 + d*10 + v）
        int d = (op - 520) / 10, v = (op - 520) % 10;
        return song && d < 5 && v <= 5 && diff_count[d] > 0 && diff_lamp[d] == v;
    }
    if (op >= 900 && op < 1000 && skin) return skin->op[op];
    return false;
}

int SelectContext::number(int id) const {
    const bool song = chart && is_song(type);
    const ScoreRecord* s = song ? score : nullptr;
    int notes = song ? chart->notes : 0;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    switch (id) {
    case 10: case 11: return cfg->hi_speed;
    case 12: return cfg->judge_timing;
    case 13: return 90;
    case 14: return cfg->lane_cover_v;
    case 15: return 0;  // 2P 側は設定を持たないので既定値
    case 21: return tm.tm_year + 1900;
    case 22: return tm.tm_mon + 1;
    case 23: return tm.tm_mday;
    case 24: return tm.tm_hour;
    case 25: return tm.tm_min;
    case 26: return tm.tm_sec;
    case 42: case 96: return song ? chart->playlevel : 0;
    case 45: case 46: case 47: case 48: case 49: return song ? diff_level[id - 45] : -1;
    case 71: return s ? s->exscore : 0;
    case 72: return notes * 2;
    case 73: return s && notes > 0 ? s->exscore * 100 / (notes * 2) : 0;
    case 74: return notes;
    case 75: return s ? s->max_combo : 0;
    case 76: return s && s->bp >= 0 ? s->bp : 0;
    case 77: return s ? s->playcount : 0;
    case 78: return s ? s->clearcount : 0;
    case 79: return s ? s->playcount - s->clearcount : 0;
    case 80: case 81: case 82: case 83: case 84: return s ? s->judge[id - 80] : 0;  // ベストの PG〜PR の数
    case 85: case 86: case 87: case 88: case 89: return s && notes > 0 ? s->judge[id - 85] * 100 / notes : 0;  // 同じく割合
    case 30: return player.playcount;
    case 31: return player.clear;
    case 32: return player.fail;
    case 33: case 34: case 35: case 36: case 37: return static_cast<int>(player.judge[id - 33]);
    case 38: case 39: return 0;  // ランニングコンボ（記録していない）
    case 90: return song && chart->bpm_max > 0 ? static_cast<int>(chart->bpm_max) : -1;
    case 91: return song && chart->bpm_min > 0 ? static_cast<int>(chart->bpm_min) : -1;
    default: return -1;
    }
}

std::string SelectContext::text(int id) const {
    const bool song = chart && is_song(type);
    auto opt = [&](const char* key, int i) { return names ? names->get(key, i) : std::string(); };
    switch (id) {
    case 1: return "NO TARGET";
    case 10: return song ? chart->title : "";
    case 11: return song ? chart->subtitle : "";
    case 12: return song ? chart->title + (chart->subtitle.empty() ? "" : " " + chart->subtitle) : "";
    case 13: return song ? chart->genre : "";
    case 14: return song ? chart->artist : "";
    case 15: return song ? chart->subartist : "";
    case 17: return song ? std::to_string(chart->playlevel) : "";
    case 18: return song ? std::to_string(chart->difficulty) : "";
    case 30: return folder_name;
    case 60: return mode_name;
    case 61: return sort_name;
    case 62: return opt("DIFFICULTY", difficulty_filter);
    case 63: return opt("RANDOM", cfg->random);
    case 64: return opt("RANDOM", 0);
    case 65: return opt("GAUGE", cfg->gauge);
    case 66: return opt("GAUGE", 0);
    case 67: return opt("ASSIST", cfg->assist ? 1 : 0);
    case 68: return opt("ASSIST", 0);
    case 69: return opt("BATTLE", 0);
    case 70: return opt("FLIP", 0);
    case 71: return opt("SCOREGRAPH", 1);
    case 72: return opt("GHOST", 0);
    case 73: return opt("LANECOVER", cfg->lane_cover ? 1 : 0);
    case 74: return cfg->hs_fix == 5 ? "MAINBPM" : opt("HSFIX", cfg->hs_fix);
    case 75: return opt("BGASIZE", 0);
    case 76: return opt("BGADRAW", 1);
    case 78: return opt("VSYNC", cfg->vsync ? 1 : 0);
    case 79: return opt("SCREEN", windowed ? 1 : 0);
    case 80: return opt("AUTOADJUST", 0);
    case 81: return opt("REPLAYSAVE", 0);
    case 84: return opt("EFFECT", cfg->effect);
    case 85: return opt("EFFECT", 0);
    default: return {};
    }
}

int SelectContext::button(int t) const {
    switch (t) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: return panel == t ? 1 : 0;
    case 10: return difficulty_filter;
    case 11: {
        // スキンの絵は LR2 の並び（ALL KEYS, SINGLE, 7KEYS, 5KEYS, DOUBLE, 14KEYS, 10KEYS, 9KEYS）なので、BMjoy の並びから読み替える
        static const int lr2[] = {0, 2, 5, 7, 3, 6, 1, 4};
        return mode_filter >= 0 && mode_filter < 8 ? lr2[mode_filter] : 0;
    }
    case 12: {
        // LR2 の並び（OFF, TITLE, LEVEL, FOLDER, CLEAR, RANK）に同じものがあればそのコマ。ARTIST・BPM・LENGTH・MISSCOUNT は LR2 に無いので OFF の絵
        static const int lr2[] = {0, 1, 0, 0, 0, 2, 4, 5, 0};
        return sort_mode >= 0 && sort_mode < 9 ? lr2[sort_mode] : 0;
    }
    case 40: return cfg->gauge;
    case 41: return 0;
    case 46: return cfg->lane_cover ? 1 : 0;
    case 42: return cfg->random;
    case 44: return cfg->assist ? 1 : 0;
    case 50: return cfg->effect;
    case 51: return 0;
    case 55: return cfg->hs_fix;
    case 57: case 58: return 1;
    case 70: return 1;  // スコアグラフ ON
    case 72: return 1;  // BGA ON
    case 76: return 90;
    case 80: return windowed ? 1 : 0;
    case 82: return cfg->vsync ? 1 : 0;
    default: return 0;
    }
}

bool SelectContext::slider(int t, int& val, int& lo, int& hi) const {
    switch (t) {
    case 1: val = list_pos, lo = 0, hi = std::max(1, list_count * 1000 - 1); return true;
    case 2: case 3: val = cfg->hi_speed, lo = 10, hi = 990; return true;
    case 4: case 5: val = cfg->lane_cover_v, lo = 0, hi = 100; return true;
    default: val = 0, lo = 0, hi = 1; return true;
    }
}

bool SelectContext::bargraph(int t, double& val, double& max) const {
    const bool song = chart && is_song(type);
    switch (t) {
    case 3: val = song ? chart->playlevel : 0, max = cfg->levelbar_7; return true;
    case 5: case 6: case 7: case 8: case 9: val = song ? diff_level[t - 5] : 0, max = cfg->levelbar_7; return true;
    case 47: val = song && score ? score->exscore : 0, max = song ? chart->notes * 2 : 0; return true;
    default: return false;
    }
}

}  // namespace bmj
