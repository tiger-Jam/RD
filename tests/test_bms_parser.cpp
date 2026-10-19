#include <cmath>

#include "chart/bms_parser.h"
#include "test.h"
#include "util/lr2_random.h"

using namespace bmj;

static BmsParseResult parse(const std::string& s, uint32_t seed = 1) {
    BmsParseOptions o;
    o.random_seed = seed;
    return parse_bms_text(s, o);
}

static int64_t ms(double v) { return static_cast<int64_t>(std::llround(v * 1000)); }

TEST(bms_basic_timing) {
    // BPM120：1小節=2000ms。小節0の頭にノーツは無いので、ずらしは起きない
    auto r = parse("#BPM 120\n#00111:01\n#00112:0001\n#00216:01\n");
    CHECK(r.ok);
    CHECK_EQ(r.chart.notes.size(), size_t(3));
    CHECK_EQ(r.chart.notes[0].time_us, ms(2000));
    CHECK_EQ(r.chart.notes[0].lane, 1);
    CHECK_EQ(r.chart.notes[1].time_us, ms(3000));
    CHECK_EQ(r.chart.notes[1].lane, 2);
    CHECK_EQ(r.chart.notes[2].time_us, ms(4000));
    CHECK_EQ(r.chart.notes[2].lane, 0);  // 16は皿
    CHECK(std::abs(r.chart.notes[1].pos - 6.0) < 1e-9);
}

