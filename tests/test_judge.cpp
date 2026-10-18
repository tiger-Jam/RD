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
