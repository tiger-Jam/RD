#include "skin/rd_buttons.h"
#include "render/skin_play.h"

#include <algorithm>
#include <cstdlib>

namespace rd {

namespace {
// フリップ中の番号の読み替え：1P 側 lo1..hi1 と 2P 側（+d）を入れ替える
int flip_id(int id, int lo1, int hi1, int d) {
    if (id >= lo1 && id <= hi1) return id + d;
    if (id >= lo1 + d && id <= hi1 + d) return id - d;
    return id;
}
}  // namespace

bool PlayContext::option(int op) const {
    if (result_flip && in_result) {
        int a = op < 0 ? -op : op;
        a = flip_id(a, 300, 308, 10);  // ランク（300〜307）と EX 0（308）↔ 2P（310〜318）
        return option_raw(op < 0 ? -a : a);
    }
    return option_raw(op);
}

int PlayContext::number(int id) const {
    if (result_flip && in_result) id = flip_id(id, 100, 116, 20);  // スコア・判定数など ↔ 2P（120〜136）
    return number_raw(id);
}

bool PlayContext::bargraph(int type, double& val, double& max) const {
    if (result_flip && in_result) type = flip_id(type, 20, 27, 10);  // 判定の割合など ↔ 2P（30〜37）
    return bargraph_raw(type, val, max);
}

// スキンの条件の番号。負の番号は、その条件が偽のときに真（0 はいつも真）
bool PlayContext::option_raw(int option) const {
    const bool negated = option < 0;
    const bool value = option_value(negated ? -option : option);
    return negated ? !value : value;
}

bool PlayContext::option_value(int option) const {
    const PlayStats* st = stats();
    double hp = gauge_hp();
    int gtype = judge ? judge->gauge_type() : gauge_type;
    if (bool v; course_option(course, option, v)) return v;
    // コースでは GROOVE/EASY も赤いゲージ（42/44 は偽、43/45 は真）
    const bool normal_gauge = (gtype == kGroove || gtype == kEasy) && !course;
    auto rank_of = [](int ex, int notes) { return notes > 0 ? ex * 9 / (notes * 2) : 0; };
    switch (option) {
    case 0: return true;
    case 30: return bga_size == 0;  // BGA の大きさ NORMAL / EXTEND
    case 31: return bga_size == 1;
    case 32: return !autoplay;
    case 33: return autoplay;
    // ゴーストの位置（SILENT のときは出さない）
    case 34: return ghost_pos == 0 || silent;
    case 35: case 36: case 37: return ghost_pos == option - 34 && !silent;
    case 38: return !score_graph;  // スコアグラフ OFF / ON
    case 39: return score_graph;
    case 40: return !bga_shown;  // BGA OFF / ON
    case 41: return bga_shown;
    case 42: case 44: return normal_gauge;
    case 43: case 45: return !normal_gauge;
    // いまのステージ（280〜288 は 1〜9 ステージ目、289 は最後のステージ。最後のステージは 280 番台より優先。単曲でも 289 は真）
    case 280: case 281: case 282: case 283: case 284: case 285: case 286: case 287: case 288:
        return course_stage == option - 280 && course_stage != course_stages - 1;
    case 289: return course_stage == course_stages - 1;
    case 46: return true;
    case 47: return false;
    case 50: return true;              // オフライン
    case 80: return in_result || !timers.on(40);  // 読み込み中（リザルトではタイマー40が立たないので真）
    case 81: return !in_result && timers.on(40);
    case 54: return true;  // 皿の自動演奏 OFF / ON（RD には無い）
    case 55: return false;
    case 308: return st && st->exscore() == 0;  // このプレイの EX スコアが 0
    case 318: {  // 2P 側の EX が 0（ターゲット無しを含む）
        const Ghost* g = p2_ghost();
        return !g || g->exscore() == 0 || !st || st->total_notes == 0;
    }
    case 350: return !result_flip;  // リザルトのフリップ無効 / 有効
    case 351: return result_flip;
    case 622: return true;  // ゴーストバトルではない
    // リプレイ：82=記録も再生もしない 83=記録している 84=再生している
    case 82: return !replay && !replay_rec;
    case 83: return replay_rec;
    case 84: return replay;
    case 90: return result_clear >= 2;  // クリア
    case 91: return result_clear < 2;   // 失敗
    case 176: case 177: {
        // BPMが変わらない曲／変わる曲
        if (!chart || chart->scroll.empty()) return option == 176;
        double lo = 1e9, hi = 0;
        for (const ScrollSeg& s : chart->scroll) lo = std::min(lo, s.bpm), hi = std::max(hi, s.bpm);
        return (hi != lo) == (option == 177);
    }
    case 180: return chart && chart->judge_rank < 1;  // 判定ランク VERY HARD
    case 181: return chart && chart->judge_rank == 1;
    case 182: return chart && chart->judge_rank == 2;
    case 183: return chart && chart->judge_rank > 2;
    case 300: case 301: case 302: case 303: case 304: case 305: case 306: case 307:
    case 340: case 341: case 342: case 343: case 344: case 345: case 346: case 347: {
        // このプレイのランク（AAA..F）。総ノーツ数に対して（LR2: 300-307 / 340-347）
        if (!st || st->total_notes == 0) return false;
        int r = rank_of(st->exscore(), st->total_notes);
        int k = (option % 10) % 8;  // 0=AAA .. 7=F
        if (k == 0) return r >= 8;
        if (k == 7) return (st->exscore() > 0 && r <= 1);
        return r == 8 - k;
    }
    case 320: case 321: case 322: case 323: case 324: case 325: case 326: case 327: {
        // このプレイの前の自己ベストのランク
        if (old_total_notes == 0) return false;
        int r = rank_of(old_exscore, old_total_notes);
        int k = option - 320;
        if (k == 0) return r >= 8;
        if (k == 7) return r <= 1;
        return r == 8 - k;
    }
    case 330: return st && st->exscore() > old_exscore;    // スコア更新
    case 331: return st && st->max_combo > old_maxcombo;   // コンボ更新
    case 332: return st && old_minbp >= 0 && bp_now() < old_minbp;  // BP更新
    case 335: {
        // ランク更新
        if (!st || st->total_notes == 0 || old_total_notes == 0) return false;
        return rank_of(old_exscore, old_total_notes) < rank_of(st->exscore(), st->total_notes);
    }
    case 100: case 101: case 102: case 103: case 104: case 105:
        return skin_lamp(best_clear) == option - 100;  // この譜面の自己ベストのランプ
    case 150: case 151: case 152: case 153: case 154: case 155:
        return chart && chart->meta.difficulty == option - 150;  // 譜面の #DIFFICULTY
    case 190: return !has_stagefile;
    case 191: return has_stagefile;
    case 194: return !has_backbmp;
    case 195: return has_backbmp;
    // 対戦・ゴーストバトル・コース・レーンカバーの調整中は、単曲プレイでは偽
    case 270: return cover_adjusting && lane_cover_on;
    case 10: case 11: case 12: case 13: case 271: case 623:
        return false;
    case 160: return true;             // 7KEYS
    case 161: case 162: case 163: case 164: return false;
    case 200: case 201: case 202: case 203: case 204: case 205: case 206: case 207: {
        // 判定済みノーツ数に対するランク（AAA..F）
        if (!st || st->note_current == 0) return false;
        int r = rank_of(st->exscore(), st->note_current);
        int want = 8 - (option - 200);  // 200=AAA(8)
        if (option == 200) return r >= 8;
        if (option == 207) return r < 2;
        return (r < want + 1 && r > want - 1);
    }
    case 210: case 211: case 212: case 213: case 214: case 215: case 216: case 217: {
        // 2P 側（ゴースト）の判定済みノーツ数に対するランク
        const Ghost* g = p2_ghost();
        if (!g || g->now_note() == 0) return false;
        int r = rank_of(g->exscore(), g->now_note());
        if (option == 210) return r >= 8;
        if (option == 217) return r < 2;
        return r == 8 - (option - 210);
    }
    case 310: case 311: case 312: case 313: case 314: case 315: case 316: case 317: {
        // リザルトの 2P 側（ゴースト）のランク（総ノーツ数に対して）
        const Ghost* g = p2_ghost();
        int n = st ? st->total_notes : 0;
        if (!g || n == 0) return false;
        int r = rank_of(g->exscore(), n);
        if (option == 310) return r >= 8;
        if (option == 317) return g->exscore() > 0 && r <= 1;
        return r == 8 - (option - 310);
    }
    case 220: case 221: case 222: case 223: case 224: case 225: case 226: case 227: {
        // 総ノーツ数に対するランク（以上）
        if (!st || st->total_notes == 0) return false;
        int r = rank_of(st->exscore(), st->total_notes);
        if (option == 220) return r >= 8;
        if (option == 227) return !(r > 1);
        return r > 6 - (option - 221);
    }
    case 230: case 231: case 232: case 233: case 234: case 235: case 236: case 237: case 238: case 239: {
        double lo = (option - 230) * 10.0, hi = lo + 10.0;
        if (hp < lo) return false;
        return hp < hi;
    }
    case 240: return hp >= 100.0;
    case 241: case 242: case 243: case 244: case 245: case 246:
        return judge_draw == 5 - (option - 241);
    case 247: return now_ms - last_miss_ms >= 1000.0;
    case 248: return !(now_ms - last_miss_ms >= 1000.0);
    default:
        if (option < 1000 && skin) return skin->op[option];
        return false;
    }
}

// LR2: SetObjectValue_Num（skinobject:1643）。知らない番号は -1、知っている番号で値が無いものは 0
int PlayContext::number_raw(int id) const {
    switch (id) {
    case 10: case 11: return hi_speed;
    case 12: return judge_timing;
    case 13: return 90;  // defaulttarget
    case 14: case 15: return lane_cover;
    case 20: return fps;
    case 50: case 51: case 52: case 53: case 54: case 55: case 56: return sound_fx ? sound_fx->eq[id - 50] : 0;
    case 60: case 61: case 62: case 63: case 64: case 65:
        if (!sound_fx) return 0;
        return id % 2 == 0 ? sound_fx->fx[(id - 60) / 2].p1 : sound_fx->fx[(id - 60) / 2].p2;
    case 66: return sound_fx ? sound_fx->pitch : 0;
    case 312: return visible_ms;
    case 313: return green_number;
    case 314: case 421: case 422: return lift;  // 421・422 は OpenLR2 の LIFT
    default: break;
    }
    if (int v; course_number(course, id, v)) return v;
    if (!stats() || (!chart && !stats_override)) return id >= 100 && id < 300 ? 0 : -1;
    const PlayStats& st = *stats();
    int n = st.total_notes;
    // 2P側（通常のプレイではターゲットのゴースト）
    const Ghost* p2g = p2_ghost();
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
        // 同じフォルダの難易度ごとのレベル（無ければ -1）
        return diff_level[id - 45];
    case 90: case 290: case 91: case 291: {
        if (!chart) return -1;
        double lo = 1e9, hi = 0;
        for (const ScrollSeg& s : chart->scroll) lo = std::min(lo, s.bpm), hi = std::max(hi, s.bpm);
        int v = static_cast<int>(id == 90 || id == 290 ? hi : lo);
        return v ? v : -1;
    }
    case 100:
        if (score_print >= 0) return static_cast<int>(score_print);
        return score_of(st.judge_count[kGood], st.judge_count[kGreat], st.judge_count[kPGreat]);
    case 101: return st.exscore();
    case 102: return static_cast<int>(rate);
    case 103: return static_cast<int>(rate * 100.0) % 100;
    // コースのプレイ中は、コンボはコース通しの値（リザルトではそのステージの値）
    case 104: return course && !in_result ? st.course_combo : st.combo;
    case 105: return course && !in_result ? st.course_max_combo : st.max_combo;
    case 106: return n;
    case 107: return static_cast<int>(hp_print >= 0 ? hp_print : gauge_hp()) / 2 * 2;
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
    case 160: case 161: case 162: case 163: case 164:
        if (!chart) return 0;
        break;
    default: break;
    }
    // リザルトでは曲の時刻のタイマー（41）が立っていない（LR2 は画面ごとにタイマーを消す）ので、経過は 0、残りは曲の長さ
    const double played_ms = in_result ? -1.0 : song_ms;
    switch (id) {
    case 160: return static_cast<int>(chart->bpm_at(static_cast<int64_t>(std::max(0.0, song_ms) * 1000)));
    case 161: return played_ms / 1000 > 0 ? static_cast<int>(played_ms) / 1000 / 60 : 0;
    case 162: return played_ms / 1000 > 0 ? static_cast<int>(played_ms) / 1000 % 60 : 0;
    case 163: case 164: {
        int rest = static_cast<int>(chart->end_us / 1000 - std::max(0.0, played_ms));
        if (rest / 1000 <= 0) return 0;
        return id == 163 ? rest / 1000 / 60 : rest / 1000 % 60;
    }
    case 165: return load_total > 0 ? loaded * 100 / load_total : 0;
    // リザルト画面：このプレイの前の自己ベストとの比較（LR2: 170-178）
    case 170: return old_exscore;
    case 171: return st.exscore();
    case 172: return st.exscore() - old_exscore;
    case 173: return old_maxcombo;
    case 174: return st.max_combo;
    case 175: return st.max_combo - old_maxcombo;
    case 176: return old_minbp < 0 ? 0 : old_minbp;
    case 177: return bp_now();
    case 178: return bp_now() - std::max(0, old_minbp);
    // OpenLR2の拡張（FAST/SLOW）
    case 201: return last_offset_ms;
    case 210: return last_fast_slow;
    case 212: return st.fast;
    case 214: return st.slow;
    // コンボが切れた数（OpenLR2 の拡張）：BAD と見逃しの POOR（空POOR と地雷は入らない。地雷は judge_count[0] に数えてある）
    case 216: return st.judge_count[kBad] + st.judge_count[kPoor] - st.empty_poor - st.judge_count[kEmptyPoor];
    case 217: return n;
    case 218: return st.note_current;
    default: return -1;
    }
}

