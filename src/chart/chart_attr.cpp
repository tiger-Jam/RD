#include "chart/chart_attr.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace rd {

namespace {

double clip(double v, double lo = 0.0, double hi = 1.0) { return std::max(lo, std::min(hi, v)); }
double round_to(double v, int digits) {
    const double k = std::pow(10.0, digits);
    return std::round(v * k) / k;
}
int attr_bit(int code) { return code > 0 ? 1 << (code - 1) : 0; }

struct Row {
    double t = 0;
    std::vector<int> lanes;
};

// 鍵盤（1〜7）だけの集合
std::set<int> key_set(const std::vector<int>& lanes) {
    std::set<int> s;
    for (int l : lanes)
        if (l >= 1 && l <= 7) s.insert(l);
    return s;
}

// 行の間隔から 16 分の間隔を推定する（30〜250 ms の最頻値。半分の間隔に十分な数があれば、そちらにする）
double estimate_grid(const std::vector<double>& intervals, double provisional_chord, double fallback) {
    std::map<int, int> hist;
    for (double iv : intervals)
        if (iv >= 0.030 && iv <= 0.250) hist[static_cast<int>(std::lround(iv * 500))]++;
    if (hist.empty()) return fallback;
    int mode_bin = hist.begin()->first, mode_count = 0;
    for (const auto& [b, n] : hist)
        if (n > mode_count) mode_bin = b, mode_count = n;
    const double half = mode_bin / 2.0;
    int half_mass = 0;
    for (int b : {static_cast<int>(half) - 1, static_cast<int>(half), static_cast<int>(half) + 1})
        if (auto it = hist.find(b); it != hist.end()) half_mass += it->second;
    const double refined_bpm = half > 0 ? 60.0 / ((half / 500.0) * 4.0) : 9999.0;
    if (half >= 15 && half_mass >= 0.40 * mode_count && provisional_chord < 2.1 && refined_bpm <= 252.0) return half / 500.0;
    return mode_bin / 500.0;
}

// 隣り合う 16 分の行の、3-3 / 4-2 系 / 5-1 系の割合（同じ鍵を含む組と、合わせて 6 鍵未満の組は数えない）
void randa_shapes(const std::vector<Row>& rows, double grid, ChartAttr& a) {
    if (grid <= 0 || rows.size() < 2) return;
    int c33 = 0, c42 = 0, c51 = 0, transitions = 0;
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        const double iv = rows[i + 1].t - rows[i].t;
        if (!(0.75 * grid <= iv && iv <= 1.30 * grid + 0.003)) continue;
        ++transitions;
        const std::set<int> as = key_set(rows[i].lanes), bs = key_set(rows[i + 1].lanes);
        if (as.empty() || bs.empty()) continue;
        bool shared = false;
        for (int l : as) shared = shared || bs.count(l);
        if (shared) continue;
        const int lo = static_cast<int>(std::min(as.size(), bs.size())), hi = static_cast<int>(std::max(as.size(), bs.size()));
        if (lo + hi < 6) continue;
        if (lo == 3 && hi == 3) ++c33;
        else if (hi == 4 && (lo == 2 || lo == 3)) ++c42;
        else if ((hi == 5 && (lo == 1 || lo == 2)) || (hi == 6 && lo == 1)) ++c51;
    }
    if (transitions <= 0) return;
    a.randa_33 = round_to(static_cast<double>(c33) / transitions, 4);
    a.randa_42 = round_to(static_cast<double>(c42) / transitions, 4);
    a.randa_51 = round_to(static_cast<double>(c51) / transitions, 4);
}

