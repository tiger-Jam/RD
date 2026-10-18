#include "render/skin_play.h"

#include <cstdlib>

namespace bmj {

bool PlayContext::option(int option) const {
    bool ret = option >= 0;
    if (!ret) option = -option;
    const PlayStats* st = judge ? &judge->stats() : nullptr;
    double hp = judge ? judge->gauge().hp(judge->gauge_type()) : 0;
    int gtype = judge ? judge->gauge_type() : gauge_type;
    auto rank_of = [](int ex, int notes) { return notes > 0 ? ex * 9 / (notes * 2) : 0; };
    switch (option) {
    case 0: return true;
    case 30: return ret;              // BGAは通常サイズ
    case 31: return !ret;
    case 32: return autoplay ? !ret : ret;
    case 33: return autoplay ? ret : !ret;
    case 34: return ret;              // ゴースト無し
    case 35: case 36: case 37: return !ret;
    case 38: return !ret;             // スコアグラフあり
    case 39: return ret;
    case 40: return !ret;             // BGAあり
    case 41: return ret;
    case 42: return (gtype == kGroove || gtype == kEasy) ? ret : !ret;
    case 43: return (gtype == kGroove || gtype == kEasy) ? !ret : ret;
    case 46: return ret;
    case 47: return !ret;
    case 50: return ret;              // オフライン
    case 80: return timers.on(40) ? !ret : ret;  // 読み込み中
    case 81: return timers.on(40) ? ret : !ret;
    case 82: return ret;              // リプレイ無し
    case 83: case 84: return !ret;
    case 160: return ret;             // 7KEYS
    case 161: case 162: case 163: case 164: return !ret;
    case 200: case 201: case 202: case 203: case 204: case 205: case 206: case 207: {
        // 判定済みノーツ数に対するランク（AAA..F）
        if (!st || st->note_current == 0) return !ret;
        int r = rank_of(st->exscore(), st->note_current);
        int want = 8 - (option - 200);  // 200=AAA(8)
        if (option == 200) return r >= 8 ? ret : !ret;
        if (option == 207) return r < 2 ? ret : !ret;
        return (r < want + 1 && r > want - 1) ? ret : !ret;
    }
    case 220: case 221: case 222: case 223: case 224: case 225: case 226: case 227: {
        // 総ノーツ数に対するランク（以上）
        if (!st || st->total_notes == 0) return !ret;
        int r = rank_of(st->exscore(), st->total_notes);
        if (option == 220) return r >= 8 ? ret : !ret;
        if (option == 227) return r > 1 ? !ret : ret;
        return r > 6 - (option - 221) ? ret : !ret;
    }
    case 230: case 231: case 232: case 233: case 234: case 235: case 236: case 237: case 238: case 239: {
        double lo = (option - 230) * 10.0, hi = lo + 10.0;
        if (hp < lo) return !ret;
        return hp < hi ? ret : !ret;
    }
    case 240: return hp >= 100.0 ? ret : !ret;
    case 241: case 242: case 243: case 244: case 245: case 246:
        return judge_draw == 5 - (option - 241) ? ret : !ret;
    case 247: return now_ms - last_miss_ms >= 1000.0 ? ret : !ret;
    case 248: return now_ms - last_miss_ms >= 1000.0 ? !ret : ret;
    default:
        if (option < 1000 && skin) return skin->op[option] ? ret : !ret;
        return !ret;
    }
}

int PlayContext::number(int id) const {
    const PlayStats* st = judge ? &judge->stats() : nullptr;
    switch (id) {
    case 10: return hi_speed;
    case 12: return judge_timing;
    case 14: return lane_cover;
    case 20: return fps;
    default: break;
    }
    if (!st) return -1;
    switch (id) {
    case 100: return st->total_notes > 0 ? static_cast<int>((st->judge_count[kGood] + (st->judge_count[kGreat] + st->judge_count[kPGreat] * 2) * 2) * 50000ull / st->total_notes) : 0;
    case 101: return st->exscore();
    case 102: return st->note_current > 0 ? st->exscore() * 100 / (st->note_current * 2) : 0;
    case 103: return st->note_current > 0 ? static_cast<int>(st->exscore() * 100.0 / (st->note_current * 2) * 100.0) % 100 : 0;
    case 104: return st->combo;
    case 105: return st->max_combo;
    case 106: return st->total_notes;
    case 107: return static_cast<int>(judge->gauge().hp(judge->gauge_type())) / 2 * 2;
    case 110: return st->judge_count[kPGreat];
    case 111: return st->judge_count[kGreat];
    case 112: return st->judge_count[kGood];
    case 113: return st->judge_count[kBad];
    case 114: return st->judge_count[kPoor] + st->judge_count[kEmptyPoor];
    case 115: return st->total_notes > 0 ? st->exscore() * 100 / (st->total_notes * 2) : -1;
    case 116: return st->total_notes > 0 ? st->exscore() * 10000 / (st->total_notes * 2) % 100 : -1;
    case 160: return chart ? static_cast<int>(chart->bpm_at(static_cast<int64_t>(std::max(0.0, song_ms) * 1000))) : -1;
    case 161: return song_ms / 1000 > 0 ? static_cast<int>(song_ms) / 1000 / 60 : 0;
    case 162: return song_ms / 1000 > 0 ? static_cast<int>(song_ms) / 1000 % 60 : 0;
    case 163: case 164: {
        int rest = chart ? static_cast<int>(chart->end_us / 1000 - std::max(0.0, song_ms)) : 0;
        if (rest / 1000 <= 0) return 0;
        return id == 163 ? rest / 1000 / 60 : rest / 1000 % 60;
    }
    case 165: return load_total > 0 ? loaded * 100 / load_total : 0;
    case 210: return st->fast;  // OpenLR2の拡張：FAST/SLOWの数
    case 211: return st->slow;
    case 290: case 291: {
        if (!chart || chart->scroll.empty()) return -1;
        double lo = 1e9, hi = 0;
        for (const ScrollSeg& s : chart->scroll) lo = std::min(lo, s.bpm), hi = std::max(hi, s.bpm);
        return static_cast<int>(id == 290 ? hi : lo);
    }
    default: return -1;
    }
}

bool PlayContext::bargraph(int type, double& val, double& max) const {
    const PlayStats* st = judge ? &judge->stats() : nullptr;
    switch (type) {
    case 1:
        val = song_ms;
        max = chart ? chart->end_us / 1000.0 : 0;
        return true;
    case 2:
        val = loaded;
        max = load_total;
        return true;
    default: break;
    }
    if (!st) return false;
    int n = st->total_notes;
    switch (type) {
    case 10: case 27: val = st->exscore(); max = n * 2; return true;
    case 11:
        if (st->note_current <= 0) return false;
        val = static_cast<double>(st->exscore() * n / st->note_current);
        max = n * 2;
        return true;
    case 20: val = st->judge_count[kPGreat]; max = n; return true;
    case 21: val = st->judge_count[kGreat]; max = n; return true;
    case 22: val = st->judge_count[kGood]; max = n; return true;
    case 23: val = st->judge_count[kBad]; max = n; return true;
    case 24: val = st->judge_count[kPoor]; max = n; return true;
    case 25: val = st->max_combo; max = n; return true;
    case 26: val = number(100); max = 200000; return true;
    default: return false;
    }
}

bool PlayContext::slider(int type, int& val, int& lo, int& hi) const {
    switch (type) {
    case 2: val = hi_speed, lo = 10, hi = 990; return true;
    case 4: val = lane_cover, lo = 0, hi = 100; return true;
    case 6:
        if (song_ms >= 0 && chart) val = static_cast<int>(song_ms), lo = 0, hi = static_cast<int>(chart->end_us / 1000);
        else val = 0, lo = 0, hi = 1;
        return true;
    default: val = 0, lo = 0, hi = 1; return true;
    }
}

std::string PlayContext::text(int id) const {
    if (!chart) return {};
    const ChartMeta& m = chart->meta;
    switch (id) {
    case 10: return m.title;
    case 11: return m.subtitle;
    case 12: return m.subtitle.empty() ? m.title : m.title + " " + m.subtitle;
    case 13: return m.genre;
    case 14: return m.artist;
    case 15: return m.subartist;
    default: return {};
    }
}

std::array<bool, 1000> PlayContext::load_options() const {
    std::array<bool, 1000> o{};
    for (int i = 0; i < 1000; ++i) o[i] = i < 900 && option(i);
    return o;
}

}  // namespace bmj
