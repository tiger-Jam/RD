// 判定エンジン（仕様書6章）。挙動はOpenLR2の ProcSinglenote / ProcLongnote（LR2/Scene04_Play.cpp:740-1150）に合わせている。
// 入力のタイムスタンプで判定し、描画フレームには依存しない。同じ入力列からは必ず同じ結果になる。
#pragma once

#include <array>
#include <optional>
#include <cstdint>
#include <vector>

#include "chart/chart.h"
#include "game/gauge.h"
#include "mod/mod.h"

namespace bmj {

// 地雷が爆発したが #WAV00 が無いとき、sounds_to_play() に入れる番号（呼び出し側がシステム音を鳴らす）
constexpr uint16_t kMineSystemSound = 0xFFFF;

// 判定幅（ms）。LR2: bmsload:3810
// 判定の幅（ms）。pgreat〜bad は早い側、*_late は遅い側（LR2 の幅は両側とも同じ。カスタム判定で分けられる）
struct JudgeWindows {
    int pgreat = 18, great = 40, good = 100, bad = 200, empty_poor = 1000;
    int pgreat_late = 18, great_late = 40, good_late = 100, bad_late = 200;
    static JudgeWindows for_rank(int rank);
    // ずれ（入力 − ノーツ、ms。正なら遅い）の判定。幅の外なら -1
    int classify(int offset) const;
};

struct JudgeEvent {
    int64_t time_us = 0;
    uint8_t lane = 0;
    int judge = 0;     // Judge
    int offset_ms = 0; // 入力時刻 − ノーツ時刻（POORのときは0）
    bool fast = false;
    bool has_offset = false;
    bool ln_start = false;  // CN・HCN の始点（押さえている間の演出を始める）
    bool ln_end = false;    // CN・HCN の終点（離した時刻で判定したもの）
    bool ln_release = false;  // LN を終点の手前で離して、始点の判定で確定したもの（ボムは出さない。LR2: Play.cpp:1108）
    bool mine = false;      // 地雷を踏んだ（判定には数えない。前の判定の絵をもう一度出す。LR2: Play.cpp:298）
};

struct PlayStats {
    std::array<int, 6> judge_count{};  // LR2と同じく、空POORはPOOR（[1]）にも数える
    int empty_poor = 0;
    int fast = 0, slow = 0;
    int combo = 0, max_combo = 0;
    // コース通しのコンボ（LR2: now_combo_course / max_combo_course）。単曲では combo / max_combo と同じ
    int course_combo = 0, course_max_combo = 0;
    int note_current = 0;  // 判定済みのノーツ数
    int total_notes = 0;
    int exscore() const { return judge_count[kGreat] + judge_count[kPGreat] * 2; }
    int bp() const { return judge_count[kBad] + judge_count[kPoor]; }
};

struct JudgeConfig {
    int gauge_type = kGroove;
    bool gas = false;
    bool autoplay = false;
    // コース（0 エキスパート 1 ノンストップ 2 段位、-1 は単曲）。2 ステージ目からは前のステージのゲージとコンボを引き継ぐ
    int course = -1;
    bool carry = false;
    std::array<double, kGaugeCount> carry_hp{};
    int carry_combo = 0, carry_max_combo = 0;
    // LN の種類（仕様書6章）。譜面で種類を決めていない LN に使う：0 = LN 1 = CN 2 = HCN
    int ln_mode = 0;
    // 難易度表のコースの制約：GOOD を無くす（no_good）、GREAT と GOOD を無くす（no_great）。無くした判定は BAD になる
    bool no_good = false, no_great = false;
    // カスタムゲージ（評価の MOD。仕様書14章）。全部を裏で計算し、custom_play が 0 以上ならそのゲージで遊ぶ（コースでは使わない）
    std::vector<CustomGaugeDef> custom_gauges;
    int custom_play = -1;
    // GAS の順番（GAUGE で「GAS」を選んだとき）。0〜5 は本体のゲージ、6 以上はカスタムゲージ（6 + custom_gauges の番号）。
    // 先頭のゲージで始め、尽きたら次へ移る。曲の終わりにクリアの線に届いていなければ、クリアできた一番上のゲージを取る
    std::vector<int> gas_chain;
    // カスタム判定（判定の MOD）。無ければ LR2 の判定幅
    std::optional<CustomJudgeDef> custom_judge;
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
    bool failed() const;
    // 遊んでいるゲージの値（カスタムゲージで遊んでいればそのゲージ）
    double play_hp() const { return custom_play() >= 0 ? chp_[custom_play()] : gauge_.hp(gauge_type_); }
    // スキンに見せるゲージの種類（カスタムゲージは、落ちるものを HARD、落ちないものを GROOVE として見せる）
    int display_gauge_type() const {
        if (custom_play() < 0) return gauge_type_;
        return cfg_.custom_gauges[custom_play()].survival ? kHard : kGroove;
    }
    // カスタムゲージ
    int custom_play() const { return cfg_.course < 0 ? cur_custom_ : -1; }
    // GAS：曲の終わりに、クリアできた一番上のゲージに決める（リザルトの前に呼ぶ）
    void gas_settle();
    int custom_count() const { return static_cast<int>(chp_.size()); }
    double custom_hp(int i) const { return chp_[i]; }
    // そのカスタムゲージで、曲の終わりにクリアの条件を満たしているか
    bool custom_cleared(int i) const;
    const CustomGaugeDef& custom_def(int i) const { return cfg_.custom_gauges[i]; }
    bool in_course() const { return cfg_.course >= 0; }
    bool finished() const { return stats_.note_current >= stats_.total_notes; }
    int clear() const;
    const JudgeWindows& windows() const { return win_; }

