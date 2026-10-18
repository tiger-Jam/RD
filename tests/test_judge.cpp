#include "chart/bms_parser.h"
#include "game/judge.h"
#include "test.h"

using namespace bmj;

static Chart chart_of(const std::string& s) {
    BmsParseOptions o;
    o.random_seed = 1;
    return parse_bms_text(s, o).chart;
}

static int64_t ms(int v) { return static_cast<int64_t>(v) * 1000; }

// BPM120・#RANK 2（NORMAL）：小節1の頭（2000ms）にレーン1のノーツ
static const char* kOne = "#BPM 120\n#RANK 2\n#TOTAL 200\n#WAV01 a.wav\n#00111:01\n";

TEST(judge_windows_normal) {
    Chart c = chart_of(kOne);
    struct Case {
        int offset, judge;
    } cases[] = {{0, kPGreat}, {18, kPGreat}, {-18, kPGreat}, {19, kGreat}, {40, kGreat},
                 {41, kGood},  {100, kGood},  {101, kBad},     {200, kBad}, {-200, kBad}};
    for (auto cs : cases) {
        JudgeEngine j(c, {});
        j.press(1, ms(2000 + cs.offset));
        CHECK_EQ(j.events().size(), size_t(1));
        if (!j.events().empty()) CHECK_EQ(j.events()[0].judge, cs.judge);
    }
}

TEST(judge_empty_poor_and_nothing) {
    Chart c = chart_of(kOne);
    {
        JudgeEngine j(c, {});
        j.press(1, ms(2000 - 500));  // BADより早く、1000ms以内 → 空POOR
        CHECK_EQ(j.events().size(), size_t(1));
        CHECK_EQ(j.events()[0].judge, kEmptyPoor);
        CHECK_EQ(j.stats().judge_count[kPoor], 1);  // 空POORはPOORにも数える
        CHECK_EQ(j.stats().note_current, 0);
        CHECK_EQ(j.stats().combo, 0);
    }
    {
        JudgeEngine j(c, {});
        j.press(1, ms(2000 - 1200));  // 1000msより前 → 何も起きない（音だけ鳴る）
        CHECK_EQ(j.events().size(), size_t(0));
        CHECK_EQ(j.sounds_to_play().size(), size_t(1));
    }
}

TEST(judge_miss_after_bad_window) {
    Chart c = chart_of(kOne);
    JudgeEngine j(c, {});
    j.advance(ms(2200));
    CHECK_EQ(j.stats().note_current, 0);
    j.advance(ms(2201));
    CHECK_EQ(j.stats().note_current, 1);
    CHECK_EQ(j.stats().judge_count[kPoor], 1);
    CHECK_EQ(j.stats().bp(), 1);
}

TEST(judge_ln_hold_and_release) {
    // 2000ms〜3000ms のLN
    Chart c = chart_of("#BPM 120\n#00151:0101\n");
    {
        JudgeEngine j(c, {});
        j.press(1, ms(2010));
        CHECK_EQ(j.stats().note_current, 0);  // 始点では数えない
        j.advance(ms(3001));
        CHECK_EQ(j.stats().note_current, 1);
        CHECK_EQ(j.stats().judge_count[kPGreat], 1);
    }
    {
        JudgeEngine j(c, {});
        j.press(1, ms(2030));
        j.release(1, ms(2500));  // 終点よりGOOD幅(100ms)以上早い → BAD
        CHECK_EQ(j.stats().judge_count[kBad], 1);
        CHECK_EQ(j.sounds_to_stop().size(), size_t(1));
    }
    {
        JudgeEngine j(c, {});
        j.press(1, ms(2030));
        j.release(1, ms(2950));  // GOOD幅以内 → 始点の判定（GREAT）
        CHECK_EQ(j.stats().judge_count[kGreat], 1);
    }
    {
        JudgeEngine j(c, {});
        j.advance(ms(2101));  // 始点はGOOD幅を過ぎたら見逃し
        CHECK_EQ(j.stats().judge_count[kPoor], 1);
    }
}

TEST(judge_bad_chains_to_next_note) {
    // 2000ms と 2125ms に連続。2140ms に押すと、1つ目がBADになり、続けて2つ目も同じ入力で判定する
    Chart c = chart_of("#BPM 120\n#00111:0100000000000000\n#00111:00010000000000000000000000000000\n");
    JudgeEngine j(c, {});
    j.press(1, ms(2150 - 10));  // 1つ目は+140ms でBAD、2つ目は+15ms → PGREAT
    CHECK_EQ(j.stats().judge_count[kBad], 1);
    CHECK_EQ(j.stats().judge_count[kPGreat], 1);
}

