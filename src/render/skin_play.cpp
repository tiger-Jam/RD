#include "render/skin_play.h"

#include <algorithm>
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

// LR2: SetObjectValue_Num（skinobject:1643）。知らない番号は -1、知っている番号で値が無いものは 0
int PlayContext::number(int id) const {
    switch (id) {
    case 10: case 11: return hi_speed;
    case 12: return judge_timing;
    case 13: return 90;  // defaulttarget
    case 14: case 15: return lane_cover;
    case 20: return fps;
    default: break;
    }
    if (!judge || !chart) return id >= 100 && id < 300 ? 0 : -1;
    const PlayStats& st = judge->stats();
    int n = st.total_notes;
    // 2P側（通常のプレイではターゲットのゴースト）
    const Ghost* p2g = target_type == 1 ? &best : target_type == 2 ? &target : nullptr;
    int p2_ex = p2g ? p2g->exscore() : 0;
    auto p2_judge = [&](int j) { return p2g ? p2g->judge(j) : 0; };
    auto score_of = [&](int gd, int gr, int pg) {
        return n > 0 ? static_cast<int>((gd + (gr + pg * 2) * 2) * 50000ull / n) : 0;
    };
    auto rank_diff = [&](int ex) {
        // 次のDJ LEVELまでの差（LR2: 109/154）
        if (ex == n * 2 || n == 0) return 0;
        int rank = std::clamp(ex * 9 / (n * 2), 1, 8);
        return ex - (n * 2 * (rank + 1) / 9);
    };
    double rate = st.note_current > 0 ? st.exscore() * 100 / static_cast<double>(st.note_current * 2) : 0;
    double p2_rate = n > 0 ? p2_ex * 100 / static_cast<double>(n * 2) : 0;
    switch (id) {
    case 45: case 46: case 47: case 48: case 49:
        // 同じ曲の難易度ごとのレベル。曲の一覧がまだ無いので、この譜面の難易度のところだけ
        return chart->meta.difficulty == id - 44 ? chart->meta.playlevel : 0;
    case 90: case 290: case 91: case 291: {
        double lo = 1e9, hi = 0;
        for (const ScrollSeg& s : chart->scroll) lo = std::min(lo, s.bpm), hi = std::max(hi, s.bpm);
        int v = static_cast<int>(id == 90 || id == 290 ? hi : lo);
        return v ? v : -1;
    }
    case 100: return score_of(st.judge_count[kGood], st.judge_count[kGreat], st.judge_count[kPGreat]);
    case 101: return st.exscore();
    case 102: return static_cast<int>(rate);
    case 103: return static_cast<int>(rate * 100.0) % 100;
    case 104: return st.combo;
    case 105: return st.max_combo;
    case 106: return n;
    case 107: return static_cast<int>(judge->gauge().hp(judge->gauge_type())) / 2 * 2;
    case 108: return st.exscore() - p2_ex;
    case 109: case 154: return rank_diff(st.exscore());
    case 110: return st.judge_count[kPGreat];
    case 111: return st.judge_count[kGreat];
    case 112: return st.judge_count[kGood];
    case 113: return st.judge_count[kBad];
    case 114: return st.judge_count[kPoor] + st.judge_count[kEmptyPoor];
    case 115: return n > 0 ? st.exscore() * 100 / (n * 2) : 0;
    case 116: return n > 0 ? st.exscore() * 10000 / (n * 2) % 100 : 0;
    case 120: return score_of(p2_judge(3), p2_judge(4), p2_judge(5));
    case 121: return p2_ex;
    case 122: return static_cast<int>(p2_rate);
    case 123: return static_cast<int>(p2_rate * 100.0) % 100;
    case 124: return p2g ? p2g->now_note() : 0;
    case 125: return 0;
    case 126: return n;
    case 127: return 0;
    case 128: return p2_ex - st.exscore();
    case 129: return rank_diff(p2_ex);
    case 130: case 131: case 132: case 133: return p2_judge(5 - (id - 130));
    case 134: return p2_judge(0) + p2_judge(1);
    case 135: return n > 0 ? p2_ex * 100 / (n * 2) : 0;
    case 136: return n > 0 ? p2_ex * 10000 / (n * 2) % 100 : 0;
    case 150: return best.exscore();
    case 151: return target.exscore();
    case 152: return st.exscore() - best.exscore();
    case 153: return st.exscore() - target.exscore();
    case 155: return n > 0 ? best.exscore() * 100 / (n * 2) : 0;
    case 156: return n > 0 ? best.exscore() * 10000 / (n * 2) % 100 : 0;
    case 157: return n > 0 ? target.exscore() * 100 / (n * 2) : 0;
    case 158: return n > 0 ? target.exscore() * 10000 / (n * 2) % 100 : 0;
    case 160: return static_cast<int>(chart->bpm_at(static_cast<int64_t>(std::max(0.0, song_ms) * 1000)));
    case 161: return song_ms / 1000 > 0 ? static_cast<int>(song_ms) / 1000 / 60 : 0;
    case 162: return song_ms / 1000 > 0 ? static_cast<int>(song_ms) / 1000 % 60 : 0;
    case 163: case 164: {
        int rest = static_cast<int>(chart->end_us / 1000 - std::max(0.0, song_ms));
        if (rest / 1000 <= 0) return 0;
        return id == 163 ? rest / 1000 / 60 : rest / 1000 % 60;
    }
    case 165: return load_total > 0 ? loaded * 100 / load_total : 0;
    // OpenLR2の拡張（FAST/SLOW）
    case 201: return last_offset_ms;
    case 210: return last_fast_slow;
    case 212: return st.fast;
    case 214: return st.slow;
    case 216: return st.judge_count[kBad] + st.judge_count[kPoor];
    case 217: return n;
    case 218: return st.note_current;
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
    case 12: val = best.exscore(); max = n * 2; return true;
    case 13: val = 0; max = n * 2; return true;  // 自己ベストの最終スコア（スコアの保存がまだ無い）
    case 14: val = target.exscore(); max = n * 2; return true;
    case 15: val = target.total_notes() > 0 ? n * 2 * 0.9 : 0; max = n * 2; return true;
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
