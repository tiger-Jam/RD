#include "game/hispeed.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bmj {

namespace {
bool counts(const Note& n) { return n.kind != NoteKind::Invisible && n.kind != NoteKind::Mine; }

// ハイスピ 1.00・基本スクロールスピード 100% のとき、1 拍が 80（640x480 のスキンの座標。LR2 と同じ）
double px_per_beat(int hs, double base_speed) { return 480.0 / 600.0 * hs * base_speed / 100.0; }
}  // namespace

void note_bpm_range(const Chart& chart, double& lo, double& hi) {
    lo = hi = chart.initial_bpm;
    for (const Note& n : chart.notes) {
        if (!counts(n)) continue;
        double bpm = chart.bpm_at(n.time_us);
        lo = std::min(lo, bpm), hi = std::max(hi, bpm);
    }
}

double main_bpm(const Chart& chart) {
    std::map<double, int> per_bpm;
    for (const Note& n : chart.notes)
        if (counts(n)) per_bpm[chart.bpm_at(n.time_us)]++;
    double best = 0;
    int most = 0;
    for (const auto& [bpm, c] : per_bpm)
        if (c > most) most = c, best = bpm;
    return best;
}

double floating_base_bpm(const Chart& chart, int base) {
    double lo, hi;
    switch (base) {
    case 1: note_bpm_range(chart, lo, hi); return hi;
    case 2: note_bpm_range(chart, lo, hi); return lo;
    case 3: if (double m = main_bpm(chart); m > 0) return m; break;
    default: break;
    }
    return chart.initial_bpm;
}

double green_number(double lane_px, double visible, int hs, double bpm, double base_speed) {
    double speed = px_per_beat(hs, base_speed) * bpm / 60000.0;  // 1 ミリ秒あたり
    if (speed <= 0) return 0;
    return lane_px * visible / speed * 0.6;
}

int hs_for_green(double lane_px, double visible, double green, double bpm, double base_speed) {
    if (green <= 0 || bpm <= 0 || base_speed <= 0) return 100;
    // green = lane_px × visible × 60000 × 0.6 ÷ (0.8 × hs × base_speed/100 × bpm)
    double hs = lane_px * visible * 60000.0 * 0.6 / (green * 0.8 * base_speed / 100.0 * bpm);
    return std::clamp(static_cast<int>(std::lround(hs)), kFloatingHsMin, kFloatingHsMax);
}

}  // namespace bmj
