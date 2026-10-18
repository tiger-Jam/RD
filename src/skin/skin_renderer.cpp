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
    for (auto& v : font_textures_)
        for (SDL_Texture* t : v) if (t) SDL_DestroyTexture(t);
}

void SkinRenderer::upload(const Lr2Skin& skin) {
    for (SDL_Texture* t : textures_) if (t) SDL_DestroyTexture(t);
    textures_.clear();
    for (const SkinImage& img : skin.images) textures_.push_back(make_texture(r_, img));
    font_textures_.clear();
    for (const ImageFont& f : skin.image_fonts) {
        std::vector<SDL_Texture*> v;
        for (const SkinImage& img : f.images) v.push_back(make_texture(r_, img));
        font_textures_.push_back(std::move(v));
    }
}

SDL_Texture* SkinRenderer::image_tex(int gr) const {
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
    if (!d.visible || !src.valid) return;
    SDL_Texture* tex = image_tex(src.gr);
    if (!tex) return;
    Item it;
    it.sort = d.sort;
    it.tex = tex;
    it.src = frame_rect(src, src_frame(src, t));
    it.d = d;
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
    double k = 480.0 * st.speed_multiplier * st.hi_speed / 600.0;
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
            if (n.kind == NoteKind::Mine) {
                add_play_area(skin.mine[lane], dst, st, 0, static_cast<float>(note_y), 255);
            } else if (!is_ln) {
                add_play_area(skin.note[lane], dst, st, 0, static_cast<float>(note_y), 255);
            } else {
                int active;
                if (st.autoplay) active = (n.pos <= st.render_beat && end_pos > st.render_beat) ? 5 : 0;
                else active = judge.note_done(i) ? -1 : judge.ln_holding(i) ? 5 : 0;
                add_ln(skin.ln_start[lane], skin.ln_end[lane], skin.ln_body[lane], dst, st, static_cast<float>(note_y),
                       static_cast<float>(note_l), active);
            }
        }
    }
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
    flush(skin);
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
