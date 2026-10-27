#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/config.h"
#include "app/skin_catalog.h"
#include "skin/lr2_skin.h"
#include "test.h"

using namespace bmj;

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
    std::string root = std::string(BMJ_SOURCE_DIR) + "/参考に/LR2beta3 2";
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
    auto root = skin_test_dir("bmj_test_skin_nl");
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
    auto root = skin_test_dir("bmj_test_skin_flip");
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
    auto root = skin_test_dir("bmj_test_skin_cf");
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
    auto root = skin_test_dir("bmj_test_skin_quote");
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
    auto root = skin_test_dir("bmj_test_skin_cfname");
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
