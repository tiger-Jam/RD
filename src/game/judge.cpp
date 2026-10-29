#include "game/judge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bmj {

JudgeWindows JudgeWindows::for_rank(int rank) {
    // #RANK ごとの PGREAT・GREAT・GOOD の幅（VERY HARD・HARD・NORMAL・EASY）。範囲の外は NORMAL
    struct Row {
        int pg, gr, gd;
    };
    static constexpr Row kRows[4] = {{8, 24, 40}, {15, 30, 60}, {18, 40, 100}, {21, 60, 120}};
    const Row& r = kRows[rank >= 0 && rank < 4 ? rank : 2];
    JudgeWindows w;
    w.pgreat = w.pgreat_late = r.pg;
    w.great = w.great_late = r.gr;
    w.good = w.good_late = r.gd;
    w.bad = w.bad_late = 200;
    w.empty_poor = 1000;
    return w;
}

int JudgeWindows::classify(int offset) const {
    if (offset >= 0) {
        if (offset <= pgreat_late) return kPGreat;
        if (offset <= great_late) return kGreat;
        if (offset <= good_late) return kGood;
        if (offset <= bad_late) return kBad;
        return -1;
    }
    int a = -offset;
    if (a <= pgreat) return kPGreat;
    if (a <= great) return kGreat;
    if (a <= good) return kGood;
    if (a <= bad) return kBad;
    return -1;
}

JudgeEngine::JudgeEngine(const Chart& chart, const JudgeConfig& cfg)
    : chart_(chart), cfg_(cfg), win_(JudgeWindows::for_rank(chart.judge_rank)), gauge_type_(cfg.gauge_type) {
    // カスタム判定（判定の MOD）。rank_scaled なら #RANK に合わせて、NORMAL に対する LR2 の比で伸び縮みさせる
    if (cfg.custom_judge) {
        const CustomJudgeDef& j = *cfg.custom_judge;
        const JudgeWindows base = JudgeWindows::for_rank(2), here = JudgeWindows::for_rank(chart.judge_rank);
        auto sc = [&](int v, int b, int h) { return j.rank_scaled ? static_cast<int>(std::lround(v * static_cast<double>(h) / b)) : v; };
        win_.pgreat = sc(j.pgreat[0], base.pgreat, here.pgreat), win_.pgreat_late = sc(j.pgreat[1], base.pgreat, here.pgreat);
        win_.great = sc(j.great[0], base.great, here.great), win_.great_late = sc(j.great[1], base.great, here.great);
        win_.good = sc(j.good[0], base.good, here.good), win_.good_late = sc(j.good[1], base.good, here.good);
        win_.bad = j.bad[0], win_.bad_late = j.bad[1];
        win_.empty_poor = j.empty_poor;
    }
    // 判定幅の倍率（bmson の judge_rank）。空POOR の幅はそのまま
    if (chart.judge_scale != 1.0 && chart.judge_scale > 0) {
        auto sc = [&](int& w) { w = std::max(1, static_cast<int>(std::lround(w * chart.judge_scale))); };
        sc(win_.pgreat), sc(win_.great), sc(win_.good), sc(win_.bad);
        sc(win_.pgreat_late), sc(win_.great_late), sc(win_.good_late), sc(win_.bad_late);
    }
    done_.assign(chart.notes.size(), false);
    phase_.assign(chart.notes.size(), LnPhase::Waiting);
    head_.assign(chart.notes.size(), 0);
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
    // カスタムゲージ：判定ごとの増減は「決まった値 + total × TOTAL ÷ 総ノーツ数」
    for (const CustomGaugeDef& g : cfg_.custom_gauges) {
        chp_.push_back(g.initial);
        std::array<double, 6> t{};
        for (int j = 0; j < 6; ++j)
            t[j] = g.delta[j].fixed + (stats_.total_notes > 0 ? g.delta[j].total * chart.total / stats_.total_notes : 0.0);
        ctable_.push_back(t);
    }
    cur_custom_ = cfg.custom_play;
    if (!cfg_.gas_chain.empty()) gas_set(cfg_.gas_chain[0]);
    if (cfg.course >= 0 && cfg.carry) {
        gauge_.set_hp(cfg.carry_hp);
        stats_.course_combo = cfg.carry_combo;
        stats_.course_max_combo = cfg.carry_max_combo;
    }
}