// 同じ形のまま 1 鍵ずつ同じ向きへ動く行（同じ向きが 2 回以上続いたもの）の割合
double staircase_rate(const std::vector<Row>& rows, double grid) {
    if (grid <= 0 || rows.size() < 3) return 0.0;
    std::vector<int> dirs;
    int eligible = 0;
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        const double iv = rows[i + 1].t - rows[i].t;
        if (!(0.75 * grid <= iv && iv <= 1.30 * grid + 0.003)) {
            dirs.push_back(0);
            continue;
        }
        const std::set<int> a = key_set(rows[i].lanes), b = key_set(rows[i + 1].lanes);
        if (a.empty() || b.empty()) {
            dirs.push_back(0);
            continue;
        }
        ++eligible;
        std::set<int> plus, minus;
        for (int l : a) {
            if (l + 1 <= 7) plus.insert(l + 1);
            if (l - 1 >= 1) minus.insert(l - 1);
        }
        if (plus.size() == a.size() && plus == b) dirs.push_back(1);
        else if (minus.size() == a.size() && minus == b) dirs.push_back(-1);
        else dirs.push_back(0);
    }
    if (eligible <= 0) return 0.0;
    int marked = 0;
    for (size_t i = 0; i < dirs.size();) {
        if (dirs[i] == 0) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < dirs.size() && dirs[j] == dirs[i]) ++j;
        if (j - i >= 2) marked += static_cast<int>(j - i);
        i = j;
    }
    return static_cast<double>(marked) / eligible;
}

// 傾向ごとの当てはまり（0〜1）
std::map<int, double> attr_scores(const ChartAttr& f) {
    const double grid = f.grid_bpm, chord = f.avg_chord, ge3 = f.chord_ge3, sub = f.subgrid_rate;
    const double micro = grid >= 140.0 ? f.micro_rate : 0.0;
    std::map<int, double> s;
    s[kAttrYubi] = clip((micro - 0.035) / 0.06) * clip((215.0 - grid) / 15.0);
    s[kAttrDelay] = std::max(clip((grid - 210.0) / 22.0) * clip((2.7 - chord) / 0.5), clip((sub - 0.10) / 0.13) * clip((2.5 - chord) / 0.6));
    s[kAttrUde] = clip((162.0 - grid) / 17.0) * std::max(clip((chord - 2.0) / 0.6), clip((ge3 - 0.25) / 0.25));
    s[kAttrRanda] = clip((grid - 143.0) / 8.0) * clip((235.0 - grid) / 10.0) * (1.0 - clip((micro - 0.04) / 0.07)) *
                    (1.0 - clip((sub - 0.18) / 0.10));
    return s;
}

