#include "skin/skin_color.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace bmj {

namespace {

void rgb_to_hsl(double r, double g, double b, double& h, double& s, double& l) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    h = s = 0;
    if (mx == mn) return;
    const double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
}

void hsl_to_rgb(double h, double s, double l, double& r, double& g, double& b) {
    if (s == 0) {
        r = g = b = l;
        return;
    }
    h = std::fmod(std::fmod(h, 360) + 360, 360) / 360;
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    auto f = [&](double t) {
        t = std::fmod(t + 1, 1.0);
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    r = f(h + 1.0 / 3), g = f(h), b = f(h - 1.0 / 3);
}

uint8_t to_byte(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 1.0) * 255 + 0.5); }

bool lane_match(int lane, const std::vector<std::string>& lanes) {
    if (lanes.empty()) return true;
    const int l = lane % 10;
    for (const std::string& s : lanes) {
        if (s == "scratch" && l == 0) return true;
        if (s == "white" && l % 2 == 1) return true;
        if (s == "blue" && l != 0 && l % 2 == 0) return true;
        if (s == std::to_string(l)) return true;
    }
    return false;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

const char* const kJudgeKeys[6] = {"empty_poor", "poor", "bad", "good", "great", "pgreat"};

// フィルターが当たる部品の切り出し
std::vector<SkinSrc*> targets(Lr2Skin& sk, const SkinColorFilter& f) {
    std::vector<SkinSrc*> out;
    const auto& p = f.parts;
    auto lanes = [&](std::array<SkinSrc, 20>& a) {
        for (int i = 0; i < 20; ++i)
            if (lane_match(i, f.lanes)) out.push_back(&a[i]);
    };
    if (has(p, "note")) lanes(sk.note), lanes(sk.auto_note);
    if (has(p, "ln")) {
        for (auto* a : {&sk.ln_start, &sk.ln_body, &sk.ln_end, &sk.auto_ln_start, &sk.auto_ln_body, &sk.auto_ln_end}) lanes(*a);
    }
    if (has(p, "mine")) lanes(sk.mine), lanes(sk.auto_mine);
    const float jb = judge_line_bottom(sk);
    for (SkinObject& o : sk.image_objs) {
        int lane = -1;
        bool on = false;
        const char* name = "frame";
        switch (image_part(o, jb, lane, on)) {
        case SkinImagePart::Bomb: name = "bomb"; break;
        case SkinImagePart::Hold: name = "hold"; break;
        case SkinImagePart::Beam: name = "beam"; break;
        case SkinImagePart::Key: name = "key"; break;
        case SkinImagePart::JudgeFx: name = "judge"; break;
        case SkinImagePart::GaugeFx: name = "gauge"; break;
        case SkinImagePart::Frame: break;
        }
        if (has(p, name) && (lane < 0 || lane_match(lane, f.lanes))) out.push_back(&o.src);
    }
    for (int side = 0; side < 2; ++side)
        for (int j = 0; j < 6; ++j) {
            if (!f.judges.empty() && !has(f.judges, kJudgeKeys[j])) continue;
            if (has(p, "judge")) out.push_back(&sk.nowjudge[side][j].src);
            if (has(p, "combo")) out.push_back(&sk.nowcombo[side][j].src);
        }
    for (int i = 0; i < 2; ++i) {
        if (has(p, "judgeline")) out.push_back(&sk.judgeline[i].src);
        if (has(p, "line")) out.push_back(&sk.line[i].src);
        if (has(p, "gauge")) out.push_back(&sk.gauge[i].src);
    }
    if (has(p, "frame")) {
        for (auto* v : {&sk.numbers, &sk.buttons, &sk.bargraphs})
            for (SkinObject& o : *v) out.push_back(&o.src);
        for (SkinObject& o : sk.sliders)
            if (o.src.op[2] != 4 && o.src.op[2] != 5) out.push_back(&o.src);
    }
    return out;
}

}  // namespace

// from の (x, y, w, h) を写した絵を sk の画像に足して、その番号を返す（写せなければ -1）。
// from は sk の画像でもよい（写し終えてから足す）
int add_skin_crop(Lr2Skin& sk, const SkinImage& from, int x, int y, int w, int h) {
    if (from.rgba.empty() || !from.movie.empty() || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > from.w || y + h > from.h) return -1;
    SkinImage img;
    img.w = w, img.h = h;
    img.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int row = 0; row < h; ++row)
        std::copy_n(&from.rgba[(static_cast<size_t>(y + row) * from.w + x) * 4], static_cast<size_t>(w) * 4,
                    &img.rgba[static_cast<size_t>(row) * w * 4]);
    sk.images.push_back(std::move(img));
    return static_cast<int>(sk.images.size()) - 1;
}

// SRC の幅・高さの -1 は「画像全体」（画像を読む前に CSV を読むので、ここで決める）
void resolve_size(SkinSrc& s, const SkinImage& img) {
    if (s.w < 0) s.w = img.w;
    if (s.h < 0) s.h = img.h;
}

