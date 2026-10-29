#include "chart/chart.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace rd {

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

std::vector<Chart::Beat> Chart::metronome_beats() const {
    // 拍の数（曲の時刻 t までに進んだ拍）と、その逆（b 拍目の時刻）。STOP の間と BPM が 0 以下の間は進まない
    struct Piece {
        int64_t t0, t1;
        double b0, rate;  // rate は拍/マイクロ秒
    };
    std::vector<Piece> pieces;
    const int64_t end = std::max<int64_t>(end_us, 0);
    double b = 0;
    if (scroll.empty()) {
        pieces.push_back({0, end, 0, initial_bpm > 0 ? initial_bpm / 60e6 : 0});
    } else {
        if (scroll.front().time_us > 0) {
            const double r = initial_bpm > 0 ? initial_bpm / 60e6 : 0;
            pieces.push_back({0, scroll.front().time_us, 0, r});
            b = r * static_cast<double>(scroll.front().time_us);
        }
        for (size_t i = 0; i < scroll.size(); ++i) {
            const ScrollSeg& s = scroll[i];
            const int64_t t1 = i + 1 < scroll.size() ? scroll[i + 1].time_us : std::max(end, s.time_us);
            const double r = s.stop || s.bpm <= 0 ? 0.0 : s.bpm / 60e6;
            pieces.push_back({s.time_us, t1, b, r});
            b += r * static_cast<double>(t1 - s.time_us);
        }
    }
    auto beats_at = [&](int64_t t) {
        for (size_t i = pieces.size(); i-- > 0;)
            if (t >= pieces[i].t0) return pieces[i].b0 + pieces[i].rate * static_cast<double>(std::min(t, pieces[i].t1) - pieces[i].t0);
        return 0.0;
    };
    auto time_at = [&](double beat) -> int64_t {
        for (const Piece& p : pieces) {
            const double b1 = p.b0 + p.rate * static_cast<double>(p.t1 - p.t0);
            if (p.rate > 0 && beat < b1 + 1e-9) return p.t0 + static_cast<int64_t>(std::llround((beat - p.b0) / p.rate));
        }
        return end;
    };
    std::vector<Beat> out;
    auto add = [&](int64_t t, bool head) {
        if (t < 0 || t > end) return;
        if (!out.empty() && t - out.back().time_us < 30000) return;
        out.push_back({t, head});
    };
    constexpr size_t kMaxBeats = 20000;
    if (bars.empty()) {
        const double last = beats_at(end);
        for (int k = 0; k <= last + 1e-9 && out.size() < kMaxBeats; ++k) add(time_at(k), k % 4 == 0);
        return out;
    }
    std::vector<int64_t> heads;
    for (const BarLine& bl : bars) heads.push_back(bl.time_us);
    std::sort(heads.begin(), heads.end());
    heads.erase(std::unique(heads.begin(), heads.end()), heads.end());
    for (size_t i = 0; i < heads.size() && out.size() < kMaxBeats; ++i) {
        const double b0 = beats_at(heads[i]);
        const double b1 = i + 1 < heads.size() ? beats_at(heads[i + 1]) : beats_at(end) + 1e-6;
        add(heads[i], true);
        for (int k = 1; b0 + k < b1 - 1e-6 && out.size() < kMaxBeats; ++k) add(time_at(b0 + k), false);
    }
    return out;
}

double Chart::bpm_at(int64_t time_us) const {
    const ScrollSeg* s = seg_at(scroll, time_us);
    return s ? s->bpm : initial_bpm;
}

void Chart::drop_scratch() {
    std::vector<Note> keep;
    keep.reserve(notes.size());
    for (const Note& n : notes) {
        if (n.lane != 0) {
            keep.push_back(n);
            continue;
        }
        if (n.kind != NoteKind::Normal && n.kind != NoteKind::Long) continue;
        if (n.sound) bgm.push_back({n.time_us, n.sound});
        if (n.kind == NoteKind::Long && n.end_sound) bgm.push_back({n.end_us, n.end_sound});
    }
    notes = std::move(keep);
    layout = KeyLayout::K7;
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
    for (const BgaEvent& b : bga)
        if (b.layer != BgaEvent::Layer::Poor) sound_only = false;
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

int Chart::judgeable_notes(int ln_mode) const {
    int n = 0;
    for (const Note& note : notes) {
        if (note.kind == NoteKind::Normal) ++n;
        if (note.kind != NoteKind::Long) continue;
        int type = note.ln_type ? note.ln_type : ln_mode + 1;
        n += type >= 2 ? 2 : 1;
    }
    return n;
}

int Chart::free_ln_count() const {
    int n = 0;
    for (const Note& note : notes) n += note.kind == NoteKind::Long && note.ln_type == 0;
    return n;
}

int Chart::judgeable_notes() const {
    int n = 0;
    for (const Note& note : notes)
        if (note.kind == NoteKind::Normal || note.kind == NoteKind::Long) ++n;
    return n;
}

}  // namespace rd