// 主な傾向の決まり（上から順に試し、最初に当てはまったもの）
int primary_rule(int rule, const ChartAttr& f) {
    const double grid = f.grid_bpm, chord = f.avg_chord, ge3 = f.chord_ge3, micro = f.micro_rate, sub = f.subgrid_rate;
    const double m_chord = f.micro_chord, m_full = f.micro_full, full_alt = f.full_alt_rate, full_rate = f.full_rate;
    const double tail_grid = f.tail_grid_bpm, tail_chord = f.tail_chord, tail_sub = f.tail_sub;
    switch (rule) {
    case 0:  // 腕：厚い反復
        return grid <= 162.0 && chord >= 2.85 && (m_chord >= 0.26 || m_full >= 0.06 || (full_alt >= 0.3 && full_rate >= 0.03)) ? kAttrUde : 0;
    case 1:  // 指：微縦連
        return micro >= 0.075 && grid >= 148.0 && grid <= 205.0 ? kAttrYubi : 0;
    case 2:  // 指：中速帯の拡張
        return micro >= 0.055 && grid >= 148.0 && grid <= 205.0 && sub < 0.25 && f.micro_thin + m_chord + m_full >= 0.04 ? kAttrYubi : 0;
    case 3:  // 指：低速の微縦連
        return micro >= 0.065 && grid >= 140.0 && grid < 148.0 && m_chord <= 0.19 ? kAttrYubi : 0;
    case 4:  // 指：同時押しの多い中速
        return grid >= 165.0 && grid < 175.0 && chord >= 2.4 && ge3 >= 0.45 && sub < 0.08 && m_chord < 0.12 && micro >= 0.05 &&
                       m_chord + m_full >= 0.04
                   ? kAttrYubi
                   : 0;
    case 5: return grid >= 228.0 && chord <= 2.6 ? kAttrDelay : 0;                       // ディレイ：高速
    case 6: return sub >= 0.35 && chord <= 2.05 ? kAttrDelay : 0;                        // ディレイ：低速のズレ
    case 7: return sub >= 0.22 && chord <= 2.2 && grid >= 150.0 ? kAttrDelay : 0;        // ディレイ：混在
    case 8: return tail_sub >= 0.26 && sub >= 0.1 && chord <= 2.15 && grid >= 150.0 ? kAttrDelay : 0;  // ディレイ：終盤
    case 9: return grid >= 205.0 && chord <= 1.65 && sub >= 0.08 ? kAttrDelay : 0;       // ディレイ：薄い高速
    case 10:  // 腕：低速で厚い（終盤が乱打なら乱打）
    case 11: {  // 腕：低速で薄め（同上）
        const bool ok = rule == 10 ? grid <= 150.0 && (chord >= 2.5 || ge3 >= 0.45)
                                   : grid <= 140.0 && chord >= 1.9 && ge3 >= 0.22 && sub < 0.25 && micro < 0.06;
        if (!ok) return 0;
        return tail_grid >= 172.0 && tail_chord <= 2.45 ? kAttrRanda : kAttrUde;
    }
    case 12: return grid >= 175.0 && grid < 228.0 ? kAttrRanda : 0;                                  // 乱打：高速
    case 13: return grid >= 146.0 && grid < 175.0 && sub < 0.2 && micro < 0.075 ? kAttrRanda : 0;    // 乱打：中速
    case 14: return grid < 150.0 && chord >= 2.0 && ge3 >= 0.3 && micro < 0.06 ? kAttrUde : 0;       // 腕：低速の拡張
    case 15: return grid >= 128.0 && grid < 146.0 && chord < 1.85 && sub < 0.2 && micro < 0.06 ? kAttrRanda : 0;  // 乱打：薄い低速
    }
    return 0;
}

