#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "test.h"
#include "util/lr2_random.h"
#include "util/path.h"

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

TEST(bms_measure0_shift_length) {
    // ずらしたときに頭に足す空の小節は、元の小節 0 の長さ（#00002）になる（LR2: bmsload:2589）
    auto r = parse("#BPM 120\n#00002:0.5\n#00011:01\n#00111:01\n");
    CHECK(r.ok);
    CHECK_EQ(r.chart.notes.size(), size_t(2));
    CHECK_EQ(r.chart.notes[0].time_us, ms(1000));  // 空の小節 0（0.5 小節）の後
    CHECK_EQ(r.chart.notes[1].time_us, ms(2000));  // 元の小節 0 も 0.5 小節
    // #00002 が無ければ今までどおり 1 小節
    CHECK_EQ(parse("#BPM 120\n#00011:01\n").chart.notes[0].time_us, ms(2000));
}

TEST(bms_no_bpm_header) {
    // #BPM が無くても、曲の頭の 03/08 で BPM が決まれば読む（LR2: 開始 BPM 0 のまま進める）
    auto r = parse("#00003:78\n#00111:01\n");
    CHECK(r.ok);
    CHECK_EQ(r.chart.notes[0].time_us, ms(2000));
    CHECK(std::abs(r.chart.initial_bpm - 120.0) < 1e-9);
    auto r8 = parse("#BPM01 150\n#00008:01\n#00111:01\n");
    CHECK(r8.ok);
    CHECK_EQ(r8.chart.notes[0].time_us, ms(1600));
    // 曲の頭で BPM が決まらなければ、LR2 では時刻が無限大になるので読まない
    CHECK(!parse("#00111:01\n").ok);
    CHECK(!parse("#00103:78\n#00111:01\n").ok);
    // 小節 0 の頭のノーツでずれると、空の小節は BPM 0 のままなので読めない（LR2 も同じく壊れる）
    CHECK(!parse("#00003:78\n#00011:01\n").ok);
}

TEST(bms_utf8_bom) {
    auto r = parse("\xEF\xBB\xBF#BPM 120\n#TITLE abc\n#00111:01\n");
    CHECK(r.ok);
    CHECK(std::abs(r.chart.initial_bpm - 120.0) < 1e-9);
}

TEST(bms_title_split) {
    // 末尾の括弧を副題に分ける（LR2: fileutil:340-366）
    auto m = [](const std::string& s) { return parse("#BPM 120\n" + s + "#00111:01\n").chart.meta; };
    ChartMeta a = m("#TITLE Song [ANOTHER]\n");
    CHECK(a.title == "Song");
    CHECK(a.subtitle == "[ANOTHER]");
    CHECK_EQ(a.difficulty, 4);
    ChartMeta b = m("#TITLE Song -HYPER-\n");
    CHECK(b.title == "Song" && b.subtitle == "-HYPER-");
    CHECK_EQ(b.difficulty, 3);
    // 開きが先頭なら分けない。閉じが末尾でなければ分けない
    ChartMeta c = m("#TITLE [Song]\n");
    CHECK(c.title == "[Song]" && c.subtitle.empty());
    ChartMeta d = m("#TITLE A(B)C\n");
    CHECK(d.title == "A(B)C" && d.subtitle.empty());
    // ( ) が合わなければ次の [ ] を試す
    ChartMeta e = m("#TITLE A (x [7H]\n");
    CHECK(e.title == "A (x" && e.subtitle == "[7H]");
    CHECK_EQ(e.difficulty, 3);
    // 全角の ～（CP932 の 0x8160）と 【】
    ChartMeta f = m("#TITLE \xE6\x9B\xB2\xEF\xBD\x9E" "EX\xEF\xBD\x9E\n");
    CHECK(f.title == "\xE6\x9B\xB2" && f.subtitle == "\xEF\xBD\x9E" "EX\xEF\xBD\x9E");
    CHECK_EQ(f.difficulty, 4);
    ChartMeta g = m("#TITLE \xE6\x9B\xB2 \xE3\x80\x90SP\xE3\x80\x91\n");
    CHECK(g.title == "\xE6\x9B\xB2" && g.subtitle == "\xE3\x80\x90SP\xE3\x80\x91");
    CHECK_EQ(g.difficulty, 0);  // 括弧の中に当てはまる言葉が無ければ変えない
    // #TITLE の前の #SUBTITLE は消え、後の #SUBTITLE は残る
    CHECK(m("#SUBTITLE x\n#TITLE Song\n").subtitle.empty());
    CHECK(m("#TITLE Song\n#SUBTITLE x\n").subtitle == "x");
    CHECK(m("#TITLE Song [A]\n#SUBTITLE x\n").subtitle == "x");
    // 区切りはタブでもよい
    CHECK(m("#TITLE\tSong\n").title == "Song");
    // 末尾の言葉
    CHECK_EQ(m("#TITLE Song HARD\n").difficulty, 2);
    CHECK_EQ(m("#TITLE Song (BEGINNER)\n").difficulty, 1);
}

