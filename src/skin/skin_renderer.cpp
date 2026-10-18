#include "skin/skin_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "stb_truetype.h"

namespace bmj {

namespace {

SDL_Texture* make_texture(SDL_Renderer* r, const SkinImage& img) {
    if (img.rgba.empty() || img.w <= 0 || img.h <= 0) return nullptr;
    SDL_Surface* s = SDL_CreateSurfaceFrom(img.w, img.h, SDL_PIXELFORMAT_ABGR8888,
                                           const_cast<uint8_t*>(img.rgba.data()), img.w * 4);
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_DestroySurface(s);
    return t;
}

std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        char32_t v = n == 0 ? c : c & (0x3F >> n);
        for (int k = 1; k <= n && i + k < s.size(); ++k) v = (v << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(v);
        i += n + 1;
    }
    return out;
}

int count_digit(int n) {
    int d = 1;
    for (int v = std::abs(n); v > 9; v /= 10) ++d;
    return d;
}

}  // namespace

SDL_Texture* SkinRenderer::create_texture(const SkinImage& img) { return make_texture(r_, img); }

SkinRenderer::~SkinRenderer() {
    for (SDL_Texture* t : textures_) if (t) SDL_DestroyTexture(t);
    for (auto& [k, v] : inverted_) if (v) SDL_DestroyTexture(v);
    for (auto& v : font_textures_)
        for (SDL_Texture* t : v) if (t) SDL_DestroyTexture(t);
}

void SkinRenderer::upload(const Lr2Skin& skin) {
    for (SDL_Texture* t : textures_) if (t) SDL_DestroyTexture(t);
    textures_.clear();
    for (auto& [k, v] : inverted_) SDL_DestroyTexture(v);
    inverted_.clear();
    images_.clear();
    for (const SkinImage& img : skin.images) textures_.push_back(make_texture(r_, img)), images_.push_back(&img);
    font_textures_.clear();
    for (const ImageFont& f : skin.image_fonts) {
        std::vector<SDL_Texture*> v;
        for (const SkinImage& img : f.images) v.push_back(make_texture(r_, img));
        font_textures_.push_back(std::move(v));
    }
}

SDL_Texture* SkinRenderer::inverted(SDL_Texture* t) {
    if (auto it = inverted_.find(t); it != inverted_.end()) return it->second;
    SDL_Texture* inv = nullptr;
    for (size_t i = 0; i < textures_.size(); ++i) {
        if (textures_[i] != t || !images_[i]) continue;
        SkinImage copy = *images_[i];
        for (size_t p = 0; p + 3 < copy.rgba.size(); p += 4)
            copy.rgba[p] = 255 - copy.rgba[p], copy.rgba[p + 1] = 255 - copy.rgba[p + 1], copy.rgba[p + 2] = 255 - copy.rgba[p + 2];
        inv = make_texture(r_, copy);
        break;
    }
    inverted_[t] = inv;
    return inv;
}

void SkinRenderer::set_image_override(int gr, SDL_Texture* tex) {
    if (tex) overrides_[gr] = tex;
    else overrides_.erase(gr);
}

SDL_Texture* SkinRenderer::image_tex(int gr) const {
    if (auto it = overrides_.find(gr); it != overrides_.end()) return it->second;
    if (gr < 0 || gr >= static_cast<int>(textures_.size())) return nullptr;
    return textures_[gr];
}

// LR2: ReadSRC の DerivationGraph と同じ切り出し
SDL_FRect SkinRenderer::frame_rect(const SkinSrc& src, int frame) const {
    int bw = src.w / src.div_x, bh = src.h / src.div_y;
    int i = frame % src.div_x, j = frame / src.div_x;
    return SDL_FRect{static_cast<float>(src.x + bw * i), static_cast<float>(src.y + bh * j), static_cast<float>(bw),
                     static_cast<float>(bh)};
}

double SkinRenderer::src_time(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st) const {
    double t = st.timers->lapse(src.timer, st.now_ms);
    // SRCとDSTが同じタイマーなら、DSTの最初のキーの時刻を起点にアニメーションする
    if (src.timer == dst.timer && !dst.keys.empty()) t -= dst.keys.front().time;
    return t;
}

bool SkinRenderer::dst_on(const SkinDst& d, const PlaySkinState& st) const {
    return !d.keys.empty() && st.option(d.op[0]) && st.option(d.op[1]) && st.option(d.op[2]);
}

void SkinRenderer::add(const SkinSrc& src, SkinDraw d, double t) {
    // 切り出す大きさが 0 の画像は描かない（押す場所だけのボタンなど）
    if (!d.visible || !src.valid || src.w == 0 || src.h == 0) return;
    SDL_Texture* tex = image_tex(src.gr);
    if (!tex) return;
    Item it;
    it.sort = d.sort;
    it.tex = tex;
    if (src.w < 0 || src.h < 0) {
        // 大きさが -1（画像全体）で、画像が後から差し替わるもの（バナーなど）
        float tw = 0, th = 0;
        SDL_GetTextureSize(tex, &tw, &th);
        SkinSrc full = src;
        full.w = static_cast<int>(tw), full.h = static_cast<int>(th);
        it.src = frame_rect(full, src_frame(full, t));
    } else {
        it.src = frame_rect(src, src_frame(src, t));
    }
    it.d = d;
    it.clip_top = clip_top_;
    it.clip_bottom = clip_bottom_;
    items_.push_back(it);
}