void classify(ChartAttr& f) {
    const double grid = f.grid_bpm, chord = f.avg_chord, ge3 = f.chord_ge3, micro = f.micro_rate, sub = f.subgrid_rate;
    int primary = 0;
    for (int rule = 0; rule <= 15 && primary == 0; ++rule) primary = primary_rule(rule, f);
    const std::map<int, double> scores = attr_scores(f);
    if (primary == 0) {
        int best = kAttrYubi;  // 同じ値なら、元と同じく指・ディレイ・腕・乱打の順で先のもの
        for (int code : {kAttrDelay, kAttrUde, kAttrRanda})
            if (scores.at(code) > scores.at(best)) best = code;
        if (scores.at(best) >= 0.35) primary = best;
        // 低速のディレイは、ズレが多く薄いときだけ。厚ければ腕、そうでなければ保留
        if (primary == kAttrDelay && grid < 145.0 && !(sub >= 0.35 && chord <= 2.05))
            primary = chord >= 2.0 && ge3 >= 0.30 && micro < 0.06 ? kAttrUde : kAttrUndecided;
    }
    double primary_score = primary ? scores.at(primary) : 0.0;
    if (primary && primary_score < 0.5) primary_score = 0.5;
    // 二番目の傾向
    int secondary = 0;
    double secondary_score = 0.0;
    // 並びは元と同じ（指・ディレイ・腕・乱打。同じ値なら先のもの）
    std::vector<std::pair<int, double>> order;
    for (int code : {kAttrYubi, kAttrDelay, kAttrUde, kAttrRanda}) order.push_back({code, scores.at(code)});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [code, sc] : order)
        if (code != primary && sc >= 0.35) {
            secondary = code, secondary_score = sc;
            break;
        }
    if (secondary == 0 && primary != 0) {
        if (primary == kAttrRanda && micro >= 0.02 && grid <= 210.0) secondary = kAttrYubi;
        else if (primary == kAttrRanda && sub >= 0.12) secondary = kAttrDelay;
        else if (primary == kAttrDelay && grid >= 175.0 && grid < 228.0 && sub < 0.35) secondary = kAttrRanda;
        else if (primary == kAttrUde && grid < 146.0 && (micro >= 0.10 || f.micro_chord >= 0.15)) secondary = kAttrYubi;
        else if (primary == kAttrUde && f.tail_grid_bpm >= 165.0 && f.tail_chord <= 2.6) secondary = kAttrRanda;
        else if (primary == kAttrYubi && chord >= 2.7) secondary = kAttrUde;
        if (secondary) secondary_score = std::max({secondary_score, scores.at(secondary), 0.35});
    }
    f.attr = primary, f.attr2 = secondary;
    f.conf = primary ? round_to(clip(primary_score - secondary_score * 0.5, 0.05, 1.0), 3) : 0.0;
    // 複合
    int mask = 0;
    for (const auto& [code, sc] : scores)
        if (sc >= 0.35) mask |= attr_bit(code);
    f.mask = mask | attr_bit(primary) | attr_bit(secondary);
    // 副分類
    f.sub = kSubNone;
    if (primary == kAttrRanda) f.sub = grid >= 195.0 ? kSubRandaHigh : kSubRandaMid;
    else if (primary == kAttrUde) f.sub = f.full_rate < 0.015 ? kSubUdeNoFull : f.full_alt_rate >= 0.5 ? kSubUdeFullAlt : kSubUdeFullMany;
    else if (primary == kAttrYubi) {
        if (f.long_jack_rate >= 0.008) f.sub = kSubYubiLongJack;
        else if (chord >= 2.4 || (micro > 0 && f.micro_chord >= 0.5 * micro)) f.sub = kSubYubiChord;
    } else if (primary == kAttrDelay && grid >= 228.0) {
        f.sub = f.stair_rate >= 0.18 ? kSubDelayUltraStair : kSubDelayUltraRanda;
    }
    // 反復練習に向かない
    const double dur = f.chart_seconds;
    f.practice_low = (dur >= 60.0 && f.recovery_sec / std::max(dur, 1.0) >= 0.40) || f.last_kill >= 2.0 ||
                     (f.avg_density > 0 && f.d10 / f.avg_density >= 2.3) || (f.stream_sec <= 10.0 && dur >= 60.0);
}

}  // namespace