int JudgeEngine::clear() const {
    int c = custom_play();
    if (c >= 0) {
        // カスタムゲージで遊んだ：フルコンボはそのまま、クリアならそのゲージのランプ（本体やスキンには lamp_as で見せる）
        if (stats_.total_notes == stats_.max_combo) return stats_.judge_count[kGood] == 0 ? 6 : 5;
        return custom_cleared(c) ? std::clamp(cfg_.custom_gauges[c].lamp_as, 1, 4) : 1;
    }
    return clear_type(gauge_, gauge_type_, stats_.total_notes, stats_.note_current, stats_.max_combo,
                      stats_.judge_count[kGood], cfg_.course >= 0);
}

bool JudgeEngine::failed() const {
    int c = custom_play();
    if (c >= 0) return cfg_.custom_gauges[c].survival && chp_[c] < 2.0;
    return gauge_failed(gauge_, gauge_type_);
}

bool JudgeEngine::gas_entry_failed(int e) const {
    if (e < kGaugeCount) return gauge_failed(gauge_, e) && !(e == kGroove || e == kEasy);
    const CustomGaugeDef& g = cfg_.custom_gauges[e - kGaugeCount];
    return g.survival && chp_[e - kGaugeCount] < 2.0;
}

bool JudgeEngine::gas_entry_cleared(int e) const {
    if (e >= kGaugeCount) return custom_cleared(e - kGaugeCount);
    if (stats_.note_current < stats_.total_notes) return false;
    return (e == kGroove || e == kEasy) ? gauge_.hp(e) >= 80.0 : gauge_.hp(e) >= 2.0;
}

void JudgeEngine::gas_set(int e) {
    if (e < kGaugeCount) gauge_type_ = e, cur_custom_ = -1;
    else cur_custom_ = e - kGaugeCount;
}

void JudgeEngine::gas_step() {
    const std::vector<int>& c = cfg_.gas_chain;
    while (gas_pos_ + 1 < c.size() && gas_entry_failed(c[gas_pos_])) gas_set(c[++gas_pos_]);
}

void JudgeEngine::gas_settle() {
    const std::vector<int>& c = cfg_.gas_chain;
    for (size_t p = gas_pos_; p < c.size(); ++p)
        if (gas_entry_cleared(c[p])) {
            gas_pos_ = p;
            gas_set(c[p]);
            return;
        }
}

bool JudgeEngine::custom_cleared(int i) const {
    if (stats_.note_current < stats_.total_notes) return false;
    const CustomGaugeDef& g = cfg_.custom_gauges[i];
    return g.survival ? chp_[i] >= 2.0 : chp_[i] >= g.border;
}

void JudgeEngine::apply_judge(int judge, double mul) {
    gauge_.apply_judge(judge, mul);
    if (cfg_.gas && cfg_.gas_chain.empty()) gauge_type_ = gas_select(gauge_, cfg_.gauge_type);
    for (size_t i = 0; i < chp_.size(); ++i) {
        const CustomGaugeDef& g = cfg_.custom_gauges[i];
        double& hp = chp_[i];
        if (g.survival && hp <= 0) continue;  // 一度 0 になったら戻らない
        double d = ctable_[i][judge] * mul;
        if (d < 0 && hp <= g.low_hp) d *= g.low_hp_mul;
        hp = std::clamp(hp + d, 0.0, 100.0);
        if (!g.survival) hp = std::max(hp, g.floor);
        else if (hp < 2.0) hp = 0;
    }
    gas_step();
}

void JudgeEngine::apply_mine(int damage, int lane, int64_t t_us) {
    // LR2はここで POOR と空POOR の数を両方増やしている（BPにも入る）
    stats_.judge_count[kPoor]++;
    stats_.judge_count[kEmptyPoor]++;
    JudgeEvent e;
    e.time_us = t_us;
    e.lane = static_cast<uint8_t>(lane);
    e.mine = true;
    events_.push_back(e);
    gauge_.apply_mine(damage);
    for (size_t i = 0; i < chp_.size(); ++i) {
        const CustomGaugeDef& g = cfg_.custom_gauges[i];
        if (g.survival && chp_[i] <= 0) continue;
        chp_[i] = std::clamp(chp_[i] - damage * g.mine_mul, 0.0, 100.0);
        if (!g.survival) chp_[i] = std::max(chp_[i], g.floor);
        else if (chp_[i] < 2.0) chp_[i] = 0;
    }
    if (cfg_.gas && cfg_.gas_chain.empty()) gauge_type_ = gas_select(gauge_, cfg_.gauge_type);
    gas_step();
}