    // 描画用：レーンごとの、まだ判定していない先頭のノーツの添字（chart.notes 上）
    bool note_done(size_t index) const { return done_[index]; }
    // LNの始点を押して保持中か
    bool ln_holding(size_t index) const { return active_[index] > 0; }
    bool lane_held(int lane) const { return held_[lane]; }
    // そのレーンでLNを押し続けている最中か
    bool lane_ln_holding(int lane) const {
        const Lane& l = lanes_[lane];
        return l.cur < l.notes.size() && active_[l.notes[l.cur]] > 0;
    }

private:
    struct Lane {
        std::vector<size_t> notes;  // chart.notes の添字（通常・LN・地雷）
        size_t cur = 0;
        uint16_t note_val = 0;      // 押したときに鳴らすキー音（LR2: noteVal）
    };
    int demote(int judge) const {
        if (cfg_.no_great && (judge == kGreat || judge == kGood)) return kBad;
        if (cfg_.no_good && judge == kGood) return kBad;
        return judge;
    }

    void proc(int lane, int keypress, int timing_ms, int64_t t_us);
    void proc_single(int lane, int keypress, int timing_ms, int64_t t_us);
    void proc_long(int lane, int keypress, int timing_ms, int64_t t_us);
    void proc_charge(int lane, int keypress, int timing_ms, int64_t t_us);
    // LN の種類（1 = LN 2 = CN 3 = HCN）
    int ln_type(const Note& n) const { return n.ln_type ? n.ln_type : cfg_.ln_mode + 1; }
    bool is_charge(const Note& n) const { return n.kind == NoteKind::Long && ln_type(n) >= 2; }
    // CN・HCN の終点の判定（終点 − 離した時刻、ms。正なら早い）。-1 は幅の外
    int tail_judge(int lane, int dm) const;
    void hcn_tick(int64_t t_us);
    void judge_to_score(int judge, int lane, int64_t t_us, int offset_ms, bool has_offset);
    void apply_judge(int judge, double mul = 1.0);
    void apply_mine(int damage, int lane, int64_t t_us);

    const Chart& chart_;
    JudgeConfig cfg_;
    JudgeWindows win_;
    GaugeSet gauge_;
    int gauge_type_;
    int cur_custom_ = -1;   // 遊んでいるカスタムゲージ（GAS で移ることがある）
    size_t gas_pos_ = 0;    // GAS の順番の、いまの位置
    bool gas_entry_failed(int e) const;
    bool gas_entry_cleared(int e) const;
    void gas_set(int e);
    void gas_step();
    std::vector<double> chp_;                    // カスタムゲージの値
    std::vector<std::array<double, 6>> ctable_;  // カスタムゲージの判定ごとの増減
    PlayStats stats_;
    std::array<Lane, kLaneCount> lanes_;
    std::array<bool, kLaneCount> held_{};
    // CN・HCN の終点の幅（ms）。鍵盤と皿で別（beatoraja の表を判定ランクで伸縮）
    struct TailWindows {
        int pg, gr, gd, bd_late, bd_early;
    } tail_key_{}, tail_scr_{};
    // HCN：レーンごとの、通過中の HCN と増減のカウンタ（200ms ごとに増減）
    struct Hcn {
        bool on = false;
        size_t idx = 0;
        int64_t last_us = 0, counter_us = 0;
        bool tail_good = false;  // 終点が PG〜GD で決まった（押していなくても増やす側）
    };
    std::array<Hcn, kLaneCount> hcn_{};
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