float judge_line_bottom(const Lr2Skin& sk) {
    if (sk.judgeline[0].dst.empty()) return 0;
    const SkinDstKey& k = sk.judgeline[0].dst.keys.front();
    return k.y + k.h;
}

SkinImagePart image_part(const SkinObject& o, float judge_bottom, int& lane, bool& on) {
    const int t = o.dst.timer ? o.dst.timer : o.src.timer;
    lane = -1, on = false;
    if (t >= 50 && t <= 69) return lane = t - 50, SkinImagePart::Bomb;
    if (t >= 70 && t <= 89) return lane = t - 70, SkinImagePart::Hold;
    if (t == 46 || t == 47) return SkinImagePart::JudgeFx;
    if (t >= 42 && t <= 45) return SkinImagePart::GaugeFx;
    if (t >= 100 && t <= 139) {
        on = t < 120;
        lane = t - (on ? 100 : 120);
        if (o.dst.keys.empty() || judge_bottom <= 0) return SkinImagePart::Beam;
        // 高さが負なら上へ伸びる（キービームは判定ラインから上へ伸ばすことが多い）
        for (const SkinDstKey& k : o.dst.keys)
            if (k.y + k.h / 2 < judge_bottom - 1) return SkinImagePart::Beam;
        return SkinImagePart::Key;
    }
    return SkinImagePart::Frame;
}

void recolor_pixel(uint8_t* px, const SkinColorFilter& f) {
    const double r = px[0] / 255.0, g = px[1] / 255.0, b = px[2] / 255.0;
    double o[3];
    if (f.mode == "hue" || f.mode == "range") {
        double h, s, l;
        rgb_to_hsl(r, g, b, h, s, l);
        double w = 1;
        if (f.mode == "range") {
            // 変える色から離れるほど弱く（幅の外と、ほぼ灰色のところは変えない）
            const double d = std::abs(std::fmod(std::fmod(h - f.from, 360) + 540, 360) - 180);
            w = d >= f.width || s < 0.08 ? 0 : 0.5 + 0.5 * std::cos(3.14159265358979 * d / f.width);
        }
        if (w == 0) return;
        h += f.hue * w;
        s = std::min(1.0, s * (1 + (f.sat / 100 - 1) * w));
        if (f.mode == "hue") l = std::clamp(l + f.lit / 100, 0.0, 1.0);
        hsl_to_rgb(h, s, l, o[0], o[1], o[2]);
    } else if (f.mode == "map" || f.mode == "mul") {
        const double c[3] = {((f.color >> 16) & 0xff) / 255.0, ((f.color >> 8) & 0xff) / 255.0, (f.color & 0xff) / 255.0};
        const double y = 0.299 * r + 0.587 * g + 0.114 * b, k = f.mix / 100, in[3] = {r, g, b};
        const double wh = std::clamp(f.white, 0.0, 100.0) / 100;
        for (int i = 0; i < 3; ++i) {
            // map：暗い所 → 色 → 明るい所（白さ wh だけ白に寄せた色）。mul：灰色 × 色
            const double top = c[i] + (1 - c[i]) * wh;
            const double v = f.mode == "mul" ? c[i] * y : y < 0.5 ? c[i] * y * 2 : c[i] + (top - c[i]) * (y - 0.5) * 2;
            o[i] = in[i] + (v - in[i]) * k;
        }
    } else {
        return;
    }
    px[0] = to_byte(o[0]), px[1] = to_byte(o[1]), px[2] = to_byte(o[2]);
}

int apply_skin_colors(Lr2Skin& sk, const std::vector<SkinColorFilter>& filters) {
    int count = 0;
    for (const SkinColorFilter& f : filters) {
        // 同じ所を切り出す部品は、色を変えた写しを 1 つだけ作って使い回す
        std::map<std::tuple<int, int, int, int, int>, int> made;
        for (SkinSrc* s : targets(sk, f)) {
            if (!s->valid || s->gr < 0 || s->gr >= static_cast<int>(sk.images.size()) || !sk.images[s->gr].movie.empty()) continue;
            resolve_size(*s, sk.images[s->gr]);
            if (s->w <= 0 || s->h <= 0) continue;
            const auto key = std::make_tuple(s->gr, s->x, s->y, s->w, s->h);
            if (auto it = made.find(key); it != made.end()) {
                s->gr = it->second, s->x = s->y = 0;
                ++count;
                continue;
            }
            const int gr = add_skin_crop(sk, sk.images[s->gr], s->x, s->y, s->w, s->h);
            if (gr < 0) continue;
            std::vector<uint8_t>& px = sk.images[gr].rgba;
            for (size_t i = 0; i < px.size(); i += 4)
                if (px[i + 3]) recolor_pixel(&px[i], f);
            made[key] = gr;
            s->gr = gr, s->x = s->y = 0;
            ++count;
        }
    }
    return count;
}

}  // namespace bmj
