// 判定エンジン（仕様書6章）。挙動はOpenLR2の ProcSinglenote / ProcLongnote（LR2/Scene04_Play.cpp:740-1150）に合わせている。
// 入力のタイムスタンプで判定し、描画フレームには依存しない。同じ入力列からは必ず同じ結果になる。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "chart/chart.h"
#include "game/gauge.h"

namespace bmj {

// 判定幅（ms）。LR2: bmsload:3810
struct JudgeWindows {
    int pgreat = 18, great = 40, good = 100, bad = 200, empty_poor = 1000;
    static JudgeWindows for_rank(int rank);
};

struct JudgeEvent {
    int64_t time_us = 0;
    uint8_t lane = 0;
    int judge = 0;     // Judge
    int offset_ms = 0; // 入力時刻 − ノーツ時刻（POORのときは0）
    bool fast = false;
    bool has_offset = false;
};

struct PlayStats {
    std::array<int, 6> judge_count{};  // LR2と同じく、空POORはPOOR（[1]）にも数える
    int empty_poor = 0;
    int fast = 0, slow = 0;
    int combo = 0, max_combo = 0;
    int note_current = 0;  // 判定済みのノーツ数
    int total_notes = 0;
    int exscore() const { return judge_count[kGreat] + judge_count[kPGreat] * 2; }
    int bp() const { return judge_count[kBad] + judge_count[kPoor]; }
};

struct JudgeConfig {
    int gauge_type = kGroove;
    bool gas = false;
    bool autoplay = false;
};

class JudgeEngine {
public:
    JudgeEngine(const Chart& chart, const JudgeConfig& cfg);

    // 時刻 t_us までの見逃し・LNの終端・地雷の通過を処理する。入力の前に必ずその時刻まで進める
    void advance(int64_t t_us);
    void press(int lane, int64_t t_us);
    void release(int lane, int64_t t_us);

    // 出力（呼び出し側が取り出して空にする）
    std::vector<uint16_t>& sounds_to_play() { return play_; }
    std::vector<uint16_t>& sounds_to_stop() { return stop_; }
    std::vector<JudgeEvent>& events() { return events_; }

    const PlayStats& stats() const { return stats_; }
    const GaugeSet& gauge() const { return gauge_; }
    int gauge_type() const { return gauge_type_; }
    bool failed() const { return gauge_failed(gauge_, gauge_type_); }
    bool finished() const { return stats_.note_current >= stats_.total_notes; }
    int clear() const;
    const JudgeWindows& windows() const { return win_; }

    // 描画用：レーンごとの、まだ判定していない先頭のノーツの添字（chart.notes 上）
    bool note_done(size_t index) const { return done_[index]; }
    // LNの始点を押して保持中か
    bool ln_holding(size_t index) const { return active_[index] > 0; }
    bool lane_held(int lane) const { return held_[lane]; }

private:
    struct Lane {
        std::vector<size_t> notes;  // chart.notes の添字（通常・LN・地雷）
        size_t cur = 0;
        uint16_t note_val = 0;      // 押したときに鳴らすキー音（LR2: noteVal）
    };

    void proc(int lane, int keypress, int timing_ms, int64_t t_us);
    void proc_single(int lane, int keypress, int timing_ms, int64_t t_us);
    void proc_long(int lane, int keypress, int timing_ms, int64_t t_us);
    void judge_to_score(int judge, int lane, int64_t t_us, int offset_ms, bool has_offset);
    void apply_judge(int judge);
    void apply_mine(int damage);

    const Chart& chart_;
    JudgeConfig cfg_;
    JudgeWindows win_;
    GaugeSet gauge_;
    int gauge_type_;
    PlayStats stats_;
    std::array<Lane, kLaneCount> lanes_;
    std::array<bool, kLaneCount> held_{};
    std::vector<bool> done_;
    std::vector<int> active_;  // LN始点の判定（LR2: note.active）
    std::vector<bool> head_fast_;
    std::vector<size_t> invisible_;
    size_t invisible_next_ = 0;
    std::vector<uint16_t> play_, stop_;
    std::vector<JudgeEvent> events_;
};

// LR2と同じく、整数msに切り捨てる（負の時刻は床関数）
inline int to_ms(int64_t t_us) {
    return static_cast<int>(t_us >= 0 ? t_us / 1000 : -((-t_us + 999) / 1000));
}

}  // namespace bmj
