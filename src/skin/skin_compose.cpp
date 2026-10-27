#include "skin/skin_compose.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "skin/skin_color.h"

namespace bmj {

namespace {

// スキンの場所：1P のレーン（0 が皿、1〜7 が鍵盤）の位置と幅、判定の高さ（ノーツの下の端が判定のときに来る所）
struct Anchors {
    bool ok = false;
    float x[8] = {}, w[8] = {};
    bool has[8] = {};
    float judge = 0, left = 0, right = 0;
    float center() const { return (left + right) / 2; }
    float lane_center(int l) const { return x[l] + w[l] / 2; }
};

Anchors anchors_of(const Lr2Skin& sk) {
    Anchors a;
    for (int l = 0; l < 8; ++l) {
        if (sk.note_dst[l].empty()) continue;
        const SkinDstKey& k = sk.note_dst[l].keys.front();
        a.x[l] = k.x, a.w[l] = k.w, a.has[l] = true;
        if (!a.ok) a.left = k.x, a.right = k.x + k.w, a.judge = k.y + k.h;
        a.left = std::min(a.left, k.x), a.right = std::max(a.right, k.x + k.w);
        a.ok = true;
    }
    return a;
}

// 絵を写して、SRC をその写しに向ける。読めない絵（dummy.png など）は透明の 1×1 にする（元のスキンでも描かれないので）
struct Copier {
    Lr2Skin& to;
    const Lr2Skin& from;
    std::map<std::tuple<int, int, int, int, int>, int> made;  // 同じ所は 1 回だけ写す

