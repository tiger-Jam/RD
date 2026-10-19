#include "game/judge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bmj {

JudgeWindows JudgeWindows::for_rank(int rank) {
    JudgeWindows w;
    switch (rank) {
    case 0: w.pgreat = 8, w.great = 24, w.good = 40; break;
    case 1: w.pgreat = 15, w.great = 30, w.good = 60; break;
    case 3: w.pgreat = 21, w.great = 60, w.good = 120; break;
    default: w.pgreat = 18, w.great = 40, w.good = 100; break;  // 2とそれ以外
    }
    w.bad = 200;
    w.empty_poor = 1000;
    return w;
}

JudgeEngine::JudgeEngine(const Chart& chart, const JudgeConfig& cfg)
    : chart_(chart), cfg_(cfg), win_(JudgeWindows::for_rank(chart.judge_rank)), gauge_type_(cfg.gauge_type) {
    // 判定幅の倍率（bmson の judge_rank）。空POOR の幅はそのまま
    if (chart.judge_scale != 1.0 && chart.judge_scale > 0) {
        auto sc = [&](int& w) { w = std::max(1, static_cast<int>(std::lround(w * chart.judge_scale))); };
        sc(win_.pgreat), sc(win_.great), sc(win_.good), sc(win_.bad);
    }
    done_.assign(chart.notes.size(), false);
    active_.assign(chart.notes.size(), 0);
    head_fast_.assign(chart.notes.size(), false);
    for (size_t i = 0; i < chart.notes.size(); ++i) {
        const Note& n = chart.notes[i];
        if (n.kind == NoteKind::Invisible) {
            invisible_.push_back(i);
            continue;
        }
        lanes_[n.lane].notes.push_back(i);
    }
    // 押したときの音の初期値は、そのレーンの最初のノーツの音（LR2: bmsload:4012）
    for (Lane& l : lanes_)
        if (!l.notes.empty()) l.note_val = chart.notes[l.notes.front()].sound;
    stats_.total_notes = chart.judgeable_notes(cfg.ln_mode);
    // CN・HCN の終点の幅（beatoraja の 7KEYS の表。判定ランクで 25/50/75/100/125% に伸縮し、bmson の倍率も掛ける）
    {
        static const int pct[5] = {25, 50, 75, 100, 125};
        double k = pct[std::clamp(chart.judge_rank, 0, 4)] / 100.0 * (chart.judge_scale > 0 ? chart.judge_scale : 1.0);
        auto make = [&](int pg, int gr, int gd, int late, int early) {
            auto r = [&](int v) { return static_cast<int>(std::lround(v * k)); };
            return TailWindows{r(pg), r(gr), r(gd), r(late), r(early)};
        };
        tail_key_ = make(120, 160, 200, 280, 220);
        tail_scr_ = make(130, 170, 210, 290, 230);
    }
    gauge_.init(static_cast<int>(chart.total), stats_.total_notes, cfg.course);
    if (cfg.course >= 0 && cfg.carry) {
        gauge_.set_hp(cfg.carry_hp);
        stats_.course_combo = cfg.carry_combo;
        stats_.course_max_combo = cfg.carry_max_combo;
    }
}

int JudgeEngine::clear() const {
    return clear_type(gauge_, gauge_type_, stats_.total_notes, stats_.note_current, stats_.max_combo,
                      stats_.judge_count[kGood], cfg_.course >= 0);
}

void JudgeEngine::apply_judge(int judge, double mul) {
    gauge_.apply_judge(judge, mul);
    if (cfg_.gas) gauge_type_ = gas_select(gauge_, cfg_.gauge_type);
}

void JudgeEngine::apply_mine(int damage) {
    // LR2はここで POOR と空POOR の数を両方増やしている（BPにも入る）
    stats_.judge_count[kPoor]++;
    stats_.judge_count[kEmptyPoor]++;
    gauge_.apply_mine(damage);
    if (cfg_.gas) gauge_type_ = gas_select(gauge_, cfg_.gauge_type);
}