TEST(judge_mine) {
    Chart c = chart_of("#BPM 120\n#001D1:0A\n#00211:01\n");
    {
        JudgeEngine j(c, {});
        j.press(1, ms(1990));  // GREAT幅（40ms）の早い側より後に押した → 爆発
        CHECK(j.gauge().hp(kGroove) < 20.0);
        CHECK_EQ(j.stats().bp(), 1);  // LR2の挙動：地雷もBPに入る
    }
    {
        JudgeEngine j(c, {});
        j.press(1, ms(1900));  // 早すぎ → まだ何もしない
        j.advance(ms(2001));   // 押したまま通過 → 爆発
        CHECK(j.gauge().hp(kGroove) < 20.0);
    }
    {
        JudgeEngine j(c, {});
        j.advance(ms(2001));  // 押していない → 何もない
        CHECK_EQ(j.gauge().hp(kGroove), 20.0);
    }
}

TEST(judge_autoplay_full_combo) {
    Chart c = chart_of("#BPM 150\n#00111:01010101\n#00151:0101\n#00216:01\n");
    JudgeConfig cfg;
    cfg.autoplay = true;
    JudgeEngine j(c, cfg);
    for (int64_t t = 0; t <= c.end_us; t += 1000) j.advance(t);
    CHECK_EQ(j.stats().note_current, j.stats().total_notes);
    CHECK_EQ(j.stats().max_combo, j.stats().total_notes);
    CHECK_EQ(j.clear(), 6);  // 全部 PGREAT なので PERFECT
}

TEST(clear_perfect_and_full_combo) {
    GaugeSet g;
    g.init(300, 10);
    for (int i = 0; i < 10; ++i) g.apply_judge(kPGreat);
    CHECK_EQ(clear_type(g, kGroove, 10, 10, 10, 0), 6);  // 空POORがあってもコンボが切れていなければ PERFECT
    CHECK_EQ(clear_type(g, kGroove, 10, 10, 10, 1), 5);  // GOOD があれば FULL COMBO
    CHECK_EQ(clear_type(g, kHard, 10, 10, 9, 0), 4);
}

TEST(gauge_groove_values) {
    GaugeSet g;
    g.init(300, 100);  // T/N = 3.0
    g.apply_judge(kPGreat);
    CHECK(std::abs(g.hp(kGroove) - 23.0) < 1e-4);
    g.apply_judge(kGood);
    CHECK(std::abs(g.hp(kGroove) - 24.5) < 1e-4);
    g.apply_judge(kPoor);
    CHECK(std::abs(g.hp(kGroove) - 18.5) < 1e-4);
    // HARD：残り30%以下ではダメージ0.6倍
    GaugeSet h;
    h.init(300, 1000);  // dmg_notebase=1, recover=(220*0.125/2)=13 → 100/13=7.69 → dmg=max(10,7.69)/10=1
    for (int i = 0; i < 7; ++i) h.apply_judge(kPoor);  // 100 → 30
    CHECK(std::abs(h.hp(kHard) - 30.0) < 1e-6);
    h.apply_judge(kPoor);
    CHECK(std::abs(h.hp(kHard) - 24.0) < 1e-6);
}

TEST(judge_deterministic) {
    Chart c = chart_of("#BPM 150\n#00111:0101010101010101\n#00112:0001000100010001\n");
    auto run = [&] {
        JudgeEngine j(c, {});
        int64_t t = 1600000;
        for (int i = 0; i < 12; ++i) {
            j.press(1 + (i % 2), t + i * 99731);
            j.release(1 + (i % 2), t + i * 99731 + 30000);
        }
        j.advance(c.end_us);
        return j.stats().exscore() * 1000 + j.stats().bp();
    };
    CHECK_EQ(run(), run());
}

#include "game/ghost.h"

TEST(ghost_default_target_matches_lr2) {
    // LR2: SetDefaultGhost(90, 100) → PG 80 / GR 20。DealJudgeFromQueue は1回目では数えない
    Ghost g;
    g.set_default(90, 100);
    g.deal();
    CHECK_EQ(g.exscore(), 0);
    g.deal();  // (2 × 80) / 100 = 1、(2 × 20) / 100 = 0
    CHECK_EQ(g.exscore(), 2);
    for (int i = 0; i < 98; ++i) g.deal();
    CHECK_EQ(g.exscore(), 180);  // 100ノーツ目で 80×2 + 20
    Ghost aaa;
    aaa.set_default(88, 9);  // AAA：PG 7 / GR 2
    for (int i = 0; i < 9; ++i) aaa.deal();
    CHECK_EQ(aaa.exscore(), 16);
    Ghost none;  // 自己ベストが無いとき
    none.deal();
    CHECK_EQ(none.exscore(), 0);
}

