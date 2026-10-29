#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/config.h"
#include "app/skin_catalog.h"
#include "skin/lr2_skin.h"
#include "skin/skin_color.h"
#include "skin/skin_compose.h"
#include "test.h"

using namespace rd;

static SkinDst dst_of(std::initializer_list<std::pair<int, float>> keys, int loop, int acc = 0) {
    SkinDst d;
    d.loop = loop;
    for (auto [t, x] : keys) {
        SkinDstKey k;
        k.time = t;
        k.x = x;
        k.acc = acc;
        d.keys.push_back(k);
    }
    return d;
}

TEST(skin_dst_interpolation) {
    SkinDst d = dst_of({{0, 0.0f}, {1000, 100.0f}}, 0);
    CHECK(!eval_dst(d, -1).visible);  // タイマー停止中
    CHECK(std::abs(eval_dst(d, 500).x - 50) < 1e-3);
    // loop=0：最後まで行ったら0から繰り返す
    CHECK(std::abs(eval_dst(d, 1250).x - 25) < 1e-3);
}

TEST(skin_dst_loop_negative_hides_after_end) {
    SkinDst d = dst_of({{0, 0.0f}, {1000, 100.0f}}, -1);
    CHECK(eval_dst(d, 999).visible);
    CHECK(!eval_dst(d, 1001).visible);
}

TEST(skin_dst_loop_equal_end_holds) {
    SkinDst d = dst_of({{0, 0.0f}, {1000, 100.0f}}, 1000);
    CHECK(std::abs(eval_dst(d, 5000).x - 100) < 1e-3);
}

TEST(skin_dst_loop_partial) {
    // 500〜1000 を繰り返す
    SkinDst d = dst_of({{0, 0.0f}, {1000, 100.0f}}, 500);
    CHECK(std::abs(eval_dst(d, 1200).x - 70) < 1e-3);
}

TEST(skin_dst_accel) {
    SkinDst d = dst_of({{0, 0.0f}, {1000, 100.0f}}, 0, 1);
    CHECK(std::abs(eval_dst(d, 500).x - 12.5) < 1e-3);  // r^3
    SkinDst d2 = dst_of({{0, 0.0f}, {1000, 100.0f}}, 0, 2);
    CHECK(std::abs(eval_dst(d2, 500).x - 87.5) < 1e-3);  // 1-(1-r)^3
    SkinDst d3 = dst_of({{0, 0.0f}, {1000, 100.0f}}, 0, 3);
    CHECK(std::abs(eval_dst(d3, 500).x - 0) < 1e-3);  // それ以外は始点のまま
}

TEST(skin_src_frame) {
    SkinSrc s;
    s.div_x = 4;
    s.div_y = 2;
    s.cycle = 800;
    CHECK_EQ(src_frame(s, 0), 0);
    CHECK_EQ(src_frame(s, 150), 1);
    CHECK_EQ(src_frame(s, 799), 7);
    CHECK_EQ(src_frame(s, 800), 0);
    CHECK_EQ(src_frame(s, -1), 0);
}

// 手元に WMIX_HD があるときだけ：選曲スキンのバーとマウスの部品が読めること
TEST(skin_wmix_select_bar) {
    std::string root = std::string(RD_SOURCE_DIR) + "/参考に/LR2beta3 2";
    std::string path = root + "/LR2files/Theme/WMIX_HD/select/select.lr2skin";
    SkinHeader h;
    if (!read_skin_header(root, path, h)) return;
    Lr2Skin sk;
    std::string err;
    std::array<bool, 1000> op{};
    CHECK(load_lr2_skin(root, path, h, op, sk, err));
    int bodies = 0, on = 0;
    for (int i = 0; i < 30; ++i) bodies += sk.bar.body[i].valid, on += !sk.bar.body_on[i].empty();
    CHECK(bodies >= 5);
    CHECK(on >= 10);
    CHECK(!sk.bar.title[0].dst.empty());
    CHECK(!sk.onmouse.empty());
    std::printf("  WMIX 選曲: バー本体 %d 種類、行 %d、中心 %d、マウス %zu\n", bodies, on, sk.bar.center, sk.onmouse.size());
}