ChartAttr analyze_chart_attr(const Chart& chart) {
    ChartAttr a;
    // 打つノーツ（通常と LN の始点。地雷・見えないノーツは数えない）
    std::vector<std::pair<double, int>> notes;
    for (const Note& n : chart.notes)
        if (n.kind == NoteKind::Normal || n.kind == NoteKind::Long) notes.push_back({n.time_us / 1e6, n.lane});
    std::stable_sort(notes.begin(), notes.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    const int total = static_cast<int>(notes.size());
    if (total < 50) return a;
    const double duration = notes.back().first - notes.front().first;
    if (duration <= 10.0) return a;

    // 1.5 ms 以内のノーツは同じ行
    std::vector<Row> rows;
    for (const auto& [t, lane] : notes) {
        if (!rows.empty() && t - rows.back().t <= 0.0015) rows.back().lanes.push_back(lane);
        else rows.push_back({t, {lane}});
    }
    const int row_count = static_cast<int>(rows.size());
    std::vector<double> row_times(row_count), intervals;
    std::vector<int> key_sizes(row_count);
    for (int i = 0; i < row_count; ++i) {
        row_times[i] = rows[i].t;
        key_sizes[i] = static_cast<int>(std::count_if(rows[i].lanes.begin(), rows[i].lanes.end(), [](int l) { return l != 0; }));
    }
    for (int i = 0; i + 1 < row_count; ++i) intervals.push_back(row_times[i + 1] - row_times[i]);
    if (intervals.empty()) return a;

    const double grid = estimate_grid(intervals, static_cast<double>(total) / row_count, 0.1);
    const double grid_bpm = 60.0 / (grid * 4.0);

    std::vector<double> times(total);
    for (int i = 0; i < total; ++i) times[i] = notes[i].first;
    auto peak = [&](double window) {
        double best = 0;
        for (int l = 0, r = 0; r < total; ++r) {
            while (times[r] - times[l] > window) ++l;
            best = std::max(best, (r - l + 1) / window);
        }
        return best;
    };
    const double avg_density = total / duration;
    int ge3 = 0;
    for (const Row& r : rows) ge3 += r.lanes.size() >= 3;
    const double avg_chord = static_cast<double>(total) / row_count;
    int sub_count = 0, on_count = 0;
    for (double iv : intervals) {
        sub_count += iv > 0.004 && iv < 0.7 * grid;
        on_count += iv >= 0.8 * grid && iv <= 1.25 * grid;
    }

    // 微縦連：同じ鍵盤の 2〜4 連打（間隔が 1.3 grid と 115 ms 以下、かつ 105 ms 以下）。5 連以上は長い縦連
    const double jack_gate = std::min(1.3 * grid, 0.115);
    std::map<int, std::vector<double>> lane_times;
    for (const auto& [t, lane] : notes) lane_times[lane].push_back(t);
    int micro_sum = 0, long_sum = 0, m_thin = 0, m_chord = 0, m_full = 0;
    auto row_of = [&](double t) {
        return std::max(0, static_cast<int>(std::upper_bound(row_times.begin(), row_times.end(), t + 1e-9) - row_times.begin()) - 1);
    };
    for (const auto& [lane, ts] : lane_times) {
        std::vector<double> run = {ts[0]};
        auto close = [&] {
            const int n = static_cast<int>(run.size());
            if (n >= 2 && n <= 4) {
                micro_sum += n;
                int max_size = 0;
                for (double t : run) max_size = std::max(max_size, key_sizes[row_of(t)]);
                (max_size >= 6 ? m_full : max_size >= 3 ? m_chord : m_thin) += n;
            } else if (n >= 5) {
                long_sum += n;
            }
        };
        for (size_t i = 1; i < ts.size(); ++i) {
            const double d = ts[i] - ts[i - 1];
            if (d <= jack_gate && d <= 0.105) run.push_back(ts[i]);
            else close(), run = {ts[i]};
        }
        close();
    }

    // いちばん長い発狂（行の間隔が 1.3 grid + 5 ms 以下で続く区間。0.3 秒を超える休みで切る）
    double stream_sec = 0, stream_last = 0;
    bool in_stream = false;
    double stream_start = 0;
    for (size_t i = 0; i < intervals.size(); ++i) {
        if (intervals[i] <= 1.3 * grid + 0.005) {
            if (!in_stream) stream_start = row_times[i], in_stream = true;
            stream_last = row_times[i + 1];
        } else if (intervals[i] > 0.3 && in_stream) {
            stream_sec = std::max(stream_sec, stream_last - stream_start), in_stream = false;
        }
    }
    if (in_stream) stream_sec = std::max(stream_sec, stream_last - stream_start);

    // ラス殺し（最後の 10% の密度 ÷ 全体の密度）と、回復地帯（2 秒ごとの密度が全体の 40% 未満の秒数。両端は除く）
    const double tail_start = notes.front().first + duration * 0.9;
    const int tail_notes = total - static_cast<int>(std::upper_bound(times.begin(), times.end(), tail_start) - times.begin());
    const double last_kill = (tail_notes / std::max(duration * 0.1, 1e-9)) / std::max(avg_density, 1e-9);
    std::map<int, int> bins;
    for (double t : times) bins[static_cast<int>((t - notes.front().first) / 2.0)]++;
    double recovery = 0;
    for (int b = 1; b < static_cast<int>(duration / 2.0); ++b)
        if ((bins.count(b) ? bins[b] : 0) / 2.0 < avg_density * 0.4) recovery += 2.0;

    // 全押し（皿を除いて 6 鍵以上の行）と、その前後が 2〜5 鍵の行（交互）
    int full = 0, alt = 0;
    for (int i = 0; i < row_count; ++i) {
        if (key_sizes[i] < 6) continue;
        ++full;
        std::vector<int> near;
        if (i > 0 && row_times[i] - row_times[i - 1] <= 1.5 * grid + 0.01) near.push_back(key_sizes[i - 1]);
        if (i + 1 < row_count && row_times[i + 1] - row_times[i] <= 1.5 * grid + 0.01) near.push_back(key_sizes[i + 1]);
        if (!near.empty() && std::all_of(near.begin(), near.end(), [](int s) { return s >= 2 && s <= 5; })) ++alt;
    }

    // 終盤（最後の 20 秒か全体の 25% の長いほう）の grid・厚さ・ズレ
    const double tail_span = std::max(20.0, duration * 0.25), tail_from = notes.back().first - tail_span;
    int tail_first = row_count - 1;
    for (int i = 0; i < row_count; ++i)
        if (row_times[i] >= tail_from) {
            tail_first = i;
            break;
        }
    std::vector<double> tail_iv;
    int tail_note_total = 0;
    for (int i = tail_first; i < row_count; ++i) {
        tail_note_total += static_cast<int>(rows[i].lanes.size());
        if (i + 1 < row_count) tail_iv.push_back(row_times[i + 1] - row_times[i]);
    }
    double tail_grid_bpm = grid_bpm, tail_chord = avg_chord, tail_sub = static_cast<double>(sub_count) / intervals.size();
    if (tail_iv.size() >= 20) {
        const int tail_rows = row_count - tail_first;
        const double tg = estimate_grid(tail_iv, static_cast<double>(tail_note_total) / tail_rows, grid);
        tail_grid_bpm = 60.0 / (tg * 4.0);
        tail_chord = static_cast<double>(tail_note_total) / tail_rows;
        int ts = 0;
        for (double iv : tail_iv) ts += iv > 0.004 && iv < 0.7 * tg;
        tail_sub = static_cast<double>(ts) / tail_iv.size();
    }

    // 元と同じ桁に丸めてから分類する（分類の境目を元と揃える）
    a.ok = true;
    a.total_notes = total;
    a.grid_bpm = round_to(grid_bpm, 1);
    a.avg_chord = round_to(avg_chord, 3);
    a.chord_ge3 = round_to(static_cast<double>(ge3) / row_count, 4);
    a.micro_rate = round_to(static_cast<double>(micro_sum) / total, 4);
    a.long_jack_rate = round_to(static_cast<double>(long_sum) / total, 4);
    a.subgrid_rate = round_to(static_cast<double>(sub_count) / intervals.size(), 4);
    a.ongrid_rate = round_to(static_cast<double>(on_count) / intervals.size(), 4);
    a.stream_sec = round_to(stream_sec, 1);
    a.d2 = round_to(peak(2.0), 2), a.d10 = round_to(peak(10.0), 2), a.d20 = round_to(peak(20.0), 2);
    a.avg_density = round_to(avg_density, 2);
    a.last_kill = round_to(last_kill, 3);
    a.recovery_sec = round_to(recovery, 1);
    a.chart_seconds = round_to(duration, 1);
    a.full_rate = round_to(static_cast<double>(full) / row_count, 4);
    a.full_alt_rate = full ? round_to(static_cast<double>(alt) / full, 3) : 0.0;
    a.micro_thin = round_to(static_cast<double>(m_thin) / total, 4);
    a.micro_chord = round_to(static_cast<double>(m_chord) / total, 4);
    a.micro_full = round_to(static_cast<double>(m_full) / total, 4);
    a.tail_grid_bpm = round_to(tail_grid_bpm, 1);
    a.tail_chord = round_to(tail_chord, 3);
    a.tail_sub = round_to(tail_sub, 4);
    randa_shapes(rows, grid, a);
    a.stair_rate = round_to(staircase_rate(rows, grid), 4);
    classify(a);
    return a;
}

}  // namespace rd
