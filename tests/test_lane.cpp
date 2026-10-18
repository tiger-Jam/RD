#include <set>

#include "game/judge.h"
#include "game/lane_option.h"
#include "test.h"

using namespace bmj;

namespace {
Note note(int lane, int ms, int end_ms = -1) {
    Note n;
    n.lane = static_cast<uint8_t>(lane);
    n.time_us = ms * 1000LL;
    n.pos = ms / 500.0;
    if (end_ms >= 0) n.kind = NoteKind::Long, n.end_us = end_ms * 1000LL, n.end_pos = end_ms / 500.0;
    return n;
}
// 0..7 の各レーンに1つずつ、時刻をずらして置く
Chart staircase() {
    Chart c;
    for (int l = 0; l < 8; ++l) c.notes.push_back(note(l, 1000 + l * 100));
    return c;
}
}  // namespace

TEST(lane_mirror) {
    Chart c = staircase();
    Lr2Random rng(1);
    apply_random_option(c, kMirror, rng);
    for (const Note& n : c.notes) {
        int orig = static_cast<int>((n.time_us / 1000 - 1000) / 100);
        CHECK_EQ(n.lane, orig == 0 ? 0 : 8 - orig);  // 皿はそのまま
    }
}

TEST(lane_random_is_permutation) {
    // 同じレーンのノーツは同じ行き先へ。皿は動かない
    Chart c;
    for (int i = 0; i < 4; ++i)
        for (int l = 0; l < 8; ++l) c.notes.push_back(note(l, 1000 + i * 1000 + l * 10));
    Lr2Random rng(12345);
    apply_random_option(c, kRandom, rng);
    int dest[8];
    std::fill(std::begin(dest), std::end(dest), -1);
    std::set<int> used;
    for (const Note& n : c.notes) {
        int orig = static_cast<int>(n.time_us / 1000 % 1000 / 10);
        if (dest[orig] < 0) dest[orig] = n.lane, used.insert(n.lane);
        CHECK_EQ(dest[orig], n.lane);
    }
    CHECK_EQ(dest[0], 0);
    CHECK_EQ(static_cast<int>(used.size()), 8);
}

TEST(lane_srandom_chord_spreads) {
    // 7 鍵の同時押しは、S-RANDOM でも全部別の鍵盤に置く
    for (uint32_t seed = 1; seed < 30; ++seed) {
        Chart c;
        for (int l = 1; l <= 7; ++l) c.notes.push_back(note(l, 1000));
        Lr2Random rng(seed);
        apply_random_option(c, kSRandom, rng);
        std::set<int> lanes;
        for (const Note& n : c.notes) lanes.insert(n.lane);
        CHECK_EQ(static_cast<int>(lanes.size()), 7);
    }
}

TEST(lane_srandom_avoids_ln) {
    // LN を押している鍵盤には、その途中のノーツを置かない
    for (uint32_t seed = 1; seed < 30; ++seed) {
        Chart c;
        c.notes.push_back(note(1, 1000, 3000));
        for (int i = 0; i < 10; ++i) c.notes.push_back(note(2 + i % 5, 1100 + i * 150));
        Lr2Random rng(seed);
        apply_random_option(c, kSRandom, rng);
        int ln_lane = -1;
        for (const Note& n : c.notes)
            if (n.kind == NoteKind::Long) ln_lane = n.lane;
        for (const Note& n : c.notes)
            if (n.kind != NoteKind::Long) CHECK(n.lane != ln_lane);
        CHECK_EQ(static_cast<int>(c.notes.size()), 11);
    }
}

TEST(lane_scatter_no_repeat) {
    // SCATTER は、直前の時刻に使った鍵盤には置かない（連打が来ない）
    for (uint32_t seed = 1; seed < 30; ++seed) {
        Chart c;
        for (int i = 0; i < 40; ++i) c.notes.push_back(note(4, 1000 + i * 100));
        Lr2Random rng(seed);
        apply_random_option(c, kScatter, rng);
        for (size_t i = 1; i < c.notes.size(); ++i) CHECK(c.notes[i].lane != c.notes[i - 1].lane);
    }
}

TEST(lane_assist_scratch) {
    // 皿の自動演奏：皿のノーツは数だけ進み、スコアにもコンボにも入らない。皿を押しても何も起きない
    Chart c;
    c.notes.push_back(note(0, 1000));
    c.notes.push_back(note(1, 1000));
    c.total = 200;
    JudgeConfig cfg;
    cfg.assist_scratch = true;
    JudgeEngine j(c, cfg);
    j.press(0, 900 * 1000LL);
    j.press(1, 1000 * 1000LL);
    j.advance(1100 * 1000LL);
    CHECK_EQ(j.stats().note_current, 2);
    CHECK_EQ(j.stats().exscore(), 2);
    CHECK_EQ(j.stats().combo, 1);
    CHECK_EQ(j.stats().empty_poor, 0);
    CHECK(j.is_assist_lane(0));
}