// LR2: AddDrawingBuffer_Object
void SkinRenderer::add_object(const SkinObject& o, const PlaySkinState& st, float dx, float dy) {
    if (o.dst.empty()) return;
    SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
    d.x += dx;
    d.y += dy;
    add(o.src, d, src_time(o.src, o.dst, st));
}

// LR2: AddDrawingBuffer_Numbers（skindraw:575）
void SkinRenderer::add_numbers(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st, int number, float x,
                               float y) {
    if (dst.empty() || !src.valid || src.op[2] <= 0 || src.op[1] < 0 || src.op[1] >= 3) return;
    int count = src.graphcount();
    double tsrc = st.timers->lapse(src.timer, st.now_ms);
    int cyc = 0, kind = 0;
    auto cycle_of = [&](int unit) {
        return src.cycle > 0 ? (count / unit) * (static_cast<int>(tsrc) % src.cycle) / src.cycle * unit : 0;
    };
    if (count % 10 == 0) kind = 1, cyc = cycle_of(10);
    if (count % 11 == 0) kind = 2, cyc = cycle_of(11);
    if (count % 24 == 0) kind = 3, cyc = cycle_of(24);
    if (kind == 0) return;
    if (kind != 3 && number < 0) return;
    SkinDraw d = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    if (!d.visible) return;
    d.x += x;
    d.y += y;
    char buf[32];
    std::string s;
    int digits = src.op[2];
    if (src.op[1] == 0) {
        std::snprintf(buf, sizeof(buf), "%10d", std::abs(number));
        s = buf;
        if (static_cast<int>(s.size()) > digits) s = s.substr(s.size() - digits);
    } else {
        if (src.op[1] == 2) {
            int blanks = std::max(0, digits - count_digit(number));
            d.x += blanks * d.w * 0.5f;
        }
        std::snprintf(buf, sizeof(buf), "%d          ", std::abs(number));
        s = buf;
        if (static_cast<int>(s.size()) > digits) s.resize(digits);
    }
    SDL_Texture* tex = image_tex(src.gr);
    if (!tex) return;
    auto put = [&](int frame) {
        Item it;
        it.sort = d.sort;
        it.tex = tex;
        it.src = frame_rect(src, frame);
        it.d = d;
        items_.push_back(it);
    };
    if (kind == 3) {
        put(cyc + (number < 0 ? 23 : 11));
        d.x += d.w;
    }
    if (number < 0 && count == 24) cyc += 12;
    for (char ch : s) {
        if (ch == ' ') {
            if (kind != 1) put(cyc + 10);
        } else if (ch >= '0' && ch <= '9') {
            put(cyc + (ch - '0'));
        }
        d.x += d.w;
    }
}

// LR2: AddDrawingBuffer_PlayArea。フィルタは無し、合成はアルファ
void SkinRenderer::add_play_area(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st, float sx, float sy,
                                 int alpha) {
    if (dst.empty() || !src.valid) return;
    SkinDraw d = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    d.x += sx;
    d.y += sy;
    d.filter = 0;
    d.blend = 1;
    if (alpha >= 0) d.a = alpha;
    add(src, d, src_time(src, dst, st));
}

// LR2: AddDrawingBuffer_LN（skindraw:279）
void SkinRenderer::add_ln(const SkinSrc& s, const SkinSrc& e, const SkinSrc& b, const SkinDst& dst,
                          const PlaySkinState& st, float shift_y, float long_y, int active) {
    if (dst.empty() || !s.valid || !e.valid || !b.valid) return;
    int alpha = active != -1 ? 255 : 128;
    SkinDraw base = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    auto part = [&](const SkinSrc& src, SkinDraw d) {
        d.blend = 1;
        d.a = alpha;
        add(src, d, active == 0 ? -1.0 : src_time(src, dst, st));
    };
    SkinDraw body = base;
    body.y += long_y + body.h;
    body.h = shift_y - long_y - body.h;
    part(b, body);
    SkinDraw start = base;
    start.sort += 2;
    start.y += shift_y;
    part(s, start);
    SkinDraw end = base;
    end.sort += 1;
    end.y += long_y;
    part(e, end);
}

