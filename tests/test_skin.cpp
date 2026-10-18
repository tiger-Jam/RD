#include <cmath>
#include <cstdio>
#include <string>

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
