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

int Chart::judgeable_notes() const {
    int n = 0;
    for (const Note& note : notes)
        if (note.kind == NoteKind::Normal || note.kind == NoteKind::Long) ++n;
    return n;
}

}  // namespace bmj