namespace {

// 一時フォルダを作る（そのまま LR2 のフォルダとして使う）
std::filesystem::path skin_test_dir(const char* name) {
    std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

bool load_test_skin(const std::filesystem::path& root, const std::string& text, Lr2Skin& sk,
                    const std::array<bool, 1000>& op = {}) {
    std::filesystem::path p = root / "test.lr2skin";
    std::ofstream(p, std::ios::binary) << text;
    SkinHeader h;
    std::string err;
    return read_skin_header(root.string(), p.string(), h) && load_lr2_skin(root.string(), p.string(), h, op, sk, err);
}

std::string with_newline(const std::string& text, const std::string& nl) {
    std::string out;
    for (char c : text) out += c == '\n' ? nl : std::string(1, c);
    return out;
}

}  // namespace

// 改行が CRLF・LF・CR のどれでも、#IF の入れ子が同じに読める（素の #ENDIF は 6 文字でも読む）
TEST(skin_line_endings_if_nesting) {
    auto root = skin_test_dir("rd_test_skin_nl");
    const std::string text =
        "#INFORMATION,0,T,me\n#ENDOFHEADER\n"
        "#IF,10\n#IF,11\n#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n#ENDIF\n#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n#ENDIF\n"
        "#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n"
        // 素の #ELSE（5 文字）は LR2 でも読まれないので、後ろの行も #IF の中のまま
        "#IF,10\n#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n#ELSE\n#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n#ENDIF\n";
    for (const char* nl : {"\r\n", "\n", "\r"}) {
        Lr2Skin sk;
        CHECK(load_test_skin(root, with_newline(text, nl), sk));
        CHECK_EQ(sk.image_objs.size(), size_t(1));  // op 10 が偽：#IF の外の 1 つだけ
        std::array<bool, 1000> op{};
        op[10] = true;
        Lr2Skin sk2;
        CHECK(load_test_skin(root, with_newline(text, nl), sk2, op));
        CHECK_EQ(sk2.image_objs.size(), size_t(4));  // op 11 の中の 1 つだけ読まない
    }
    std::filesystem::remove_all(root);
}

// #FLIPRESULT / #DISABLEFLIP は読んだ時点で op 350/351 を書き換え、後に書いた方が勝つ（LR2 標準リザルトの RESULT SIDE）
TEST(skin_flipresult_in_order) {
    auto root = skin_test_dir("rd_test_skin_flip");
    const std::string text =
        "#INFORMATION,7,T,me\n#ENDOFHEADER\n"
        "#IF,911,\n#DISABLEFLIP,\n#ENDIF,\n#IF,912,\n#FLIPRESULT,\n#ENDIF,\n"
        "#IF,351,\n#SRC_IMAGE,0,0,0,0,1,1,1,1,0,0\n#ENDIF,\n";
    std::array<bool, 1000> op{};
    op[350] = true;
    Lr2Skin plain;
    CHECK(load_test_skin(root, text, plain, op));
    CHECK(!plain.flip_result);
    CHECK(plain.image_objs.empty());
    // プレイスキンが #FLIPRESULT（op 351 が真）で、リザルト側が LEFT を強制
    std::array<bool, 1000> flipped{};
    flipped[351] = true;
    flipped[911] = true;
    Lr2Skin left;
    CHECK(load_test_skin(root, text, left, flipped));
    CHECK(!left.flip_result);
    CHECK(left.image_objs.empty());
    // リザルト側の #FLIPRESULT も効き、後の #IF に効く
    op[912] = true;
    Lr2Skin right;
    CHECK(load_test_skin(root, text, right, op));
    CHECK(right.flip_result);
    CHECK_EQ(right.image_objs.size(), size_t(1));
    // 何も書かなければ、渡した旗のまま
    Lr2Skin keep;
    CHECK(load_test_skin(root, "#INFORMATION,7,T,me\n", keep, flipped));
    CHECK(keep.flip_result);
    std::filesystem::remove_all(root);
}

// #CUSTOMFILE の候補：フォルダは名前のまま、ファイルは最初の '.' の手前まで。txt は除き、最後に RANDOM
TEST(skin_customfile_candidates) {
    auto root = skin_test_dir("rd_test_skin_cf");
    std::filesystem::create_directories(root / "parts" / "frame.v2");
    for (const char* n : {"a.png", "b.x.png", "readme.txt"}) std::ofstream(root / "parts" / n) << "x";
    std::ofstream(root / "test.lr2skin") << "#INFORMATION,0,T,me\n#CUSTOMFILE,All,parts\\*,b\n"
                                            "#CUSTOMFILE,Png,parts\\*.png,\n#CUSTOMFILE,Missing,parts\\*.png,zzz\n";
    SkinHeader h;
    CHECK(read_skin_header(root.string(), (root / "test.lr2skin").string(), h));
    CHECK_EQ(h.customs.size(), size_t(3));
    const std::vector<std::string> all = {"a", "b", "frame.v2", "RANDOM"};
    CHECK(h.customs[0].labels == all);
    CHECK_EQ(h.customs[0].selected, 1);
    const std::vector<std::string> png = {"a", "b", "RANDOM"};
    CHECK(h.customs[1].labels == png);
    // 既定が候補に無い（空も含む）ときは、どれも選ばない（既定の文字列をそのまま使う）
    CHECK_EQ(h.customs[1].selected, -1);
    CHECK_EQ(h.customs[2].selected, -1);
    std::filesystem::remove_all(root);
}

// パスの終わりの " は、LR2（Windows のファイル検索）と同じく無いものとして探す。拡張子の大文字小文字も見ない
TEST(skin_customfile_trailing_quote) {
    auto root = skin_test_dir("rd_test_skin_quote");
    std::filesystem::create_directories(root / "parts");
    for (const char* n : {"#RED.PNG", "BLUE.PNG"}) std::ofstream(root / "parts" / n) << "x";
    std::ofstream(root / "test.lr2skin") << "#INFORMATION,0,T,me\n#CUSTOMFILE,Flash,.\\parts\\*.png\",#RED\n";
    SkinHeader h;
    CHECK(read_skin_header(root.string(), (root / "test.lr2skin").string(), h));
    CHECK_EQ(h.customs.size(), size_t(1));
    const std::vector<std::string> want = {"#RED", "BLUE", "RANDOM"};
    if (!h.customs.empty()) {
        CHECK(h.customs[0].labels == want);
        CHECK_EQ(h.customs[0].selected, 0);
    }
    CHECK(!resolve_lr2_path(root.string(), ".\\parts\\#RED.png\"", false).empty());
    std::filesystem::remove_all(root);
}

// #CUSTOMFILE は名前で残すので、候補のファイルが増えても選択がずれない。古い形式（番号）も読める
TEST(skin_custom_choice_by_name) {
    auto root = skin_test_dir("rd_test_skin_cfname");
    std::filesystem::create_directories(root / "parts");
    for (const char* n : {"b.png", "c.png"}) std::ofstream(root / "parts" / n) << "x";
    std::string skin = (root / "test.lr2skin").string();
    std::ofstream(skin) << "#INFORMATION,0,T,me\n#CUSTOMOPTION,Judge,900,ON,OFF\n#CUSTOMFILE,Parts,parts\\*.png,b\n";
    SkinHeader h;
    CHECK(read_skin_header(root.string(), skin, h));
    h.customs[0].selected = 1;
    h.customs[1].selected = 1;  // c
    std::vector<SkinCustomChoice> sel = skin_custom_choices(h);
    CHECK(sel[0].file.empty() && sel[0].index == 1);
    CHECK(sel[1].file == "c");
    // config.txt の形で往復する
    CHECK(format_skin_custom(sel) == "1,\"c\"");
    CHECK(parse_skin_custom(format_skin_custom(sel)) == sel);
    SkinCustomChoice odd;
    odd.file = "x, \"y\"";
    CHECK(parse_skin_custom(format_skin_custom({odd})) == std::vector<SkinCustomChoice>{odd});
    CHECK(parse_skin_custom("").empty());
    // 前に a.png が増えても c のまま
    std::ofstream(root / "parts" / "a.png") << "x";
    SkinHeader h2;
    CHECK(read_skin_header(root.string(), skin, h2));
    apply_skin_custom(sel, h2);
    CHECK(h2.customs[1].labels[h2.customs[1].selected] == "c");
    CHECK_EQ(h2.customs[0].selected, 1);
    // 名前が候補から消えたら既定（b）に戻る
    std::filesystem::remove(root / "parts" / "c.png");
    SkinHeader h3;
    CHECK(read_skin_header(root.string(), skin, h3));
    apply_skin_custom(sel, h3);
    CHECK(h3.customs[1].labels[h3.customs[1].selected] == "b");
    // 古い形式：#CUSTOMFILE も番号（a, b, RANDOM の 1 番）
    SkinHeader h4;
    CHECK(read_skin_header(root.string(), skin, h4));
    apply_skin_custom(parse_skin_custom("0,1"), h4);
    CHECK_EQ(h4.customs[0].selected, 0);
    CHECK(h4.customs[1].labels[h4.customs[1].selected] == "b");
    std::filesystem::remove_all(root);
}

// MOD の部品の色：選んだレーンのノーツだけが、色を変えた写しに差し替わる（同じ絵の皿は元のまま）
TEST(skin_color_notes_by_lane) {
    Lr2Skin sk;
    SkinImage img;
    img.w = 4, img.h = 1;
    img.rgba = {255, 0, 0, 255, 0, 0, 255, 255, 128, 128, 128, 255, 0, 0, 0, 0};
    sk.images.push_back(img);
    auto note = [](int x) {
        SkinSrc s;
        s.valid = true, s.gr = 0, s.x = x, s.w = 1, s.h = 1;
        return s;
    };
    sk.note[0] = note(0);  // 皿：赤
    sk.note[1] = note(1);  // 白鍵：青
    sk.note[2] = note(1);  // 青鍵：同じ所
    SkinColorFilter f;
    f.parts = {"note"}, f.lanes = {"white", "blue"}, f.mode = "mul", f.color = 0x00ff00;
    CHECK_EQ(apply_skin_colors(sk, {f}), 2);
    CHECK_EQ(sk.note[0].gr, 0);
    CHECK_EQ(sk.note[1].gr, 1);
    CHECK_EQ(sk.note[2].gr, 1);  // 同じ所は写しを 1 つだけ作る
    CHECK_EQ(sk.images.size(), size_t(2));
    CHECK(sk.note[1].x == 0 && sk.note[1].y == 0);
    // 青（明るさ 0.114）× 緑
    CHECK(sk.images[1].rgba[0] == 0 && sk.images[1].rgba[1] == 29 && sk.images[1].rgba[2] == 0 && sk.images[1].rgba[3] == 255);
    CHECK_EQ(sk.images[0].rgba[6], 255);  // 元の絵は変えない（2 つ目の画素の青）
}

// 色の変え方：色相をずらす（灰色は変わらない）・近い色だけ（遠い色は変わらない）・グラデーションマップ（白は白のまま）
TEST(skin_color_modes) {
    SkinColorFilter f;
    f.mode = "hue", f.hue = 120;
    uint8_t red[4] = {255, 0, 0, 255}, gray[4] = {128, 128, 128, 255};
    recolor_pixel(red, f), recolor_pixel(gray, f);
    CHECK(red[0] == 0 && red[1] == 255 && red[2] == 0);
    CHECK(gray[0] == 128 && gray[1] == 128 && gray[2] == 128);
    f.mode = "range", f.from = 240, f.width = 30;
    uint8_t blue[4] = {0, 0, 255, 255}, red2[4] = {255, 0, 0, 255};
    recolor_pixel(blue, f), recolor_pixel(red2, f);
    CHECK(blue[0] == 255 && blue[1] == 0 && blue[2] == 0);  // 青 → 120 度ずらして赤
    CHECK(red2[0] == 255 && red2[1] == 0 && red2[2] == 0);
    f.mode = "map", f.color = 0x23d100, f.mix = 100;
    uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    recolor_pixel(white, f), recolor_pixel(black, f);
    CHECK(white[0] == 255 && white[1] == 255 && white[2] == 255);
    CHECK(black[0] == 0 && black[1] == 0 && black[2] == 0);
    // 明るい所の白さを 0 にすると、白い所も選んだ色になる
    f.white = 0;
    uint8_t white2[4] = {255, 255, 255, 255};
    recolor_pixel(white2, f);
    CHECK(white2[0] == 0x23 && white2[1] == 0xd1 && white2[2] == 0);
}

// 部品の付け替えのための、レーンが 8 本並んだスキン（x から w ずつ、判定は judge の高さ、ノーツの高さ nh）
static Lr2Skin lanes_skin(float x0, float w, float judge, float nh, int width, int height) {
    Lr2Skin sk;
    sk.width = width, sk.height = height;
    for (int l = 0; l < 8; ++l) {
        SkinDstKey k;
        k.x = x0 + l * w, k.y = judge - nh, k.w = w, k.h = nh;
        sk.note_dst[l].keys.push_back(k);
    }
    SkinImage img;
    img.w = img.h = 4, img.rgba.assign(4 * 4 * 4, 200);
    sk.images.push_back(img);
    return sk;
}

static SkinSrc whole_src(int gr) {
    SkinSrc s;
    s.valid = true, s.gr = gr, s.w = -1, s.h = -1;  // -1 は画像全体
    return s;
}

// ノーツ：絵を写し、太さは元のスキンのノーツの高さをレーン全体の幅の比で直したもの（LN の中は太さを決めない）
TEST(skin_compose_notes) {
    Lr2Skin host = lanes_skin(100, 60, 880, 16, 1920, 1080), from = lanes_skin(20, 20, 315, 6, 640, 480);
    for (int l = 0; l < 8; ++l) from.note[l] = whole_src(0), from.ln_body[l] = whole_src(0);
    CHECK_EQ(compose_play_part(host, from, "notes", PartScale::Lane), 8);
    CHECK(host.note[3].valid && host.note[3].gr == 1 && host.note[3].w == 4);
    CHECK_EQ(host.note[3].draw_h, 18);  // 6 × (480 ÷ 160)
    CHECK_EQ(compose_play_part(host, from, "ln", PartScale::Screen), 8);
    CHECK_EQ(host.ln_body[2].draw_h, 0);
    CHECK_EQ(host.images.size(), size_t(3));  // 部品ごとに 1 回だけ写す（ノーツの 8 本で 1 枚、LN で 1 枚）
}

// ボム：元のスキンのレーンの真ん中・判定の高さからのずれを比で直して付け替え、今のスキンのボムは外す
TEST(skin_compose_bomb) {
    Lr2Skin host = lanes_skin(100, 60, 880, 16, 1920, 1080), from = lanes_skin(20, 20, 315, 6, 640, 480);
    SkinObject mine_bomb, mine_frame;
    mine_bomb.src = whole_src(0), mine_bomb.dst.timer = 51;
    mine_bomb.dst.keys.push_back(SkinDstKey{});
    mine_frame.src = whole_src(0);
    mine_frame.dst.keys.push_back(SkinDstKey{});
    host.image_objs = {mine_bomb, mine_frame};
    SkinObject bomb;
    bomb.src = whole_src(0), bomb.dst.timer = 51;
    SkinDstKey k;
    k.x = 50 - 10, k.y = 315 - 10, k.w = 20, k.h = 20;  // 1 鍵の真ん中（50）・判定の高さ（315）が真ん中
    bomb.dst.keys.push_back(k);
    from.image_objs = {bomb};
    CHECK_EQ(compose_play_part(host, from, "bomb", PartScale::Lane), 1);
    CHECK_EQ(host.image_objs.size(), size_t(2));
    const SkinDstKey& b = host.image_objs.back().dst.keys.front();
    CHECK(b.w == 60 && b.h == 60);                 // 比は 3
    CHECK(b.x == 190 - 30 && b.y == 880 - 30);     // 今のスキンの 1 鍵の真ん中（190）・判定の高さ（880）が真ん中
    CHECK_EQ(host.image_objs.back().dst.timer, 51);
    CHECK_EQ(host.image_objs.front().dst.timer, 0);  // ボムでないものは残る
}

// 元のスキンに無い部品は、今のスキンのものを残す。フレームの部品は持ってこない
TEST(skin_compose_keep_and_gauge) {
    Lr2Skin host = lanes_skin(100, 60, 880, 16, 1920, 1080), from = lanes_skin(20, 20, 315, 6, 640, 480);
    SkinObject bomb;
    bomb.src = whole_src(0), bomb.dst.timer = 52;
    bomb.dst.keys.push_back(SkinDstKey{});
    host.image_objs = {bomb};
    CHECK_EQ(compose_play_part(host, from, "bomb", PartScale::Lane), 0);
    CHECK_EQ(host.image_objs.size(), size_t(1));
    // フレームの部品（ゲージなど）は持ってこない
    from.gauge[0].src = whole_src(0);
    CHECK_EQ(compose_play_part(host, from, "gauge", PartScale::Lane), 0);
}

// 読めない絵（dummy.png）の部品は、透明にする（REMI-S の LN の終点）
TEST(skin_compose_dummy_is_blank) {
    Lr2Skin host = lanes_skin(100, 60, 880, 16, 1920, 1080), from = lanes_skin(20, 20, 315, 6, 640, 480);
    from.images.push_back(SkinImage{});  // 読めなかった絵
    for (int l = 0; l < 8; ++l) from.ln_end[l] = whole_src(1), from.ln_end[l].y = 109;
    CHECK_EQ(compose_play_part(host, from, "ln", PartScale::Lane), 8);
    const SkinSrc& e = host.ln_end[1];
    CHECK(e.x == 0 && e.y == 0 && e.w == 1 && e.h == 1);
    CHECK_EQ(host.images[e.gr].rgba[3], 0);
}

// 部品の絵を選んでいる #CUSTOMFILE が分かる（画像を読まずに読んだスキンで）
TEST(skin_part_customs) {
    auto root = skin_test_dir("rd_test_skin_partcf");
    for (const char* d : {"note", "bomb", "bg"}) {
        std::filesystem::create_directories(root / d);
        for (const char* n : {"a.png", "b.png"}) std::ofstream(root / d / n) << "x";
    }
    Lr2Skin sk;
    std::filesystem::path p = root / "test.lr2skin";
    std::ofstream(p, std::ios::binary) << "#INFORMATION,0,T,me\n#CUSTOMOPTION,Side,900,1P,2P\n"
                                          "#CUSTOMFILE,NOTES,note\\*.png,a\n#CUSTOMFILE,BOMB,bomb\\*.png,b\n#CUSTOMFILE,BG,bg\\*.png,a\n"
                                          "#ENDOFHEADER\n#IMAGE,note\\*.png\n#IMAGE,bomb\\*.png\n#IMAGE,bg\\*.png\n"
                                          "#SRC_NOTE,1,0,0,0,10,10,1,1,0,0\n#DST_NOTE,0,1,0,0,10,10,0,255,255,255,255\n"
                                          "#SRC_IMAGE,0,1,0,0,10,10,1,1,0,51\n#DST_IMAGE,0,0,0,0,10,10,0,255,255,255,255\n"
                                          "#SRC_IMAGE,0,2,0,0,10,10,1,1,0,0\n#DST_IMAGE,0,0,0,0,10,10,0,255,255,255,255\n";
    SkinHeader h;
    std::string err;
    CHECK(read_skin_header(root.string(), p.string(), h));
    CHECK(load_lr2_skin(root.string(), p.string(), h, {}, sk, err, nullptr, false));
    CHECK(play_part_customs(sk, "notes") == std::vector<int>{1});
    CHECK(play_part_customs(sk, "bomb") == std::vector<int>{2});
    CHECK(play_part_customs(sk, "beam").empty());
    CHECK(sk.images[0].rgba.empty());  // 画像は読まない
    std::filesystem::remove_all(root);
}

// 部品ごとの絵の種類は config.txt に残る
TEST(skin_parts_config_custom) {
    auto root = skin_test_dir("rd_test_skin_partcfg");
    AppConfig a;
    a.skin_parts["host.lr2skin"].from["bomb"] = "w.lr2skin";
    a.skin_parts["host.lr2skin"].custom["bomb"][12] = "splash blue";
    a.skin_parts["host.lr2skin"].from["notes"] = "r.lr2skin";
    const std::string path = (root / "config.txt").string();
    CHECK(a.save(path));
    AppConfig b;
    CHECK(b.load(path));
    CHECK_EQ(b.skin_parts.size(), size_t(1));  // パスは実行ファイルのフォルダからの相対で読む
    const AppConfig::SkinParts& p = b.skin_parts.begin()->second;
    CHECK(p.from.size() == 2 && p.custom.size() == 1);
    CHECK(p.custom.at("bomb").at(12) == "splash blue");
    std::filesystem::remove_all(root);
}

// 判定の表示の大きさは config.txt に残る。前の版の 11 個の行は 100% として読む
TEST(skin_adjust_judge_scale_config) {
    auto root = skin_test_dir("rd_test_skin_adjcfg");
    const std::string path = (root / "config.txt").string();
    AppConfig a;
    a.skin_adjust["host.lr2skin"].judge_scale = 135;
    a.skin_adjust["host.lr2skin"].judge_y = -20;
    a.skin_adjust["host.lr2skin"].note_fine_y = -3;
    CHECK(a.save(path));
    AppConfig b;
    CHECK(b.load(path));
    CHECK_EQ(b.skin_adjust.size(), size_t(1));
    CHECK_EQ(b.skin_adjust.begin()->second.judge_scale, 135);
    CHECK_EQ(b.skin_adjust.begin()->second.judge_y, -20);
    CHECK_EQ(b.skin_adjust.begin()->second.note_fine_y, -3);
    std::ofstream(path) << "skin_adjust old.lr2skin = 0,0,100,100,10,0,0,0,0,0,0\n";
    AppConfig c;
    CHECK(c.load(path));
    CHECK_EQ(c.skin_adjust.size(), size_t(1));
    CHECK_EQ(c.skin_adjust.begin()->second.judge_scale, 100);
    CHECK_EQ(c.skin_adjust.begin()->second.judge_x, 10);
    CHECK_EQ(c.skin_adjust.begin()->second.note_fine_y, 0);
    std::filesystem::remove_all(root);
}