// LR2: DrawNotes（Scene04_Play.cpp:310）。1P側（スキンのレーン番号0..7）だけ
void SkinRenderer::add_notes(const Lr2Skin& skin, const PlaySkinState& st) {
    const Chart& chart = *st.chart;
    const JudgeEngine& judge = *st.judge;
    // 1拍=480（LR2の1小節=1920）。y = (今 − ノーツ) × ハイスピ / 600
    double k = 480.0 * st.speed_multiplier * st.hi_speed * (st.base_speed / 100.0) / 600.0;
    float draw_start = 0;
    if (!skin.note_dst[1].empty()) draw_start = -skin.note_dst[1].keys.front().h;

    // 小節線
    if (!skin.line[0].dst.empty()) {
        float base_y = skin.line[0].dst.keys.front().y;
        for (const BarLine& bar : chart.bars) {
            if (bar.pos < st.render_beat) continue;
            float y = static_cast<float>((st.render_beat - bar.pos) * k);
            if (base_y + y <= draw_start) break;
            add_play_area(skin.line[0].src, skin.line[0].dst, st, 0, y, 255);
        }
    }

    for (int lane = 0; lane < kLaneCount; ++lane) {
        const SkinDst& dst = skin.note_dst[lane];
        if (dst.empty()) continue;
        float dst_y = dst.keys.front().y;
        // HIDDEN / SUDDEN（LR2: Play.cpp:328）。判定ラインまでの高さの 60% を境に隠す。
        // OpenLR2 はここでスキンの行番号（sortID）を高さとして使っているが、移植の誤りと見て判定ラインの位置を使う
        clip_top_ = clip_bottom_ = -1;
        if (st.effect == 1) clip_top_ = 0, clip_bottom_ = dst_y * 0.6f;
        else if (st.effect == 2) clip_top_ = dst_y * 0.6f, clip_bottom_ = static_cast<float>(skin.height);
        else if (st.effect == 3) clip_top_ = dst_y * 0.55f, clip_bottom_ = dst_y * 0.65f;
        for (size_t i = 0; i < chart.notes.size(); ++i) {
            const Note& n = chart.notes[i];
            if (n.lane != lane || n.kind == NoteKind::Invisible) continue;
            bool is_ln = n.kind == NoteKind::Long;
            double end_pos = is_ln ? n.end_pos : n.pos;
            // 判定が終わって、判定ラインも過ぎたものは描かない
            if (judge.note_done(i) && n.pos <= st.render_beat && end_pos <= st.render_beat) continue;
            double raw = (st.render_beat - n.pos) * k;
            if (dst_y + raw <= draw_start && n.pos > st.render_beat) break;  // 画面より上
            int note_y = static_cast<int>(raw);
            int note_l = static_cast<int>((st.render_beat - end_pos) * k);
            if (note_y > 0) note_y = 0;
            if (note_l > 0) note_l = 0;
            // 皿の自動演奏のレーンは AUTO 用の絵（スキンに無ければふだんの絵）
            bool assist = judge.is_assist_lane(lane);
            auto pick = [&](const std::array<SkinSrc, 20>& a, const std::array<SkinSrc, 20>& b) -> const SkinSrc& {
                return assist && a[lane].valid ? a[lane] : b[lane];
            };
            if (n.kind == NoteKind::Mine) {
                add_play_area(pick(skin.auto_mine, skin.mine), dst, st, 0, static_cast<float>(note_y), 255);
            } else if (!is_ln) {
                add_play_area(pick(skin.auto_note, skin.note), dst, st, 0, static_cast<float>(note_y), 255);
            } else if (assist && skin.auto_ln_start[lane].valid) {
                bool on = n.pos <= st.render_beat && end_pos > st.render_beat;
                add_ln(skin.auto_ln_start[lane], skin.auto_ln_end[lane], skin.auto_ln_body[lane], dst, st,
                       static_cast<float>(note_y), static_cast<float>(note_l), on ? 5 : 0);
            } else {
                int active;
                if (st.autoplay) active = (n.pos <= st.render_beat && end_pos > st.render_beat) ? 5 : 0;
                else active = judge.note_done(i) ? -1 : judge.ln_holding(i) ? 5 : 0;
                add_ln(skin.ln_start[lane], skin.ln_end[lane], skin.ln_body[lane], dst, st, static_cast<float>(note_y),
                       static_cast<float>(note_l), active);
            }
        }
    }
    clip_top_ = clip_bottom_ = -1;
}

// LR2: AddDrawingBuffer_Gauge（skindraw:389）
void SkinRenderer::add_gauge(const SkinObject& o, const PlaySkinState& st) {
    if (o.dst.empty() || !o.src.valid) return;
    SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
    if (!d.visible) return;
    int count = o.src.graphcount();
    int cyc = 0;
    if (count > 4 && o.src.cycle > 0)
        cyc = (count / 4) * (static_cast<int>(st.timers->lapse(o.src.timer, st.now_ms)) % o.src.cycle) / o.src.cycle * 4;
    int hp = static_cast<int>(st.gauge_hp) / 2;
    rand_ = rand_ * 1103515245u + 12345u;
    int tremble = hp - static_cast<int>((rand_ >> 16) % 3);
    SDL_Texture* tex = image_tex(o.src.gr);
    if (!tex) return;
    for (int i = 1; i <= 50; ++i) {
        int f;
        if (!st.gauge_survival) {
            if (i < 40) f = ((i < tremble || hp <= i || i == 1) && i <= hp) ? cyc + 1 : cyc + 3;
            else if (tremble <= i && i < hp && i != 1) f = cyc + 2;
            else if (hp < i) f = cyc + 2;
            else f = cyc;
        } else if (i < tremble || hp <= i || i == 1) {
            f = hp < i ? cyc + 2 : cyc;
        } else {
            f = cyc + 2;
        }
        Item it;
        it.sort = d.sort;
        it.tex = tex;
        it.src = frame_rect(o.src, std::min(f, count - 1));
        it.d = d;
        items_.push_back(it);
        d.x += o.src.op[0];
        d.y += o.src.op[1];
    }
}