TEST(bms_difficulty_order) {
    // #DIFFICULTY と、#TITLE・#GENRE からの推定は、後に書いたほうが勝つ（LR2: songmanage:209-214, 2970）
    auto m = [](const std::string& s) { return parse("#BPM 120\n" + s + "#00111:01\n").chart.meta; };
    CHECK_EQ(m("#GENRE TRANCE ANOTHER\n").difficulty, 4);
    CHECK(m("#GENRE TRANCE ANOTHER\n").genre == "TRANCE ANOTHER");  // ジャンルは分けない
    CHECK_EQ(m("#GENRE X (HYPER)\n").difficulty, 3);
    CHECK_EQ(m("#DIFFICULTY 2\n#GENRE X ANOTHER\n").difficulty, 4);
    CHECK_EQ(m("#GENRE X ANOTHER\n#DIFFICULTY 2\n").difficulty, 2);
    CHECK_EQ(m("#TITLE Song [ANOTHER]\n#DIFFICULTY 1\n").difficulty, 1);
    CHECK_EQ(m("#DIFFICULTY 1\n#TITLE Song [ANOTHER]\n").difficulty, 4);
    CHECK_EQ(m("#DIFFICULTY 7\n").difficulty, 7);                 // 範囲外もそのまま残す
    CHECK_EQ(m("#DIFFICULTY 3\n#DIFFICULTY x\n").difficulty, 3);  // 数字でなければ変えない
}

TEST(bms_select_meta_headers) {
    auto m = [](const std::string& s) { return parse("#BPM 120\n" + s + "#00111:01\n").chart.meta; };
    CHECK(m("#GENRE a\n#CATEGORY b\n").genre == "b");
    CHECK(m("#INFORMATION_A x\n#INFORMATION_B y\n").artist == "x");
    CHECK(m("#INFORMATION_A x\n#INFORMATION_B y\n").subartist == "y");
    CHECK_EQ(m("#PLAYLEVEL 5\n#MAXTRACKS 9\n").playlevel, 9);
    CHECK_EQ(m("#PLAYLEVEL\t12\n").playlevel, 12);
}

TEST(bms_select_meta_random) {
    // 選曲用の読み込みは #RANDOM を評価せず、いつも「#IF 1」の枝を読む（LR2: songmanage:2952）
    std::string s = "#BPM 120\n#RANDOM 2\n#IF 1\n#00111:01\n#ENDIF\n#IF 2\n#00112:01\n#00113:01\n#ENDIF\n#00114:01\n";
    BmsParseOptions o;
    o.select_meta = true;
    for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
        o.random_seed = seed;
        auto r = parse_bms_text(s, o);
        CHECK(r.ok);
        CHECK_EQ(r.chart.notes.size(), size_t(2));
        CHECK(r.random_values.empty());
    }
    // 「#IF 10」も「#IF 1」で始まるので読む。小文字の #if は分岐として扱わない
    CHECK_EQ(parse_bms_text("#BPM 120\n#IF 10\n#00111:01\n#ENDIF\n#if 2\n#00112:01\n", o).chart.notes.size(), size_t(2));
}