bool PlayContext::bargraph_raw(int type, double& val, double& max) const {
    const PlayStats* st = stats();
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
    case 13: val = best_exscore; max = n * 2; return true;  // 自己ベストの最終スコア
    case 14: val = target.exscore(); max = n * 2; return true;
    case 15: val = target.final_exscore(); max = n * 2; return true;
    case 20: val = st->judge_count[kPGreat]; max = n; return true;
    case 21: val = st->judge_count[kGreat]; max = n; return true;
    case 22: val = st->judge_count[kGood]; max = n; return true;
    case 23: val = st->judge_count[kBad]; max = n; return true;
    case 24: val = st->judge_count[kPoor]; max = n; return true;
    case 25: val = st->max_combo; max = n; return true;
    case 26: val = number_raw(100); max = 200000; return true;
    default: return false;
    }
}

bool PlayContext::slider(int type, int& val, int& lo, int& hi) const {
    switch (type) {
    case 2: val = hi_speed, lo = 10, hi = 990; return true;
    case 4: val = lane_cover_on ? lane_cover : 0, lo = 0, hi = 100; return true;
    case 27: case 28: val = lift, lo = 0, hi = 100; return true;  // LIFT（OpenLR2 の拡張）
    // イコライザ（10〜16）、エフェクタのパラメータ（20〜25）、ピッチ（26）
    case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        val = sound_fx ? sound_fx->eq[type - 10] : 0, lo = -12, hi = 12;
        return true;
    case 20: case 21: case 22: case 23: case 24: case 25: {
        const FxSlot* f = sound_fx ? &sound_fx->fx[(type - 20) / 2] : nullptr;
        val = f ? (type % 2 == 0 ? f->p1 : f->p2) : 0, lo = 0, hi = 100;
        return true;
    }
    case 26: val = sound_fx ? sound_fx->pitch : 0, lo = -12, hi = 12; return true;
    case 6:
        if (song_ms >= 0 && chart) val = static_cast<int>(song_ms), lo = 0, hi = static_cast<int>(chart->end_us / 1000);
        else val = 0, lo = 0, hi = 1;
        return true;
    default: val = 0, lo = 0, hi = 1; return true;
    }
}