    bool copy(SkinSrc& s) {
        if (!s.valid || s.gr < 0 || s.gr >= static_cast<int>(from.images.size())) return false;
        const SkinImage& img = from.images[s.gr];
        if (!img.movie.empty()) return false;
        if (s.w < 0) s.w = img.w;  // -1 は画像全体
        if (s.h < 0) s.h = img.h;
        const bool blank = img.rgba.empty();
        const auto key = std::make_tuple(s.gr, s.x, s.y, s.w, s.h);
        int gr;
        if (auto it = made.find(key); it != made.end()) {
            gr = it->second;
        } else if (blank) {
            SkinImage none;
            none.w = none.h = 1, none.rgba = {0, 0, 0, 0};
            to.images.push_back(std::move(none));
            gr = made[key] = static_cast<int>(to.images.size()) - 1;
        } else {
            gr = add_skin_crop(to, img, s.x, s.y, s.w, s.h);
            if (gr < 0) return false;
            made[key] = gr;
        }
        if (blank) s.w = s.h = 1, s.div_x = s.div_y = 1;
        s.gr = gr, s.x = s.y = 0;
        return true;
    }
};

// DST のキーを、元のスキンの点 (sx, sy) からのずれを kx・ky 倍して、今のスキンの点 (hx, hy) に付け替える
void move_keys(SkinDst& d, float sx, float sy, float hx, float hy, float kx, float ky, float sort) {
    for (SkinDstKey& k : d.keys) {
        k.x = hx + (k.x - sx) * kx, k.w *= kx;
        k.y = hy + (k.y - sy) * ky, k.h *= ky;
        k.sort = sort;
    }
}

float first_sort(const SkinDst& d, float fallback) { return d.empty() ? fallback : d.keys.front().sort; }

// 部品ごとの、#SRC_IMAGE の種類
std::vector<SkinImagePart> image_kinds(const std::string& part) {
    if (part == "bomb") return {SkinImagePart::Bomb, SkinImagePart::Hold};
    if (part == "beam") return {SkinImagePart::Beam};
    if (part == "judge") return {SkinImagePart::JudgeFx};
    return {};
}

}  // namespace

int compose_play_part(Lr2Skin& sk, const Lr2Skin& from, const std::string& part, PartScale scale) {
    const Anchors h = anchors_of(sk), f = anchors_of(from);
    if (!h.ok || !f.ok) return 0;
    // 大きさの比：レーン全体の幅、または画面の高さ
    const float k = scale == PartScale::Lane ? (h.right - h.left) / std::max(1.0f, f.right - f.left)
                                             : static_cast<float>(sk.height) / std::max(1, from.height);
    float top_sort = 0;
    for (const SkinObject& o : sk.image_objs) top_sort = std::max(top_sort, first_sort(o.dst, 0));
    Copier cp{sk, from, {}};
    int count = 0;

    // ノーツ類：絵だけ持ってきて、レーンに描く。太さは元のスキンのノーツの高さを比で直したもの
    auto notes = [&](std::array<SkinSrc, 20>& to, const std::array<SkinSrc, 20>& src, bool body) {
        for (int l = 0; l < 8; ++l) {
            SkinSrc s = src[l];
            if (!h.has[l] || !cp.copy(s)) continue;
            if (!body && f.has[l]) s.draw_h = std::max(1, static_cast<int>(std::lround(from.note_dst[l].keys.front().h * k)));
            to[l] = s;
            ++count;
        }
    };
    if (part == "notes") notes(sk.note, from.note, false), notes(sk.auto_note, from.auto_note, false);
    if (part == "ln") {
        notes(sk.ln_start, from.ln_start, false), notes(sk.ln_end, from.ln_end, false), notes(sk.ln_body, from.ln_body, true);
        notes(sk.auto_ln_start, from.auto_ln_start, false), notes(sk.auto_ln_end, from.auto_ln_end, false);
        notes(sk.auto_ln_body, from.auto_ln_body, true);
    }
    if (part == "mine") notes(sk.mine, from.mine, false), notes(sk.auto_mine, from.auto_mine, false);

    // #SRC_IMAGE の部品（ボム・キービーム・判定の飾り）：元のスキンのものを全部持ってきて、今のスキンのものは外す
    auto images = [&](const std::vector<SkinImagePart>& kinds, auto place) {
        auto is_kind = [&](const SkinObject& o, const Lr2Skin& s, int& lane) {
            bool on;
            const SkinImagePart p = image_part(o, judge_line_bottom(s), lane, on);
            return std::find(kinds.begin(), kinds.end(), p) != kinds.end();
        };
        std::vector<SkinObject> taken;
        for (const SkinObject& o : from.image_objs) {
            int lane;
            if (!is_kind(o, from, lane)) continue;
            SkinObject c = o;
            if (!cp.copy(c.src) || !place(c, lane)) continue;
            taken.push_back(std::move(c));
        }
        if (taken.empty()) return;  // 元のスキンに無ければ、今のスキンのものを残す
        float sort = top_sort + 1;
        std::vector<SkinObject> kept;
        for (SkinObject& o : sk.image_objs) {
            int lane;
            if (is_kind(o, sk, lane)) sort = std::min(sort, first_sort(o.dst, sort));
            else kept.push_back(std::move(o));
        }
        for (SkinObject& o : taken)
            for (SkinDstKey& key : o.dst.keys) key.sort = sort;
        count += static_cast<int>(taken.size());
        kept.insert(kept.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
        sk.image_objs = std::move(kept);
    };
    if (part == "bomb")
        // ボム：レーンの真ん中と判定の高さから
        images(image_kinds(part), [&](SkinObject& o, int lane) {
            const int l = lane % 10;
            if (lane >= 10 || l >= 8 || !h.has[l] || !f.has[l]) return false;
            move_keys(o.dst, f.lane_center(l), f.judge, h.lane_center(l), h.judge, k, k, 0);
            return true;
        });
    if (part == "beam")
        // キービーム：横はレーンの幅に合わせ、縦は判定の高さから比で
        images(image_kinds(part), [&](SkinObject& o, int lane) {
            const int l = lane % 10;
            if (lane >= 10 || l >= 8 || !h.has[l] || !f.has[l]) return false;
            move_keys(o.dst, f.x[l], f.judge, h.x[l], h.judge, h.w[l] / std::max(1.0f, f.w[l]), k, 0);
            return true;
        });
    if (part == "judge") {
        // 判定の文字：レーン全体の真ん中と判定の高さから。コンボの数字は判定の文字からの位置なので比だけ
        for (int j = 0; j < 6; ++j) {
            SkinObject jo = from.nowjudge[0][j], co = from.nowcombo[0][j];
            if (jo.dst.empty() || !cp.copy(jo.src)) continue;
            const float sort = first_sort(sk.nowjudge[0][j].dst, top_sort + 1);
            move_keys(jo.dst, f.center(), f.judge, h.center(), h.judge, k, k, sort);
            sk.nowjudge[0][j] = jo;
            if (!co.dst.empty() && cp.copy(co.src)) {
                move_keys(co.dst, 0, 0, 0, 0, k, k, sort);
                sk.nowcombo[0][j] = co;
            } else {
                sk.nowcombo[0][j] = SkinObject{};
            }
            ++count;
        }
        if (count > 0)
            images(image_kinds(part), [&](SkinObject& o, int) {
                move_keys(o.dst, f.center(), f.judge, h.center(), h.judge, k, k, 0);
                return true;
            });
    }
    return count;
}

std::vector<int> play_part_customs(const Lr2Skin& from, const std::string& part) {
    std::vector<const SkinSrc*> srcs;
    auto notes = [&](const std::array<SkinSrc, 20>& a) {
        for (int l = 0; l < 8; ++l) srcs.push_back(&a[l]);
    };
    if (part == "notes") notes(from.note), notes(from.auto_note);
    if (part == "ln")
        for (auto* a : {&from.ln_start, &from.ln_end, &from.ln_body, &from.auto_ln_start, &from.auto_ln_end, &from.auto_ln_body}) notes(*a);
    if (part == "mine") notes(from.mine), notes(from.auto_mine);
    if (part == "judge")
        for (int j = 0; j < 6; ++j) srcs.push_back(&from.nowjudge[0][j].src), srcs.push_back(&from.nowcombo[0][j].src);
    const std::vector<SkinImagePart> kinds = image_kinds(part);
    if (!kinds.empty()) {
        const float jb = judge_line_bottom(from);
        for (const SkinObject& o : from.image_objs) {
            int lane;
            bool on;
            if (std::find(kinds.begin(), kinds.end(), image_part(o, jb, lane, on)) != kinds.end() && lane < 10) srcs.push_back(&o.src);
        }
    }
    std::vector<int> out;
    for (const SkinSrc* s : srcs)
        if (s->valid && s->gr >= 0 && s->gr < static_cast<int>(from.images.size()) && from.images[s->gr].custom >= 0)
            out.push_back(from.images[s->gr].custom);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace bmj