// LR2: DrawJudgeCombo / AddDrawingBuffer_JudgeCombo（skindraw:697）
void SkinRenderer::add_judge_combo(const Lr2Skin& skin, const PlaySkinState& st) {
    int judge = std::clamp(st.judge_draw, 0, 5);
    int combo = judge < 3 ? 0 : st.combo_draw;
    const SkinObject& j = skin.nowjudge[0][judge];
    const SkinObject& c = skin.nowcombo[0][judge];
    if (j.dst.empty()) return;
    int digit = 0;
    if (combo > 0) digit = count_digit(combo);
    float pos = 0, posn = 0;
    double jt = st.timers->lapse(j.dst.timer, st.now_ms);
    SkinDraw cd = eval_dst(c.dst, jt);
    if (j.src.op[0] == 0 && combo > 0) pos = -(digit * cd.w * 0.5f);
    else posn = -((digit - 1) * cd.w * 0.5f);
    add_object(j, st, pos, 0);
    if (combo > 0) {
        SkinDraw jd = eval_dst(j.dst, jt);
        add_numbers(c.src, c.dst, st, combo, pos + posn + jd.x, jd.y);
    }
}

// LR2: AddDrawingBuffer_BGA（skindraw:434）。256より小さい画像は等倍のまま横は中央に寄せる
void SkinRenderer::add_bga(const SkinObject& o, const PlaySkinState& st, int id, bool base) {
    if (o.dst.empty() || !st.bga || id < 0 || id >= static_cast<int>(st.bga->size())) return;
    SDL_Texture* tex = (*st.bga)[id];
    if (!tex) return;
    SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
    if (!d.visible) return;
    float iw = 0, ih = 0;
    SDL_GetTextureSize(tex, &iw, &ih);
    if (iw < 256) {
        d.x += d.w / 256.0f * (256.0f - iw) * 0.5f;
        d.w *= iw / 256.0f;
    }
    if (ih < 256) d.h *= ih / 256.0f;
    if (!base) d.sort += 1;
    Item it;
    it.sort = d.sort;
    it.tex = tex;
    it.src = SDL_FRect{0, 0, iw, ih};
    it.d = d;
    it.backbox = base;
    items_.push_back(it);
}

void SkinRenderer::draw_play(const Lr2Skin& skin, const PlaySkinState& st) {
    items_.clear();
    // 集める順はLR2と同じ（ProcI_Play → メインループの各オブジェクト）。最後に描画順で並べ替える
    add_gauge(skin.gauge[0], st);
    if (st.notes_visible && st.chart && st.judge) {
        add_notes(skin, st);
        add_judge_combo(skin, st);
    }
    if (!skin.judgeline[0].dst.empty()) add_play_area(skin.judgeline[0].src, skin.judgeline[0].dst, st, 0, 0, -1);
    add_common(skin, st);
    flush(skin);
}

// LR2: Proc_Result（Scene05_Result.cpp:30）。ゲージの推移グラフは80%未満とそれ以上で画像を描き分ける
void SkinRenderer::add_gauge_chart(const Lr2Skin& skin, const PlaySkinState& st) {
    const SkinObject& lo = skin.gaugechart[0];
    const SkinObject& hi = skin.gaugechart[1];
    if (!st.graph || lo.src.op[0] <= 0 || lo.dst.empty()) return;
    const PlayGraph& g = *st.graph;
    int gauge = std::clamp(st.graph_gauge, 0, kGaugeCount - 1);
    bool groove = gauge == kGroove || gauge == kEasy;
    double t0 = st.timers->lapse(0, st.now_ms);
    int length = static_cast<int>(st.chart_length(lo.src.op[0], t0));
    int step = std::max(1, static_cast<int>(lo.dst.keys.front().w));
    int val = g.hp[gauge][0];
    for (int i = 0; i < length; i += step) {
        int axis = std::min(PlayGraph::kPoints - 1, i * 1000 / lo.src.op[0]);
        int last = 0;
        // 前の点との間は、1%ずつ縦に点を打って線にする
        while (val != g.hp[gauge][axis]) {
            const SkinObject& o = (groove && val < 80) ? lo : hi;
            int y = o.src.op[1] * val / -100;
            if (last != y) {
                add_object(o, st, static_cast<float>(i), static_cast<float>(y));
                last = y;
            }
            val += val < g.hp[gauge][axis] ? 1 : -1;
        }
        const SkinObject& o = (groove && g.hp[gauge][axis] < 80) ? lo : hi;
        add_object(o, st, static_cast<float>(i), static_cast<float>(o.src.op[1] * g.hp[gauge][axis] / -100));
    }
}