std::string PlayContext::text(int id) const {
    if (std::string v; course_text(course, id, v)) return v;
    if (!chart) return {};
    const ChartMeta& m = chart->meta;
    switch (id) {
    case 1: return target.name().empty() ? "NO TARGET" : target.name();  // ターゲット名（LR2: 演奏の始めに入れる）
    case 10: return m.title;
    case 11: return m.subtitle;
    case 12: return m.subtitle.empty() ? m.title : m.title + " " + m.subtitle;
    case 13: return m.genre;
    case 14: return m.artist;
    case 15: return m.subartist;
    default: return {};
    }
}

// LR2: SetObjectValue_Button。プレイ画面で意味があるのは、まだ機能の無い設定ばかりなので 0（オフ）
int PlayContext::bp_now() const {
    if (!stats()) return 0;
    const PlayStats& st = *stats();
    // LR2: totalnotes - note_current + BAD + POOR（途中でやめたときは残りもBPに入る）
    return st.total_notes - st.note_current + st.judge_count[kBad] + st.judge_count[kPoor];
}

int PlayContext::button(int type) const {
    switch (type) {
    // ゲージの種類（カスタムゲージは、落ちるものを HARD、落ちないものを GROOVE として見せる）
    case 40: return judge ? judge->display_gauge_type() : gauge_type;
    case kClearLabelButton: return judge ? judge->display_gauge_type() : gauge_type;  // 今回のクリア（絵の選び方はゲージの種類と同じ）
    // エフェクタの種類・オンオフ・掛け先、イコライザ、ピッチ・方式
    case 20: case 21: case 22: return sound_fx ? sound_fx->fx[type - 20].type : 0;
    case 23: case 24: case 25: return sound_fx && sound_fx->fx[type - 23].on ? 1 : 0;
    case 26: case 27: case 28: return sound_fx ? sound_fx->fx[type - 26].target : 0;
    case 29: return sound_fx && sound_fx->eq_on ? 1 : 0;
    case 32: return sound_fx && sound_fx->pitch_on ? 1 : 0;
    case 33: return sound_fx ? sound_fx->pitch_type : 0;
    case 55: return hs_fix;  // ハイスピ固定の種類
    default: return 0;
    }
}

std::array<bool, 1000> PlayContext::load_options() const {
    std::array<bool, 1000> o{};
    for (int i = 0; i < 1000; ++i) o[i] = i < 900 && option(i);
    return o;
}

}  // namespace rd
