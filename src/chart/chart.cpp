#include "chart/chart.h"

#include <algorithm>

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

int Chart::judgeable_notes() const {
    int n = 0;
    for (const Note& note : notes)
        if (note.kind == NoteKind::Normal || note.kind == NoteKind::Long) ++n;
    return n;
}

}  // namespace bmj