// スコアの推移（0=自分, 1=自己ベスト, 2=ターゲット）
void SkinRenderer::add_score_chart(const Lr2Skin& skin, const PlaySkinState& st) {
    if (!st.graph || st.total_notes <= 0) return;
    const PlayGraph& g = *st.graph;
    const std::array<int, PlayGraph::kPoints>* data[3] = {&g.exscore, &g.best, &g.target};
    double t0 = st.timers->lapse(0, st.now_ms);
    for (int k = 0; k < 3; ++k) {
        const SkinObject& o = skin.scorechart[k];
        if (!o.src.valid || o.src.op[0] <= 0 || o.dst.empty()) continue;
        const auto& d = *data[k];
        int length = static_cast<int>(st.chart_length(o.src.op[0], t0));
        int step = std::max(1, static_cast<int>(o.dst.keys.front().w));
        int div = st.total_notes * -2;
        int val = d[0];
        for (int i = 0; i < length; i += step) {
            int axis = std::min(PlayGraph::kPoints - 1, i * 1000 / o.src.op[0]);
            int last = 0;
            while (val != d[axis]) {
                int y = o.src.op[1] * val / div;
                if (last != y) {
                    add_object(o, st, static_cast<float>(i), static_cast<float>(y));
                    last = y;
                }
                val += val < d[axis] ? 1 : -1;
            }
            add_object(o, st, static_cast<float>(i), static_cast<float>(d[axis] * o.src.op[1] / div));
        }
    }
}

void SkinRenderer::draw_result(const Lr2Skin& skin, const PlaySkinState& st) {
    items_.clear();
    add_gauge_chart(skin, st);
    add_score_chart(skin, st);
    add_common(skin, st);
    flush(skin);
}

// 選曲画面（docs/lr2_select_spec.md）
void SkinRenderer::draw_select(const Lr2Skin& skin, const PlaySkinState& st) {
    items_.clear();
    if (st.select) add_bars(skin, st);
    add_common(skin, st);
    if (st.select) add_onmouse(skin, st), add_readme(skin, st);
    flush(skin);
}

namespace {
SkinDraw lerp_draw(const SkinDraw& a, const SkinDraw& b, float r) {
    SkinDraw d = a;
    auto mix = [r](float x, float y) { return x + (y - x) * r; };
    auto mixi = [r](int x, int y) { return static_cast<int>(x + (y - x) * r); };
    d.x = mix(a.x, b.x), d.y = mix(a.y, b.y), d.w = mix(a.w, b.w), d.h = mix(a.h, b.h);
    d.a = mixi(a.a, b.a), d.r = mixi(a.r, b.r), d.g = mixi(a.g, b.g), d.b = mixi(a.b, b.b);
    d.angle = mix(a.angle, b.angle);
    d.sort = mix(a.sort, b.sort);
    return d;
}
}  // namespace

void SkinRenderer::add_text(const SkinText& t, const PlaySkinState& st, const std::string& str, float dx, float dy) {
    if (t.dst.empty() || str.empty()) return;
    SkinDraw d = eval_dst(t.dst, st.timers->lapse(t.dst.timer, st.now_ms));
    if (!d.visible) return;
    d.x += dx;
    d.y += dy;
    Item it;
    it.sort = d.sort;
    it.is_text = true;
    it.str = str;
    it.font = t.src.font;
    it.align = t.src.align;
    it.d = d;
    items_.push_back(it);
}

// 曲リスト（仕様 3.2, 3.3）。行 i は、行 i と行 i-1 の配置の間を、スクロールの端数で補間した位置に描く
void SkinRenderer::add_bars(const Lr2Skin& skin, const PlaySkinState& st) {
    const SelectSkinState& ss = *st.select;
    const Lr2Skin::Bar& b = skin.bar;
    if (ss.count <= 0 || !ss.bar) return;
    double t0 = st.timers->lapse(0, st.now_ms);  // バーの配置は常にシーン開始からの時間で決まる
    int frac = ss.lcb % 1000, base = ss.lcb / 1000;
    auto row = [&](int i) -> const SkinDst& { return i == ss.sel && !b.body_on[i].empty() ? b.body_on[i] : b.body_off[i]; };
    for (int i = 1; i < 30; ++i) {
        if (b.body_off[i - 1].empty() || b.body_off[i].empty()) continue;
        SkinDraw a = eval_dst(row(i), t0);
        SkinDraw p = eval_dst(row(i - 1), t0);
        if (!a.visible || (frac != 0 && !p.visible)) continue;
        SkinDraw d = frac ? lerp_draw(a, p, frac / 1000.0f) : a;
        int idx = ((base - b.center + i) % ss.count + ss.count) % ss.count;
        SelectBar bar = ss.bar(idx);
        if (bar.type >= 0 && bar.type < 10) add(b.body[bar.type], d, st.timers->lapse(b.body[bar.type].timer, st.now_ms));
        if (i == ss.sel && b.flash.src.valid && !b.flash.dst.empty()) {
            // フラッシュは選んでいる行の位置に留まる（スクロールで動かない）
            SkinDraw o = eval_dst(row(ss.sel), t0);
            add_object(b.flash, st, o.x, o.y);
        }
        add_text(b.title[bar.is_new ? 1 : 0], st, bar.title, d.x, d.y);
        if ((bar.type == 0 || bar.type == 5) && bar.level_index >= 0 && bar.level_index <= 10) {
            const SkinObject& lv = b.level[bar.level_index];
            if (lv.src.valid && !lv.dst.empty()) add_numbers(lv.src, lv.dst, st, bar.level, d.x, d.y);
        }
        if (bar.lamp >= 0 && bar.lamp < 10 && b.lamp[bar.lamp].src.valid) add_object(b.lamp[bar.lamp], st, d.x, d.y);
    }
}