TEST(bms_p2_mine_only) {
    // 2P 側の地雷だけなら SP の譜面として読む（LR2 は地雷を鍵盤数の判定に数えない）
    auto r = parse("#BPM 120\n#001E1:01\n#00111:01\n");
    CHECK(r.ok);
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK(!parse("#BPM 120\n#00141:01\n#00111:01\n").ok);  // 2P の不可視は DP
}

TEST(bms_double_for_database) {
    // 曲データベース用には DP の譜面も読む（2P 側のノーツは捨て、DP の印を立てる）。遊ぶときは読まない
    const std::string dp = "#BPM 120\n#00111:01\n#00121:01\n#00129:01\n";
    auto play = parse(dp);
    CHECK(!play.ok && play.double_play);
    BmsParseOptions o;
    o.random_seed = 1, o.allow_double = true;
    auto r = parse_bms_text(dp, o);
    CHECK(r.ok && r.double_play);
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK(!parse_bms_text("#BPM 120\n#00111:01\n", o).double_play);
}

TEST(bms_zero_length_ln) {
    // 長さ 0 の LN は、同じ時刻の通常ノーツを上書きしない（LR2: bmsload:3678）
    auto r = parse("#BPM 120\n#00111:02\n#00151:01\n#00151:01\n");
    CHECK_EQ(r.chart.notes.size(), size_t(1));
    CHECK(r.chart.notes[0].kind == NoteKind::Normal);
    CHECK_EQ(r.chart.notes[0].sound, 2);
    // 長さのある LN は上書きする
    auto l = parse("#BPM 120\n#00111:02\n#00151:01\n#00251:01\n");
    CHECK_EQ(l.chart.notes.size(), size_t(1));
    CHECK(l.chart.notes[0].kind == NoteKind::Long);
}

TEST(bms_end_ignores_invisible) {
    // 曲の終わりに不可視ノーツは数えない（LR2: bmsload:3486, 3649）
    auto r = parse("#BPM 120\n#00111:01\n#00231:01\n");
    CHECK_EQ(r.chart.end_us, ms(4000));
}

TEST(bms_main_bpm_counts_dropped) {
    // 主 BPM は、重なりで捨てたノーツも数える（LR2: bmsload:2899）。同数なら低い BPM
    auto r = parse("#BPM 120\n#00111:01\n#00111:01\n#00203:B4\n#00211:01\n#00212:01\n");
    CHECK_EQ(r.chart.notes.size(), size_t(3));
    CHECK(std::abs(r.chart.main_bpm - 120.0) < 1e-9);
}

namespace {
void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
}  // namespace

TEST(find_alt_sound_and_image) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "bmj_test_altsound";
    fs::remove_all(root);
    fs::path song = root / "song";
    touch(song / "a.flac");
    touch(song / "Sub1" / "sub2" / "Deep.WAV");
    touch(song / "c.ogg");
    touch(song / "c.wav");
    touch(root / "x.mp3");
    touch(root / "d.ogg");
    touch(root / "d.wav");
    touch(song / "img.png");
    touch(root / "up.jpg");
    DirIndex dir(song.string());
    auto same = [](const std::string& a, const fs::path& b) { return !a.empty() && fs::equivalent(a, b); };
    // 書かれた名前 → ogg → wav → flac → mp3（LR2: fileutil:734-795）
    CHECK(same(dir.find_sound("a.wav"), song / "a.flac"));
    CHECK(same(dir.find_sound("c.mp3"), song / "c.ogg"));
    CHECK(same(dir.find_sound("C.WAV"), song / "c.wav"));
    // 何段下のフォルダでも、'\' でも、大文字小文字が違っても見つかる
    CHECK(same(dir.find_sound("sub1\\SUB2\\deep.wav"), song / "Sub1" / "sub2" / "Deep.WAV"));
    CHECK(same(dir.find_sound("sub1/sub2/deep.ogg"), song / "Sub1" / "sub2" / "Deep.WAV"));
    // 1 つ上のフォルダ：書かれた名前 → wav → ogg → flac → mp3
    CHECK(same(dir.find_sound("x.wav"), root / "x.mp3"));
    CHECK(same(dir.find_sound("d.flac"), root / "d.wav"));
    CHECK(same(dir.find_sound("..\\x.mp3"), root / "x.mp3"));
    CHECK(dir.find_sound("none.wav").empty());
    // 画像：書かれた名前 → png → jpg → bmp → 1 つ上のフォルダで同じ手順
    CHECK(same(dir.find_image("IMG.bmp"), song / "img.png"));
    CHECK(same(dir.find_image("up.bmp"), root / "up.jpg"));
    // 今までの resolve も深いフォルダを探せる
    CHECK(same(dir.resolve("SUB1/Sub2/deep.ogg", {".ogg", ".wav"}), song / "Sub1" / "sub2" / "Deep.WAV"));
    fs::remove_all(root);
}