TEST(gauge_course) {
    GaugeSet g;
    g.init(300, 100, 2);  // 段位
    CHECK_EQ(g.hp(kGroove), 100.0);  // コースでは GROOVE も 100% から
    g.apply_judge(kPoor);
    CHECK(std::abs(g.hp(kGroove) - 97.0) < 1e-4);
    // 2% で止まらず、0 まで落ちる
    std::array<double, kGaugeCount> low{};
    low.fill(3.0);
    g.set_hp(low);
    g.apply_judge(kPoor);  // 30% 以下なので 0.6 倍
    CHECK(std::abs(g.hp(kGroove) - 1.2) < 1e-4);
    CHECK(gauge_failed(g, kGroove));
    // コース中は 2% 残ればクリア
    GaugeSet h;
    h.init(300, 10, 0);
    h.apply_judge(kBad);
    CHECK_EQ(clear_type(h, kGroove, 10, 10, 5, 0, true), 3);
    CHECK_EQ(clear_type(h, kGroove, 10, 10, 5, 0, false), 3);
    GaugeSet s;
    s.init(300, 10);
    CHECK_EQ(s.hp(kGroove), 20.0);
}

TEST(chart_constant_speed) {
    // BPM 120 で始まり、小節 1 の頭で BPM 240 に変わる。小節 1・2 の頭にノーツ
    Chart c = chart_of("#BPM 120\n#00103:F0\n#00111:01\n#00211:01\n");
    CHECK_EQ(c.notes.size(), size_t(2));
    if (c.notes.size() != 2) return;
    // 普通は 1 小節 = 4 拍
    CHECK(std::abs((c.notes[1].pos - c.notes[0].pos) - 4.0) < 1e-6);
    c.make_constant_speed();
    // 一定速度では、位置は時刻に比例する（BPM 150：1 秒 = 2.5 拍）
    double dt_s = (c.notes[1].time_us - c.notes[0].time_us) / 1e6;
    CHECK(std::abs((c.notes[1].pos - c.notes[0].pos) - dt_s * 2.5) < 1e-6);
    CHECK(std::abs(c.notes[0].pos - 2.0 * 2.5) < 1e-6);  // 2 秒の位置
    CHECK(std::abs(c.pos_at(c.notes[1].time_us) - c.notes[1].pos) < 1e-9);
}

#include "game/hispeed.h"

TEST(hispeed_green_number) {
    // レーン 400、ハイスピ 1.00、BPM 150：1 拍 80 → 1 ミリ秒 0.2 → 2000 ms → 緑数字 1200
    CHECK(std::abs(green_number(400, 1.0, 100, 150, 100) - 1200.0) < 1e-6);
    // カバーで半分隠すと半分
    CHECK(std::abs(green_number(400, 0.5, 100, 150, 100) - 600.0) < 1e-6);
    // 逆算
    CHECK_EQ(hs_for_green(400, 1.0, 1200, 150, 100), 100);
    CHECK_EQ(hs_for_green(400, 1.0, 300, 150, 100), 400);
    // 範囲 0.50〜10.00 に収める
    CHECK_EQ(hs_for_green(400, 1.0, 1e6, 150, 100), kFloatingHsMin);
    CHECK_EQ(hs_for_green(400, 1.0, 1, 150, 100), kFloatingHsMax);
    // 基準の BPM
    Chart c = chart_of("#BPM 120\n#00103:F0\n#00111:01\n#00211:0101\n");
    CHECK_EQ(floating_base_bpm(c, 0), 120.0);
    CHECK_EQ(floating_base_bpm(c, 1), 240.0);
    CHECK_EQ(floating_base_bpm(c, 2), 120.0);
    CHECK_EQ(floating_base_bpm(c, 3), 240.0);  // ノーツが多いのは BPM 240
}

TEST(judge_no_good_no_great) {
    Chart c = chart_of(kOne);
    auto judge_at = [&](int t_ms, bool no_good, bool no_great) {
        JudgeConfig cfg;
        cfg.no_good = no_good, cfg.no_great = no_great;
        JudgeEngine j(c, cfg);
        j.advance(ms(t_ms));
        j.press(1, ms(t_ms));
        return j.events().empty() ? -1 : j.events().back().judge;
    };
    CHECK_EQ(judge_at(2070, false, false), int(kGood));
    CHECK_EQ(judge_at(2070, true, false), int(kBad));    // GOOD 無し
    CHECK_EQ(judge_at(2030, true, false), int(kGreat));
    CHECK_EQ(judge_at(2030, false, true), int(kBad));    // GREAT・GOOD 無し
    CHECK_EQ(judge_at(2010, false, true), int(kPGreat));
}