// テキストビューア（仕様 2.13）。各行を「DST の位置 + 行の高さ×行 + スクロール量」に描く。画面の外の行は描かない
void SkinRenderer::add_readme(const Lr2Skin& skin, const PlaySkinState& st) {
    const SelectSkinState& ss = *st.select;
    if (!ss.readme || ss.readme->empty()) return;
    for (const SkinText& t : skin.readme) {
        if (t.dst.empty()) continue;
        SkinDraw d = eval_dst(t.dst, st.timers->lapse(t.dst.timer, st.now_ms));
        if (!d.visible) continue;
        float lh = static_cast<float>(t.src.op[0] > 0 ? t.src.op[0] : 20);
        for (size_t i = 0; i < ss.readme->size(); ++i) {
            float y = d.y + lh * static_cast<float>(i) + ss.readme_y;
            if (y < -lh) continue;
            if (y > skin.height) break;
            const std::string& line = (*ss.readme)[i];
            if (line.empty()) continue;
            Item it;
            it.sort = d.sort;
            it.is_text = true;
            it.str = line;
            it.font = t.src.font;
            it.align = t.src.align;
            it.d = d;
            it.d.x += ss.readme_x;
            it.d.y = y;
            items_.push_back(it);
        }
    }
}

// マウスを乗せたときだけの絵（仕様 2.12）と、マウスカーソル（2.14）
void SkinRenderer::add_onmouse(const Lr2Skin& skin, const PlaySkinState& st) {
    const SelectSkinState& ss = *st.select;
    for (const SkinObject& o : skin.onmouse) {
        if (ss.readme_open) break;  // テキストを見ている間は出さない
        if (!dst_on(o.dst, st) || !o.src.valid) continue;
        int panel = o.src.op[0];
        if (panel != 0 && panel != ss.panel) continue;
        SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
        if (!d.visible) continue;
        float hx = d.x + o.src.op[1], hy = d.y + o.src.op[2];
        if (ss.mouse_x < hx || ss.mouse_x >= hx + o.src.op[3] || ss.mouse_y < hy || ss.mouse_y >= hy + o.src.op[4]) continue;
        add(o.src, d, src_time(o.src, o.dst, st));
    }
    const SkinObject& c = skin.mousecursor;
    if (c.src.valid && !c.dst.empty() && ss.mouse_idle_ms < 10500) {
        SkinDraw d = eval_dst(c.dst, st.timers->lapse(c.dst.timer, st.now_ms));
        if (d.visible) {
            d.x += ss.mouse_x;
            d.y += ss.mouse_y;
            if (ss.mouse_idle_ms > 10000) d.a = static_cast<int>(d.a * (10500 - ss.mouse_idle_ms) / 500), d.blend = 1;
            d.sort = 1e9f;  // 一番手前
            add(c.src, d, src_time(c.src, c.dst, st));
        }
    }
}