TEST(decode_flac) {
    // ffmpeg で作った 8 kHz・モノラル・80 サンプルの FLAC（パディングのブロックは除いた）
    static const unsigned char kFlac[] = {
        0x66, 0x4c, 0x61, 0x43, 0x80, 0x00, 0x00, 0x22, 0x00, 0x50, 0x00, 0x50,
        0x00, 0x00, 0x8b, 0x00, 0x00, 0x8b, 0x01, 0xf4, 0x00, 0xf0, 0x00, 0x00,
        0x00, 0x50, 0x78, 0xbb, 0xd2, 0x76, 0x1a, 0xe3, 0x66, 0x42, 0xc3, 0x00,
        0xaa, 0xf5, 0xda, 0x7c, 0x09, 0xdc, 0xff, 0xf8, 0x64, 0x08, 0x00, 0x4f,
        0x09, 0x16, 0x00, 0x00, 0x0b, 0x50, 0x0f, 0xff, 0x0a, 0xf5, 0xe7, 0x5e,
        0x9a, 0x84, 0xd4, 0x2d, 0x7a, 0xd7, 0x96, 0xa0, 0xb5, 0x07, 0x5e, 0x75,
        0xe9, 0xa8, 0x4d, 0x42, 0xd7, 0xad, 0x79, 0x6a, 0x0b, 0x50, 0x75, 0xe6,
        0xf5, 0xe9, 0xa8, 0x4d, 0x42, 0xd7, 0xad, 0x79, 0x6a, 0x0b, 0x50, 0x75,
        0xe7, 0x5e, 0x9a, 0x84, 0xd4, 0x2d, 0x7a, 0xd7, 0x96, 0xa0, 0xb5, 0x07,
        0x5e, 0x75, 0xe9, 0xa8, 0x4d, 0x42, 0xd7, 0xab, 0xd7, 0x96, 0xa0, 0xb5,
        0x07, 0x5e, 0x75, 0xe9, 0xa8, 0x4d, 0x42, 0xd7, 0xad, 0x79, 0x6a, 0x0b,
        0x50, 0x75, 0xe7, 0x5e, 0x9a, 0x84, 0xd4, 0x2d, 0x7a, 0xd7, 0x96, 0xa0,
        0xb5, 0x07, 0x5e, 0x6f, 0x5e, 0x9a, 0x84, 0xd4, 0x2d, 0x7a, 0xd7, 0x96,
        0xa0, 0xb5, 0x07, 0x5e, 0x75, 0xe9, 0xa8, 0x4d, 0x42, 0xd7, 0xad, 0x79,
        0x6a, 0x0b, 0x50, 0x75, 0xe7, 0x5e, 0x9a, 0x84, 0xd4, 0x2d, 0x7a, 0x34,
        0xb8
    };
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "bmj_test.flac";
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(kFlac), sizeof(kFlac));
    Sample s;
    std::string err;
    CHECK(decode_sound_file(p.string(), 8000, s, err));
    CHECK_EQ(s.frames(), int64_t(80));
    float peak = 0;
    for (float v : s.pcm) peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.1f);
    fs::remove(p);
}
