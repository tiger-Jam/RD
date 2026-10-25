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

TEST(lane_drop_scratch) {
    // 7k モード：皿のノーツは除いて、音は BGM として鳴らす（LN は終点の音も）。地雷と見えないノーツは捨てる
    Chart c;
    c.notes.push_back(note(0, 1000));
    c.notes.back().sound = 5;
    c.notes.push_back(note(0, 2000));
    c.notes.back().kind = NoteKind::Long, c.notes.back().end_us = 2500 * 1000LL, c.notes.back().sound = 6, c.notes.back().end_sound = 7;
    c.notes.push_back(note(0, 3000));
    c.notes.back().kind = NoteKind::Mine;
    c.notes.push_back(note(0, 3500));
    c.notes.back().kind = NoteKind::Invisible, c.notes.back().sound = 8;
    c.notes.push_back(note(1, 1000));
    c.drop_scratch();
    CHECK_EQ(c.notes.size(), size_t(1));
    CHECK_EQ(int(c.notes[0].lane), 1);
    CHECK_EQ(c.judgeable_notes(), 1);
    CHECK_EQ(c.bgm.size(), size_t(3));
    CHECK(c.layout == KeyLayout::K7);
    CHECK_EQ(c.bgm[2].time_us, 2500 * 1000LL);
    CHECK_EQ(int(c.bgm[2].sound), 7);
}

TEST(lane_converge_without_scratch) {
    // 7k モードの CONVERGE は皿に置かない
    for (uint32_t seed = 1; seed < 20; ++seed) {
        Chart c;
        for (int i = 0; i < 40; ++i) c.notes.push_back(note(1 + i % 7, 1000 + i * 100));
        Lr2Random rng(seed);
        apply_random_option(c, kConverge, rng, false);
        CHECK_EQ(c.notes.size(), size_t(40));
        for (const Note& n : c.notes) CHECK(n.lane >= 1 && n.lane <= 7);
    }
}

// 5 鍵の譜面（6・7 鍵にノーツが無い）は、1〜5 鍵の中だけで並べ替える（LR2: bmsload:3369）
TEST(lane_five_keys) {
    Chart c;
    for (int i = 0; i < 40; ++i) c.notes.push_back(note(1 + i % 5, 1000 + i * 50));
    c.notes.push_back(note(6, 5000));
    c.notes.back().kind = NoteKind::Mine;  // 地雷は鍵盤数に数えない
    CHECK_EQ(key_count(c), 5);
    for (int opt : {kMirror, kRandom, kSRandom, kScatter}) {
        Chart d = c;
        Lr2Random rng(3);
        apply_random_option(d, opt, rng);
        for (const Note& n : d.notes)
            if (n.kind != NoteKind::Mine) CHECK(n.lane >= 1 && n.lane <= 5);
    }
    Chart m = c;
    Lr2Random rng(1);
    apply_random_option(m, kMirror, rng);
    CHECK_EQ(m.notes[0].lane, 5);  // 1 鍵 → 5 鍵
    c.notes.push_back(note(7, 6000));
    c.notes.back().kind = NoteKind::Invisible;  // 見えないノーツは数える
    CHECK_EQ(key_count(c), 7);
}