// プレイ画面とリザルト画面で共通のオブジェクト
void SkinRenderer::add_common(const Lr2Skin& skin, const PlaySkinState& st) {
    for (const SkinObject& o : skin.image_objs) {
        if (!dst_on(o.dst, st)) continue;
        if (o.dst.op[3] == 1) {
            // 皿の回転（LR2: AddDrawingBuffer_Scratch）
            SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
            d.angle = st.scratch_angle;
            add(o.src, d, src_time(o.src, o.dst, st));
        } else {
            add_object(o, st, 0, 0);
        }
    }
    // BGA（LR2: Main.cpp:2045）。ミスした直後はPOORの層だけ、それ以外は一番下の層と重ねる層
    if (st.notes_visible) {
        for (const SkinObject& o : skin.bgas) {
            if (!dst_on(o.dst, st)) continue;
            if (st.bga_show_poor && st.bga_poor >= 0) {
                add_bga(o, st, st.bga_poor, false);
            } else {
                add_bga(o, st, st.bga_base, true);
                add_bga(o, st, st.bga_layer, false);
            }
        }
    }
    // ボタン（LR2: SetObjectValue_Button → AddDrawingBuffer_Image）。設定の値をそのままコマ番号にする
    for (const SkinObject& o : skin.buttons) {
        if (!dst_on(o.dst, st) || !o.src.valid || o.src.w == 0 || o.src.h == 0) continue;
        SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
        if (!d.visible) continue;
        SDL_Texture* tex = image_tex(o.src.gr);
        if (!tex) continue;
        int v = st.button ? st.button(o.src.op[0]) : 0;
        int count = o.src.graphcount();
        v = std::clamp(v, 0, count - 1);
        Item it;
        it.sort = d.sort;
        it.tex = tex;
        it.src = frame_rect(o.src, v);
        it.d = d;
        items_.push_back(it);
    }
    for (const SkinObject& o : skin.sliders) {
        if (!dst_on(o.dst, st) || o.src.op[1] < 1) continue;
        int v = 0, lo = 0, hi = 1;
        if (!st.slider || !st.slider(o.src.op[2], v, lo, hi)) continue;
        // LR2: SliderByTime。値を 0..op2 の移動量にする
        double move = hi == lo ? 0 : (static_cast<double>(v) - lo) / (hi - lo) * o.src.op[1];
        float sx = 0, sy = 0;
        switch (o.src.op[0]) {
        case 0: sy = static_cast<float>(-move); break;
        case 1: sx = static_cast<float>(move); break;
        case 2: sy = static_cast<float>(move); break;
        case 3: sx = static_cast<float>(-move); break;
        default: break;
        }
        add_object(o, st, sx, sy);
    }
    for (const SkinObject& o : skin.numbers) {
        if (!dst_on(o.dst, st)) continue;
        add_numbers(o.src, o.dst, st, st.number ? st.number(o.src.op[0]) : -1, 0, 0);
    }
    for (const SkinText& t : skin.texts) {
        if (!dst_on(t.dst, st) || !st.text) continue;
        SkinDraw d = eval_dst(t.dst, st.timers->lapse(t.dst.timer, st.now_ms));
        if (!d.visible) continue;
        Item it;
        it.sort = d.sort;
        it.is_text = true;
        it.str = st.text(t.src.st);
        it.font = t.src.font;
        it.align = t.src.align;
        it.d = d;
        if (!it.str.empty()) items_.push_back(it);
    }
    for (const SkinObject& o : skin.bargraphs) {
        if (!dst_on(o.dst, st) || !st.bargraph) continue;
        double val = 0, max = 0;
        if (!st.bargraph(o.src.op[0], val, max)) continue;
        // LR2: AddDrawingBuffer_BarGraph
        if (max == 0 || val <= 0) continue;
        if (val >= max) val = max;
        double rate = val / max;
        SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
        if (o.src.op[1] == 0) d.w = static_cast<float>(d.w * rate);
        else if (o.src.op[1] == 1) d.h = static_cast<float>(d.h * rate);
        add(o.src, d, st.timers->lapse(o.src.timer, st.now_ms));
    }
}

void SkinRenderer::flush(const Lr2Skin& skin) {
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.sort < b.sort; });
    for (Item& it : items_) {
        if (it.is_text) draw_text(skin, it);
        else draw_image(it);
    }
}