JudgeEvent JudgeEngine::make_event(int lane, int64_t t_us, int judge, int offset_ms, bool has_offset) const {
    JudgeEvent e;
    e.time_us = t_us;
    e.lane = static_cast<uint8_t>(lane);
    e.judge = judge;
    e.offset_ms = offset_ms;
    e.fast = offset_ms < 0;
    e.has_offset = has_offset;
    return e;
}

// 判定を記録に入れる：判定の数（空POOR は POOR にも数える）・ゲージ・コンボ・FAST/SLOW と、画面に出す出来事
void JudgeEngine::judge_to_score(int judge, int lane, int64_t t_us, int offset_ms, bool has_offset) {
    const bool empty = judge == kEmptyPoor;
    if (!empty) stats_.note_current++;
    stats_.judge_count[empty ? kPoor : judge]++;
    if (empty) stats_.empty_poor++;
    apply_judge(judge);
    // BAD・POOR はコンボを切り、GOOD 以上はつなぐ（空POOR はどちらでもない）
    auto extend = [](int& now, int& best) { best = std::max(best, ++now); };
    if (judge == kPoor || judge == kBad) {
        stats_.combo = stats_.course_combo = 0;
    } else if (judge >= kGood) {
        extend(stats_.combo, stats_.max_combo);
        extend(stats_.course_combo, stats_.course_max_combo);
    }
    const JudgeEvent e = make_event(lane, t_us, judge, offset_ms, has_offset);
    const bool timed = judge == kBad || judge == kGood || judge == kGreat;  // PGREAT と POOR は FAST/SLOW に数えない
    if (has_offset && timed) (e.fast ? stats_.fast : stats_.slow)++;
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
    const int timing = to_ms(t_us);
    for (int lane = 0; lane < kLaneCount; ++lane) {
        Lane& l = lanes_[lane];
        // ノーツが進むか判定の数が変わる間は、同じ時刻で次の先頭も見る
        while (l.cur < l.notes.size()) {
            if (cfg_.autoplay) {
                if (!auto_step(lane, t_us)) break;
                continue;
            }
            const size_t cur = l.cur;
            const int counted = stats_.note_current;
            sweep(lane, held_[lane], timing, t_us);
            if (l.cur == cur && stats_.note_current == counted) break;
        }
    }
}

// オートプレイ：時刻が来たノーツを PGREAT で取る。何もしなければ false
bool JudgeEngine::auto_step(int lane, int64_t t_us) {
    Lane& l = lanes_[lane];
    const size_t idx = l.notes[l.cur];
    const Note& n = chart_.notes[idx];
    if (is_charge(n) && phase_[idx] != LnPhase::Held) {
        // CN・HCN：始点と終点をそれぞれ PGREAT
        if (n.time_us > t_us) return false;
        judge_to_score(kPGreat, lane, n.time_us, 0, true);
        events_.back().ln_start = true;
        phase_[idx] = LnPhase::Held;
        if (ln_type(n) == 3) start_hcn(lane, idx, n.time_us);
        return true;
    }
    const int64_t due = n.kind == NoteKind::Long ? n.end_us : n.time_us;
    if (due > t_us) return false;
    if (n.kind != NoteKind::Mine) judge_to_score(kPGreat, lane, due, 0, true);
    l.note_val = n.sound;
    step_past(lane);
    return true;
}

void JudgeEngine::press(int lane, int64_t t_us) {
    if (lane < 0 || lane >= kLaneCount) return;
    advance(t_us);
    held_[lane] = true;
    if (cfg_.autoplay) return;  // オートプレイでは押しても何もしない（LR2: Play.cpp:1138）
    if (lanes_[lane].cur >= lanes_[lane].notes.size()) {
        play_key(lane);
        return;
    }
    const int timing = to_ms(t_us);
    while (hit(lane, timing, t_us)) {
    }
}

void JudgeEngine::release(int lane, int64_t t_us) {
    if (lane < 0 || lane >= kLaneCount) return;
    advance(t_us);
    held_[lane] = false;
    if (cfg_.autoplay) return;
    if (lanes_[lane].cur >= lanes_[lane].notes.size()) return;
    lift(lane, to_ms(t_us), t_us);
}

namespace {
// ノーツの時刻（ms）。判定のずれは切り捨てた整数で、見逃しは小数のまま比べる
double exact_ms(int64_t us) { return us / 1000.0; }
int whole_ms(int64_t us) { return static_cast<int>(us / 1000.0); }
}  // namespace

JudgeEngine::Front JudgeEngine::front_of(const Note& n) const {
    if (is_charge(n)) return Front::Charge;
    if (n.kind == NoteKind::Long) return Front::Ln;
    return n.kind == NoteKind::Mine ? Front::Mine : Front::Tap;
}