// LR2: JudgeToScore + ApplyJudgeNote
void JudgeEngine::judge_to_score(int judge, int lane, int64_t t_us, int offset_ms, bool has_offset) {
    if (judge >= kPoor) stats_.note_current++;
    if (judge == kEmptyPoor) {
        stats_.judge_count[kPoor]++;
        stats_.empty_poor++;
    } else {
        stats_.judge_count[judge]++;
    }
    apply_judge(judge);
    switch (judge) {
    case kPoor:
    case kBad: stats_.combo = 0, stats_.course_combo = 0; break;
    case kGood:
    case kGreat:
    case kPGreat:
        stats_.combo++;
        if (stats_.combo > stats_.max_combo) stats_.max_combo = stats_.combo;
        stats_.course_combo++;
        if (stats_.course_combo > stats_.course_max_combo) stats_.course_max_combo = stats_.course_combo;
        break;
    default: break;
    }
    JudgeEvent e;
    e.time_us = t_us;
    e.lane = static_cast<uint8_t>(lane);
    e.judge = judge;
    e.offset_ms = offset_ms;
    e.fast = offset_ms < 0;
    e.has_offset = has_offset;
    if (has_offset && judge != kPGreat && judge >= kBad) (e.fast ? stats_.fast : stats_.slow)++;
    events_.push_back(e);
}

// HCN の増減（beatoraja と同じく、始点の判定のあと、終点の時刻まで 200ms ごと）。
// 押していれば GREAT の 0.5 倍を足し、離していれば BAD の 0.5 倍を引く。判定数・コンボ・EX には入らない
void JudgeEngine::hcn_tick(int64_t t_us) {
    constexpr int64_t kStep = 200000;
    for (int lane = 0; lane < kLaneCount; ++lane) {
        Hcn& h = hcn_[lane];
        if (!h.on) continue;
        const Note& n = chart_.notes[h.idx];
        int64_t now = std::min(t_us, n.end_us);
        if (now > h.last_us) {
            int64_t dt = now - h.last_us;
            if (held_[lane] || h.tail_good || cfg_.autoplay) {
                h.counter_us += dt;
                while (h.counter_us > kStep) apply_judge(kGreat, 0.5), h.counter_us -= kStep;
            } else {
                h.counter_us -= dt;
                while (h.counter_us < -kStep) apply_judge(kBad, 0.5), h.counter_us += kStep;
            }
            h.last_us = now;
        }
        if (t_us >= n.end_us) h = Hcn{};
    }
}

void JudgeEngine::advance(int64_t t_us) {
    hcn_tick(t_us);
    // 不可視ノーツは、時刻が来たらそのレーンの「押したときの音」を差し替えるだけ（LR2: Play.cpp:1508）
    while (invisible_next_ < invisible_.size() && chart_.notes[invisible_[invisible_next_]].time_us < t_us) {
        const Note& n = chart_.notes[invisible_[invisible_next_++]];
        lanes_[n.lane].note_val = n.sound;
    }
    int timing = to_ms(t_us);
    for (int lane = 0; lane < kLaneCount; ++lane) {
        Lane& l = lanes_[lane];
        // 1回の処理でノーツが進んだら、次のノーツも同じ時刻で見直す
        for (;;) {
            if (l.cur >= l.notes.size()) break;
            size_t before = l.cur;
            int before_count = stats_.note_current;
            if (lane_auto(lane) && !cfg_.autoplay) {
                // 皿の自動演奏（LR2: Play.cpp の bmsobj_note.autoplay）。始点で音を鳴らし、終点（LN）で数だけ進める
                while (l.sound_cur < l.notes.size() && chart_.notes[l.notes[l.sound_cur]].time_us <= t_us) {
                    const Note& s = chart_.notes[l.notes[l.sound_cur++]];
                    if (s.kind != NoteKind::Mine && s.sound > 0) play_.push_back(s.sound);
                }
                const Note& n = chart_.notes[l.notes[l.cur]];
                int64_t due = n.kind == NoteKind::Long ? n.end_us : n.time_us;
                if (due > t_us) break;
                if (n.kind != NoteKind::Mine) stats_.note_current += is_charge(n) ? 2 : 1;
                done_[l.notes[l.cur]] = true;
                l.note_val = n.sound;
                l.cur++;
                continue;
            }
            if (cfg_.autoplay) {
                const Note& n = chart_.notes[l.notes[l.cur]];
                size_t idx = l.notes[l.cur];
                if (is_charge(n) && active_[idx] <= 0) {
                    // CN・HCN：始点と終点をそれぞれ PGREAT
                    if (n.time_us > t_us) break;
                    judge_to_score(kPGreat, lane, n.time_us, 0, true);
                    events_.back().ln_start = true;
                    active_[idx] = 1;
                    if (ln_type(n) == 3) hcn_[lane] = Hcn{true, idx, n.time_us, 0, false};
                    continue;
                }
                int64_t due = n.kind == NoteKind::Long ? n.end_us : n.time_us;
                if (due > t_us) break;
                if (n.kind != NoteKind::Mine) judge_to_score(kPGreat, lane, due, 0, true);
                done_[l.notes[l.cur]] = true;
                l.note_val = n.sound;
                l.cur++;
                continue;
            }
            proc(lane, held_[lane] ? 2 : 0, timing, t_us);
            if (l.cur == before && stats_.note_current == before_count) break;
        }
    }
}

