#include "chart/chart.h"

#include <algorithm>
#include <map>

namespace bmj {

static const ScrollSeg* seg_at(const std::vector<ScrollSeg>& scroll, int64_t t) {
    if (scroll.empty()) return nullptr;
    auto it = std::upper_bound(scroll.begin(), scroll.end(), t,
                               [](int64_t v, const ScrollSeg& s) { return v < s.time_us; });
    if (it == scroll.begin()) return &scroll.front();
    return &*(it - 1);
}

double Chart::pos_at(int64_t time_us) const {
    const ScrollSeg* s = seg_at(scroll, time_us);
    if (!s) return 0.0;
    return s->pos + s->speed * static_cast<double>(time_us - s->time_us);
}

double Chart::bpm_at(int64_t time_us) const {
    const ScrollSeg* s = seg_at(scroll, time_us);
    return s ? s->bpm : initial_bpm;
}

void Chart::make_constant_speed() {
    constexpr double kSpeed = 150.0 / 60e6;  // BPM 150 の拍/マイクロ秒
    double pos = 0;
    for (size_t i = 0; i < scroll.size(); ++i) {
        ScrollSeg& s = scroll[i];
        if (i > 0) pos += scroll[i - 1].speed * static_cast<double>(s.time_us - scroll[i - 1].time_us);
        s.pos = pos;
        s.speed = s.stop || s.speed == 0 ? 0.0 : kSpeed;
    }
    for (Note& n : notes) {
        n.pos = pos_at(n.time_us);
        if (n.end_us >= 0) n.end_pos = pos_at(n.end_us);
    }
    for (BarLine& b : bars) b.pos = pos_at(b.time_us);
}

void Chart::finish_import() {
    double pos = 0;
    for (size_t i = 0; i < scroll.size(); ++i) {
        ScrollSeg& s = scroll[i];
        if (i > 0) pos += scroll[i - 1].speed * static_cast<double>(s.time_us - scroll[i - 1].time_us);
        s.pos = pos;
        s.speed = s.stop ? 0.0 : s.bpm / 60e6 * s.scroll;
    }
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.time_us < b.time_us; });
    for (Note& n : notes) {
        n.pos = pos_at(n.time_us);
        if (n.end_us >= 0) n.end_pos = pos_at(n.end_us);
    }
    for (BarLine& b : bars) b.pos = pos_at(b.time_us);
    std::stable_sort(bgm.begin(), bgm.end(), [](const BgmEvent& a, const BgmEvent& b) { return a.time_us < b.time_us; });
    std::stable_sort(bga.begin(), bga.end(), [](const BgaEvent& a, const BgaEvent& b) { return a.time_us < b.time_us; });
    // 主 BPM：ノーツが一番多い BPM（BMS と同じ）
    std::map<double, int> per_bpm;
    for (const Note& n : notes)
        if (n.kind != NoteKind::Invisible) per_bpm[bpm_at(n.time_us)]++;
    main_bpm = initial_bpm;
    int best = 0;
    for (auto& [bpm, cnt] : per_bpm)
        if (cnt > best) best = cnt, main_bpm = bpm;
    // TOTAL：未定義（1 未満）なら LR2 の既定式（LR2: bmsload:3790）
    int n = judgeable_notes();
    if (total < 1) {
        double t;
        if (n < 400) t = n / 5.0 + 200.0;
        else if (n < 600) t = (n - 400) / 2.5 + 280.0;
        else t = (n - 600) / 5.0 + 360.0;
        total = static_cast<int>(t * 0.8);
    }
    // 曲の終わり：最後のノーツ（LN なら終点）+2 秒と、最後の BGM の遅いほう（BMS と同じ）
    int64_t end = 0;
    for (const Note& note : notes) end = std::max(end, std::max(note.time_us, note.end_us) + 2000000);
    for (const BgmEvent& b : bgm) end = std::max(end, b.time_us);
    end_us = end;
}

int Chart::judgeable_notes() const {
    int n = 0;
    for (const Note& note : notes)
        if (note.kind == NoteKind::Normal || note.kind == NoteKind::Long) ++n;
    return n;
}

}  // namespace bmj