void JudgeEngine::step_past(int lane) {
    Lane& l = lanes_[lane];
    done_[l.notes[l.cur]] = true;
    l.cur++;
}

void JudgeEngine::sound_on(int lane, uint16_t sound) {
    lanes_[lane].note_val = sound;
    play_key(lane);
}

void JudgeEngine::play_key(int lane) {
    if (lanes_[lane].note_val > 0) play_.push_back(lanes_[lane].note_val);
}

// 見逃し：POOR にして、レーンの音をそのノーツの音に替える
void JudgeEngine::miss(int lane, int64_t t_us) {
    Lane& l = lanes_[lane];
    judge_to_score(kPoor, lane, t_us, 0, false);
    l.note_val = chart_.notes[l.notes[l.cur]].sound;
    step_past(lane);
}

// CN・HCN の始点の見逃し：始点と終点の両方を POOR（HCN は押し直せばゲージが戻る）
void JudgeEngine::miss_charge(int lane, int64_t t_us) {
    Lane& l = lanes_[lane];
    const size_t idx = l.notes[l.cur];
    const Note& n = chart_.notes[idx];
    judge_to_score(kPoor, lane, t_us, 0, false);
    judge_to_score(kPoor, lane, t_us, 0, false);
    l.note_val = n.sound;
    if (ln_type(n) == 3) start_hcn(lane, idx, std::max(t_us, n.time_us));
    close_long(lane, LnPhase::Closed);
}

void JudgeEngine::close_long(int lane, LnPhase to) {
    const Lane& l = lanes_[lane];
    phase_[l.notes[l.cur]] = to;
    step_past(lane);
}

void JudgeEngine::explode(int lane, const Note& mine, int64_t t_us) {
    apply_mine(mine.damage, lane, t_us);
    const bool own_sound = !chart_.wav.empty() && !chart_.wav[0].empty();  // #WAV00
    play_.push_back(own_sound ? 0 : kMineSystemSound);
}

void JudgeEngine::start_hcn(int lane, size_t index, int64_t from_us) { hcn_[lane] = Hcn{true, index, from_us, 0, false}; }

void JudgeEngine::stray_press(int lane, const Note& n, int timing, int64_t t_us, bool may_count) {
    // 空POOR の幅の中なら空POOR にして、レーンの音をそのノーツの音に替える。鳴らすのはどちらでもレーンの音
    if (may_count && whole_ms(n.time_us) - timing < win_.empty_poor) {
        judge_to_score(kEmptyPoor, lane, t_us, 0, false);
        lanes_[lane].note_val = n.sound;
    }
    play_key(lane);
}

// 押していない・押し続けている間：見逃しと、地雷の通過と、LN の終わり
void JudgeEngine::sweep(int lane, bool held, int timing, int64_t t_us) {
    const size_t idx = lanes_[lane].notes[lanes_[lane].cur];
    const Note& n = chart_.notes[idx];
    const bool waiting = phase_[idx] != LnPhase::Held;
    switch (front_of(n)) {
    case Front::Mine:
        // 通り過ぎたら消す。そのとき押していれば爆発
        if (timing > exact_ms(n.time_us)) {
            if (held) explode(lane, n, t_us);
            step_past(lane);
        }
        break;
    case Front::Tap:
        if (counting() && timing - exact_ms(n.time_us) > win_.bad_late) miss(lane, t_us);
        break;
    case Front::Ln:
        // 始点は GOOD の幅を過ぎたら見逃し。押し続けて終点を過ぎたら、始点の判定で確定
        if (waiting) {
            if (timing - exact_ms(n.time_us) > win_.good_late) miss(lane, t_us);
        } else if (held && timing > whole_ms(n.end_us)) {
            judge_to_score(head_[idx], lane, t_us, 0, false);
            close_long(lane, LnPhase::Closed);
        }
        break;
    case Front::Charge:
        // 押し続けて、終点の時刻から BAD 幅を過ぎたら終点は POOR（押しっぱなしでは取れない）
        if (waiting) {
            if (counting() && timing - exact_ms(n.time_us) > win_.bad_late) miss_charge(lane, t_us);
        } else if (timing - whole_ms(n.end_us) > win_.bad_late) {
            judge_to_score(kPoor, lane, t_us, 0, false);
            close_long(lane, LnPhase::Closed);
        }
        break;
    }
}