void JudgeEngine::press(int lane, int64_t t_us) {
    if (lane < 0 || lane >= kLaneCount) return;
    advance(t_us);
    held_[lane] = true;
    if (cfg_.autoplay || lane_auto(lane)) return;  // 自動演奏のレーンは押しても何もしない（LR2: Play.cpp:1138）
    Lane& l = lanes_[lane];
    if (l.cur >= l.notes.size()) {
        if (l.note_val > 0) play_.push_back(l.note_val);
        return;
    }
    proc(lane, 1, to_ms(t_us), t_us);
}

void JudgeEngine::release(int lane, int64_t t_us) {
    if (lane < 0 || lane >= kLaneCount) return;
    advance(t_us);
    held_[lane] = false;
    if (cfg_.autoplay || lane_auto(lane)) return;
    Lane& l = lanes_[lane];
    if (l.cur >= l.notes.size()) return;
    proc(lane, 3, to_ms(t_us), t_us);
}

// keypress: 0=押していない, 1=押した, 2=押し続けている, 3=離した（LR2と同じ）
void JudgeEngine::proc(int lane, int keypress, int timing, int64_t t_us) {
    const Note& n = chart_.notes[lanes_[lane].notes[lanes_[lane].cur]];
    if (is_charge(n)) proc_charge(lane, keypress, timing, t_us);
    else if (n.kind == NoteKind::Long) proc_long(lane, keypress, timing, t_us);
    else proc_single(lane, keypress, timing, t_us);
}