TEST(bms_measure0_shift) {
    // 小節0の頭にノーツがあると、全体が1小節後ろにずれる（LR2: bmsload:2581）
    auto r = parse("#BPM 120\n#00011:01\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK_EQ(r.chart.notes[0].time_us, ms(2000));
}

TEST(bms_lane_mapping) {
    auto r = parse("#BPM 60\n#00111:01\n#00112:01\n#00113:01\n#00114:01\n#00115:01\n#00118:01\n#00119:01\n#00116:01\n#00117:01\n");
    int lanes[8] = {};
    for (auto& n : r.chart.notes) lanes[n.lane]++;
    for (int i = 0; i < 8; ++i) CHECK_EQ(lanes[i], 1);  // 17（フリーゾーン）は捨てる
    CHECK_EQ(r.chart.notes.size(), size_t(8));
}

TEST(bms_bpm_change_and_stop) {
    // 小節1の真ん中でBPM 120→240（拡張BPM）、小節2の頭で STOP 192（=1小節分 = 4拍）
    auto r = parse("#BPM 120\n#BPM01 240\n#STOP01 192\n#00108:0001\n#00209:01\n#00211:01\n#00311:01\n");
    CHECK(r.ok);
    CHECK_EQ(r.chart.notes.size(), size_t(2));
    // 小節1: 前半1000ms + 後半500ms → 小節2の頭は 2000+1500 = 3500ms
    CHECK_EQ(r.chart.notes[0].time_us, ms(3500));
    // STOP: 192/192 × 240000/240 = 1000ms、小節2の長さ 1000ms → 5500ms
    CHECK_EQ(r.chart.notes[1].time_us, ms(5500));
    // STOP中はスクロールが止まる
    double p0 = r.chart.pos_at(ms(3600)), p1 = r.chart.pos_at(ms(4400));
    CHECK(std::abs(p0 - p1) < 1e-9);
    CHECK(std::abs(r.chart.pos_at(ms(3500)) - 8.0) < 1e-9);
}

TEST(bms_hex_bpm_and_measure_length) {
    // 03は16進のBPM（78 = 120）。小節1の長さ 0.5
    auto r = parse("#BPM 60\n#00103:78\n#00102:0.5\n#00211:01\n");
    // 小節0: BPM60で4000ms、小節1: BPM120で半分の長さ → 1000ms
    CHECK_EQ(r.chart.notes[0].time_us, ms(5000));
}

TEST(bms_ln_channel_pairs) {
    auto r = parse("#BPM 120\n#00151:01000001\n#00211:01\n");
    CHECK_EQ(r.chart.notes.size(), size_t(2));
    CHECK(r.chart.notes[0].kind == NoteKind::Long);
    CHECK_EQ(r.chart.notes[0].time_us, ms(2000));
    CHECK_EQ(r.chart.notes[0].end_us, ms(3500));
    CHECK_EQ(r.chart.judgeable_notes(), 2);
}

TEST(bms_lnobj) {
    auto r = parse("#BPM 120\n#LNOBJ ZZ\n#00111:01ZZ\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK(r.chart.notes[0].kind == NoteKind::Long);
    CHECK_EQ(r.chart.notes[0].end_us, ms(3000));
}

TEST(bms_unterminated_ln_is_normal) {
    auto r = parse("#BPM 120\n#00151:01\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK(r.chart.notes[0].kind == NoteKind::Normal);
}

TEST(bms_same_time_first_wins) {
    // 同じレーン・同じ時刻の通常ノーツは、先に来たものが残る
    auto r = parse("#BPM 120\n#WAV01 a.wav\n#WAV02 b.wav\n#00111:01\n#00111:02\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK_EQ(r.chart.notes[0].sound, 1);
}

TEST(bms_notes_inside_ln_dropped) {
    auto r = parse("#BPM 120\n#00151:0100000000000001\n#00111:00000100\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
}

TEST(bms_mine_and_invisible) {
    auto r = parse("#BPM 120\n#001D1:0A\n#00131:05\n#00211:01\n");
    int mines = 0, inv = 0;
    for (auto& n : r.chart.notes) {
        if (n.kind == NoteKind::Mine) {
            mines++;
            CHECK_EQ(n.damage, 10);
        }
        if (n.kind == NoteKind::Invisible) inv++;
    }
    CHECK_EQ(mines, 1);
    CHECK_EQ(inv, 1);
    CHECK_EQ(r.chart.judgeable_notes(), 1);
}

TEST(bms_default_total) {
    // 未定義なら (N/5+200)*0.8（N<400）。N=2 → 160
    auto r = parse("#BPM 120\n#00111:0101\n");
    CHECK_EQ(static_cast<int>(r.chart.total), 160);
    auto r2 = parse("#BPM 120\n#TOTAL 250.9\n#00111:01\n");
    CHECK_EQ(static_cast<int>(r2.chart.total), 250);
}

TEST(bms_dp_rejected) {
    auto r = parse("#BPM 120\n#00121:01\n");
    CHECK(!r.ok);
}

TEST(bms_random_lr2_semantics) {
    std::string s = "#BPM 120\n#RANDOM 2\n#IF 1\n#00111:01\n#ENDIF\n#IF 2\n#00112:01\n#ENDIF\n";
    for (uint32_t seed = 1; seed < 20; ++seed) {
        auto r = parse(s, seed);
        CHECK_EQ(r.random_values.size(), size_t(1));
        CHECK_EQ(r.chart.notes.size(), size_t(1));
        CHECK_EQ(static_cast<int>(r.chart.notes[0].lane), r.random_values[0]);
    }
    // 同じシードなら同じ結果
    CHECK_EQ(parse(s, 7).random_values[0], parse(s, 7).random_values[0]);
}

TEST(bms_base36) {
    auto r = parse("#BPM 120\n#WAVzz z.wav\n#00111:ZZ\n");
    CHECK_EQ(r.chart.notes[0].sound, 35 * 36 + 35);
    CHECK(r.chart.wav[1295] == "z.wav");
}

TEST(lr2_random_matches_dxlib) {
    // OpenLR2 同梱の DxLib（dep/DxLib/DxUseCLib.cpp の srandMT/randMT/GetRand）をそのまま動かした出力
    Lr2Random r(4357);
    CHECK_EQ(r.next_u32(), 2867219139u);
    CHECK_EQ(r.next_u32(), 1585203162u);
    Lr2Random r2(12345);
    for (int i = 0; i < 699; ++i) r2.next_u32();
    CHECK_EQ(r2.next_u32(), 3745491450u);  // 状態を作り直した後（625個目以降）も一致する
    CHECK_EQ(r2.get_rand(6), 0);
    // 上限が負でも乱数を 1 つ使う（#RANDOM 0 の行で、その後の分岐がずれないように）
    Lr2Random a(7), b(7);
    CHECK_EQ(a.get_rand(-1), 0);
    b.next_u32();
    CHECK_EQ(a.next_u32(), b.next_u32());
    CHECK(Lr2Random(7).get_rand(-2) <= 0);
}