// LR2: LRDrawImg（skindraw:737）
void SkinRenderer::draw_image(Item& it) {
    SkinDraw& d = it.d;
    if (d.a < 2 && d.blend != 0) return;
    if (d.blend == 1 && d.a > 254) d.blend = 0;
    if (d.h < 0) d.h = -d.h, d.y -= d.h;
    if (d.w < 0) d.w = -d.w, d.x -= d.w;
    if (d.w == 0 || d.h == 0) return;
    SDL_Texture* t = it.tex;
    // 合成方法 10：描画元の色を反転して、ふつうのαブレンドで描く（DxLib: DX_BLENDMODE_INVSRC）
    if (d.blend == 10)
        if (SDL_Texture* inv = inverted(t)) t = inv;
    SDL_SetTextureScaleMode(t, d.filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    SDL_SetTextureColorMod(t, static_cast<Uint8>(std::clamp(d.r, 0, 255)), static_cast<Uint8>(std::clamp(d.g, 0, 255)),
                           static_cast<Uint8>(std::clamp(d.b, 0, 255)));
    Uint8 alpha = static_cast<Uint8>(std::clamp(d.a, 0, 255));
    SDL_BlendMode mode = SDL_BLENDMODE_BLEND;
    switch (d.blend) {
    case 0: alpha = 255; break;
    case 2: mode = SDL_BLENDMODE_ADD; break;
    case 3:
    case 5:
        mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
                                          SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
        break;
    case 4:
    case 11: mode = SDL_BLENDMODE_MOD; break;
    case 9:
        mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_BLENDFACTOR_ZERO,
                                          SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
                                          SDL_BLENDOPERATION_ADD);
        break;
    default: break;
    }
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_SetTextureBlendMode(t, mode);
    // LR2は四捨五入した整数座標で描く
    float x1 = std::floor(d.x + 0.5f), y1 = std::floor(d.y + 0.5f);
    float x2 = std::floor(d.x + d.w + 0.5f), y2 = std::floor(d.y + d.h + 0.5f);
    SDL_FRect dst{x1, y1, x2 - x1, y2 - y1};
    if (it.backbox) {
        SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
        SDL_RenderFillRect(r_, &dst);
    }
    bool clipped = it.clip_top >= 0;
    if (clipped) {
        SDL_Rect cr{0, static_cast<int>(it.clip_top), 1 << 16, static_cast<int>(it.clip_bottom - it.clip_top)};
        SDL_SetRenderClipRect(r_, &cr);
    }
    if (d.angle != 0.0f) {
        SDL_FPoint c;
        float w = dst.w, h = dst.h;
        switch (d.center) {
        case 1: c = {0, h}; break;
        case 2: c = {w / 2, h}; break;
        case 3: c = {w, h}; break;
        case 4: c = {0, h / 2}; break;
        case 6: c = {w, h / 2}; break;
        case 7: c = {0, 0}; break;
        case 8: c = {w / 2, 0}; break;
        case 9: c = {w, 0}; break;
        default: c = {w / 2, h / 2}; break;
        }
        SDL_RenderTextureRotated(r_, t, &it.src, &dst, d.angle, &c, SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(r_, t, &it.src, &dst);
    }
    if (clipped) SDL_SetRenderClipRect(r_, nullptr);
}

// LR2: LRDrawText（skindraw:847）。画像フォントがあればそれを、無ければTTFで描く
void SkinRenderer::draw_text(const Lr2Skin& skin, Item& it) {
    SkinDraw& d = it.d;
    if (d.h == 0 || d.w == 0) return;
    Uint8 r = static_cast<Uint8>(std::clamp(d.r, 0, 255)), g = static_cast<Uint8>(std::clamp(d.g, 0, 255)),
          b = static_cast<Uint8>(std::clamp(d.b, 0, 255)), a = static_cast<Uint8>(std::clamp(d.a, 0, 255));
    SDL_BlendMode mode = d.blend == 2 ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND;
    bool use_image = it.font >= 0 && it.font < static_cast<int>(skin.image_fonts.size()) &&
                     skin.image_fonts[it.font].loaded && skin.image_fonts[it.font].size > 0;
    if (use_image) {
        const ImageFont& f = skin.image_fonts[it.font];
        const std::vector<SDL_Texture*>& texs = font_textures_[it.font];
        std::u32string u = utf32(it.str);
        auto glyph = [&](char32_t ch) -> const ImageFontChar* {
            auto p = f.chars.find(ch);
            if (p == f.chars.end() || p->second.w <= 0) p = f.chars.find(U'?');
            return p == f.chars.end() ? nullptr : &p->second;
        };
        float width = 0;
        for (char32_t ch : u)
            if (const ImageFontChar* c = glyph(ch)) width += f.kerning + c->w;
        width -= f.kerning;
        if (width <= 0) return;
        float hl = d.h / static_cast<float>(f.size);
        float wl = (width > d.w ? d.w / width : 1.0f) * hl;
        float x = d.x;
        if (it.align == 1) x -= static_cast<int>(width * wl * 0.5f);
        else if (it.align == 2) x -= static_cast<int>(width * wl);
        float sum = 0;
        for (char32_t ch : u) {
            const ImageFontChar* c = glyph(ch);
            if (!c) continue;
            float cw = c->w * wl;
            if (ch != U' ' && c->image >= 0 && c->image < static_cast<int>(texs.size()) && texs[c->image]) {
                SDL_Texture* t = texs[c->image];
                SDL_SetTextureColorMod(t, r, g, b);
                SDL_SetTextureAlphaMod(t, a);
                SDL_SetTextureBlendMode(t, mode);
                SDL_SetTextureScaleMode(t, d.filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                SDL_FRect src{static_cast<float>(c->x), static_cast<float>(c->y), static_cast<float>(c->w),
                              static_cast<float>(c->h)};
                SDL_FRect dst{std::floor(x + sum + 0.5f), std::floor(d.y + 0.5f), cw, hl * c->h};
                SDL_RenderTexture(r_, t, &src, &dst);
            }
            sum += f.kerning * wl + cw;
        }
        return;
    }
    if (it.font < 0 || it.font >= static_cast<int>(skin.ttf_fonts.size())) return;
    const TtfFont& f = skin.ttf_fonts[it.font];
    TextCache::Tex tt = text_.get(f.size, f.thick, it.str);
    if (!tt.tex || tt.w <= 0) return;
    float hl = d.h / static_cast<float>(f.size);
    float width = static_cast<float>(tt.w);
    float wl = (width > d.w ? d.w / width : 1.0f) * hl;
    float x = d.x;
    if (it.align == 1) x -= static_cast<int>(width * wl * 0.5f);
    else if (it.align == 2) x -= static_cast<int>(width * wl);
    SDL_SetTextureColorMod(tt.tex, r, g, b);
    SDL_SetTextureAlphaMod(tt.tex, a);
    SDL_SetTextureBlendMode(tt.tex, mode);
    SDL_SetTextureScaleMode(tt.tex, SDL_SCALEMODE_LINEAR);
    float tex_w = 0, tex_h = 0;
    SDL_GetTextureSize(tt.tex, &tex_w, &tex_h);
    SDL_FRect dst{x, d.y, tex_w * wl, tex_h * hl};
    SDL_RenderTexture(r_, tt.tex, nullptr, &dst);
}

}  // namespace bmj