void JudgeEngine::proc_single(int lane, int keypress, int timing, int64_t t_us) {
    Lane& l = lanes_[lane];
    size_t idx = l.notes[l.cur];
    const Note& note = chart_.notes[idx];
    double real = note.time_us / 1000.0;
    int note_ms = static_cast<int>(real);

    if (note.kind == NoteKind::Mine) {
        // 押した瞬間がGREATの早い側より後なら爆発。通り過ぎた時点で押していても爆発
        if (keypress == 1 && real - timing < win_.great) {
            apply_mine(note.damage);
            play_.push_back(!chart_.wav.empty() && !chart_.wav[0].empty() ? 0 : kMineSystemSound);
        } else if (timing <= real) {
            return;
        } else if (keypress == 1 || keypress == 2) {
            apply_mine(note.damage);
            play_.push_back(!chart_.wav.empty() && !chart_.wav[0].empty() ? 0 : kMineSystemSound);
        }
        done_[idx] = true;
        l.cur++;
        return;
    }

    bool remaining = stats_.note_current < stats_.total_notes;
    // 見逃し
    if (timing - real > win_.bad && remaining) {
        judge_to_score(kPoor, lane, t_us, 0, false);
        l.note_val = note.sound;
        done_[idx] = true;
        l.cur++;
        return;
    }
    if (keypress != 1) return;

    int offset = timing - note_ms;
    int gap = std::abs(offset);
    int judge = -1;
    if (gap <= win_.pgreat) judge = kPGreat;
    else if (gap <= win_.great) judge = kGreat;
    else if (gap <= win_.good) judge = kGood;
    else if (gap <= win_.bad) judge = kBad;
    judge = demote(judge);
    if (judge >= 0 && remaining) {
        judge_to_score(judge, lane, t_us, offset, true);
        l.note_val = note.sound;
        if (l.note_val > 0) play_.push_back(l.note_val);
        done_[idx] = true;
        l.cur++;
        // BADのあと、同じレーンの次のノーツもBAD幅に入っていれば、同じ入力で判定する（LR2: Play.cpp:902）
        if (judge == kBad && l.cur < l.notes.size()) {
            const Note& next = chart_.notes[l.notes[l.cur]];
            if (std::abs(timing - static_cast<int>(next.time_us / 1000.0)) <= win_.bad) proc(lane, 1, timing, t_us);
        }
        return;
    }
    if (note_ms - timing < win_.empty_poor && remaining) {
        judge_to_score(kEmptyPoor, lane, t_us, 0, false);
        l.note_val = note.sound;
        if (l.note_val > 0) play_.push_back(l.note_val);
        return;
    }
    if (l.note_val > 0) play_.push_back(l.note_val);
}

void JudgeEngine::proc_long(int lane, int keypress, int timing, int64_t t_us) {
    Lane& l = lanes_[lane];
    size_t idx = l.notes[l.cur];
    const Note& note = chart_.notes[idx];
    double real = note.time_us / 1000.0;
    int note_ms = static_cast<int>(real);
    int end_ms = static_cast<int>(note.end_us / 1000.0);

    // 始点の見逃し（LNはGOOD幅を過ぎたら）
    if (timing - real > win_.good && active_[idx] <= 0) {
        judge_to_score(kPoor, lane, t_us, 0, false);
        l.note_val = note.sound;
        done_[idx] = true;
        l.cur++;
        return;
    }

    if (keypress == 1) {
        int offset = timing - note_ms;
        int gap = std::abs(offset);
        int judge = -1;
        if (gap <= win_.pgreat) judge = kPGreat;
        else if (gap <= win_.great) judge = kGreat;
        else if (gap <= win_.good) judge = kGood;
        else if (gap <= win_.bad) judge = kBad;
        judge = demote(judge);
        if (judge >= 0) {
            // 始点ではまだ数えない。終点で始点の判定として数える
            l.note_val = note.sound;
            if (l.note_val > 0) play_.push_back(l.note_val);
            active_[idx] = judge;
            head_fast_[idx] = offset < 0;
            JudgeEvent e;
            e.time_us = t_us;
            e.lane = static_cast<uint8_t>(lane);
            e.judge = judge;
            e.offset_ms = offset;
            e.fast = offset < 0;
            e.has_offset = true;
            // 始点の表示用（スコアには入らない）。judge に +100 して区別する
            e.judge += 100;
            events_.push_back(e);
            if (judge != kPGreat && judge != kBad) (e.fast ? stats_.fast : stats_.slow)++;
            if (judge == kBad) (e.fast ? stats_.fast : stats_.slow)++;
            return;
        }
        if (note_ms - timing < win_.empty_poor) {
            judge_to_score(kEmptyPoor, lane, t_us, 0, false);
            l.note_val = note.sound;
            if (l.note_val > 0) play_.push_back(l.note_val);
            return;
        }
        if (l.note_val > 0) play_.push_back(l.note_val);
        return;
    }
    if (keypress == 2) {
        // 押し続けて終点を過ぎたら、始点の判定で確定
        if (end_ms < timing && active_[idx] > 0) {
            judge_to_score(active_[idx], lane, t_us, 0, false);
            active_[idx] = -1;
            done_[idx] = true;
            l.cur++;
        }
        return;
    }
    if (keypress == 3 && active_[idx] > 0) {
        // 終点よりGOOD幅以上早く離したらBAD（音も止める）
        if (win_.good + timing < end_ms) {
            if (l.note_val > 0) stop_.push_back(l.note_val);
            judge_to_score(kBad, lane, t_us, 0, false);
        } else {
            judge_to_score(active_[idx], lane, t_us, 0, false);
        }
        active_[idx] = -1;
        done_[idx] = true;
        l.cur++;
    }
}