bool JudgeEngine::hit(int lane, int timing, int64_t t_us) {
    Lane& l = lanes_[lane];
    const size_t idx = l.notes[l.cur];
    const Note& n = chart_.notes[idx];
    const int offset = timing - whole_ms(n.time_us);
    switch (front_of(n)) {
    case Front::Mine:
        // GREAT の早い側の幅に入ってから押すか、通り過ぎてから押したら爆発
        if (exact_ms(n.time_us) - timing < win_.great || timing > exact_ms(n.time_us)) {
            explode(lane, n, t_us);
            step_past(lane);
        }
        return false;
    case Front::Tap: {
        if (counting() && timing - exact_ms(n.time_us) > win_.bad_late) {
            miss(lane, t_us);
            return false;
        }
        const int j = demote(win_.classify(offset));
        if (j < 0 || !counting()) {
            stray_press(lane, n, timing, t_us, counting());
            return false;
        }
        judge_to_score(j, lane, t_us, offset, true);
        sound_on(lane, n.sound);
        step_past(lane);
        // BAD のあと、同じレーンの次のノーツも BAD の幅に入っていれば、同じ押下で判定する（LR2: Play.cpp:902）
        if (j != kBad || l.cur >= l.notes.size()) return false;
        return win_.classify(timing - whole_ms(chart_.notes[l.notes[l.cur]].time_us)) >= 0;
    }
    case Front::Ln: {
        if (phase_[idx] != LnPhase::Held && timing - exact_ms(n.time_us) > win_.good_late) {
            miss(lane, t_us);
            return false;
        }
        const int j = demote(win_.classify(offset));
        if (j < 0) {
            stray_press(lane, n, timing, t_us, true);
            return false;
        }
        // 始点ではまだ数えない（終点で、始点の判定として数える）。画面には始点の判定を judge + 100 で知らせる
        sound_on(lane, n.sound);
        phase_[idx] = LnPhase::Held;
        head_[idx] = j;
        const JudgeEvent e = make_event(lane, t_us, j + 100, offset, true);
        events_.push_back(e);
        if (j != kPGreat) (e.fast ? stats_.fast : stats_.slow)++;
        return false;
    }
    case Front::Charge: {
        if (phase_[idx] == LnPhase::Held) {
            sweep(lane, true, timing, t_us);
            return false;
        }
        if (counting() && timing - exact_ms(n.time_us) > win_.bad_late) {
            miss_charge(lane, t_us);
            return false;
        }
        const int j = demote(win_.classify(offset));
        if (j < 0 || !counting()) {
            stray_press(lane, n, timing, t_us, counting());
            return false;
        }
        judge_to_score(j, lane, t_us, offset, true);
        events_.back().ln_start = true;
        sound_on(lane, n.sound);
        phase_[idx] = LnPhase::Held;
        if (ln_type(n) == 3) start_hcn(lane, idx, std::max(t_us, n.time_us));
        return false;
    }
    }
    return false;
}

void JudgeEngine::lift(int lane, int timing, int64_t t_us) {
    Lane& l = lanes_[lane];
    const size_t idx = l.notes[l.cur];
    const Note& n = chart_.notes[idx];
    const Front f = front_of(n);
    // 始点を押さえていない LN・CN と、ほかのノーツは、時刻が進んだときと同じ（離しても爆発はしない）
    if ((f != Front::Ln && f != Front::Charge) || phase_[idx] != LnPhase::Held) {
        sweep(lane, false, timing, t_us);
        return;
    }
    if (f == Front::Ln) {
        // 終点より GOOD の幅より早く離したら BAD（音も止める）。それ以外は始点の判定で確定
        if (whole_ms(n.end_us) - timing > win_.good) {
            if (l.note_val > 0) stop_.push_back(l.note_val);
            judge_to_score(kBad, lane, t_us, 0, false);
        } else {
            judge_to_score(head_[idx], lane, t_us, 0, false);
            events_.back().ln_release = true;
        }
        close_long(lane, LnPhase::Closed);
        return;
    }
    // CN・HCN：離した時刻を終点の幅で判定。早く離して BAD・POOR なら音を止める
    const int early = whole_ms(n.end_us) - timing;
    int j = tail_judge(lane, early);
    if (j < 0) j = kPoor;
    if (early > 0 && j <= kBad && l.note_val > 0) stop_.push_back(l.note_val);
    judge_to_score(j, lane, t_us, -early, true);
    events_.back().ln_end = true;
    if (ln_type(n) == 3 && j >= kGood) hcn_[lane].tail_good = true;
    close_long(lane, LnPhase::Closed);
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

}  // namespace bmj