int JudgeEngine::tail_judge(int lane, int dm) const {
    const TailWindows& w = lane == 0 ? tail_scr_ : tail_key_;
    int a = std::abs(dm), j = -1;
    if (a <= w.pg) j = kPGreat;
    else if (a <= w.gr) j = kGreat;
    else if (a <= w.gd) j = kGood;
    else if (dm > 0 ? dm <= w.bd_early : -dm <= w.bd_late) j = kBad;
    return demote(j);
}

// CN・HCN（beatoraja と同じ）。始点はその場で 1 つの判定として数え、終点は離した時刻で別に判定する
void JudgeEngine::proc_charge(int lane, int keypress, int timing, int64_t t_us) {
    Lane& l = lanes_[lane];
    size_t idx = l.notes[l.cur];
    const Note& note = chart_.notes[idx];
    double real = note.time_us / 1000.0;
    int note_ms = static_cast<int>(real);
    int end_ms = static_cast<int>(note.end_us / 1000.0);
    const bool hcn = ln_type(note) == 3;
    bool remaining = stats_.note_current < stats_.total_notes;
    auto finish = [&] {
        active_[idx] = -1;
        done_[idx] = true;
        l.cur++;
    };

    if (active_[idx] <= 0) {
        // 始点の見逃し：BAD 幅を過ぎたら、始点と終点の両方を POOR（HCN は押し直せばゲージが戻る）
        if (timing - real > win_.bad && remaining) {
            judge_to_score(kPoor, lane, t_us, 0, false);
            judge_to_score(kPoor, lane, t_us, 0, false);
            l.note_val = note.sound;
            if (hcn) hcn_[lane] = Hcn{true, idx, std::max(t_us, note.time_us), 0, false};
            finish();
            return;
        }
        if (keypress != 1) return;
        int offset = timing - note_ms;
        int gap = std::abs(offset);
        int judge = -1;
        if (gap <= win_.pgreat) judge = kPGreat;
        else if (gap <= win_.great) judge = kGreat;
        else if (gap <= win_.good) judge = kGood;
        else if (gap <= win_.bad) judge = kBad;
        judge = demote(judge);
        if (judge >= 0 && remaining) {
            judge_to_score(judge, lane, t_us, offset, true);
            events_.back().ln_start = true;
            l.note_val = note.sound;
            if (l.note_val > 0) play_.push_back(l.note_val);
            active_[idx] = 1;
            if (hcn) hcn_[lane] = Hcn{true, idx, std::max(t_us, note.time_us), 0, false};
            return;
        }
        if (note_ms - timing < win_.empty_poor && remaining) {
            judge_to_score(kEmptyPoor, lane, t_us, 0, false);
            l.note_val = note.sound;
        }
        if (l.note_val > 0) play_.push_back(l.note_val);
        return;
    }
    if (keypress == 3) {
        // 離した：終点の幅で判定。早く離して BAD・POOR なら音を止める
        int dm = end_ms - timing;
        int judge = tail_judge(lane, dm);
        if (judge < 0) judge = kPoor;
        if (dm > 0 && judge <= kBad && l.note_val > 0) stop_.push_back(l.note_val);
        judge_to_score(judge, lane, t_us, -dm, true);
        events_.back().ln_end = true;
        if (hcn && judge >= kGood) hcn_[lane].tail_good = true;
        finish();
        return;
    }
    // 押し続けて、終点の時刻から BAD 幅を過ぎたら終点は POOR（押しっぱなしでは取れない）
    if (timing - end_ms > win_.bad) {
        judge_to_score(kPoor, lane, t_us, 0, false);
        finish();
    }
}

}  // namespace bmj
