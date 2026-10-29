#include "skin/skin_renderer.h"

#include "util/dev.h"

#include "game/gauge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "stb_truetype.h"

namespace rd {

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
    for (auto& [k, v] : gray_) if (v) SDL_DestroyTexture(v);
    for (auto& v : font_textures_)
        for (SDL_Texture* t : v) if (t) SDL_DestroyTexture(t);
}

void SkinRenderer::upload(const Lr2Skin& skin) {
    for (SDL_Texture* t : textures_) if (t) SDL_DestroyTexture(t);
    textures_.clear();
    for (auto& [k, v] : inverted_) SDL_DestroyTexture(v);
    inverted_.clear();
    for (auto& [k, v] : gray_) if (v) SDL_DestroyTexture(v);
    gray_.clear();
    images_.clear();
    ink_.clear();
    movies_.clear();
    for (const SkinImage& img : skin.images) {
        SDL_Texture* t = make_texture(r_, img);
        if (!img.movie.empty() && VideoDecoder::available()) {
            auto v = std::make_unique<VideoDecoder>();
            if (v->open(img.movie) && v->width() > 0) {
                v->set_loop(true);
                t = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, v->width(), v->height());
                if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                movies_[static_cast<int>(textures_.size())] = std::move(v);
            }
        }
        textures_.push_back(t), images_.push_back(&img);
    }
    font_textures_.clear();
    for (const ImageFont& f : skin.image_fonts) {
        std::vector<SDL_Texture*> v;
        for (const SkinImage& img : f.images) v.push_back(make_texture(r_, img));
        font_textures_.push_back(std::move(v));
    }
}

SDL_Texture* SkinRenderer::gray_tex(int gr) {
    if (auto it = gray_.find(gr); it != gray_.end()) return it->second;
    SDL_Texture* out = nullptr;
    if (gr >= 0 && gr < static_cast<int>(images_.size()) && images_[gr] && images_[gr]->movie.empty() && !overrides_.count(gr)) {
        SkinImage copy = *images_[gr];
        reload_skin_image(copy);  // GPU に送ったあとで画素を捨てていたら、ファイルから読み直す
        for (size_t p = 0; p + 3 < copy.rgba.size(); p += 4)
            copy.rgba[p] = copy.rgba[p + 1] = copy.rgba[p + 2] = std::max({copy.rgba[p], copy.rgba[p + 1], copy.rgba[p + 2]});
        out = make_texture(r_, copy);
    }
    gray_[gr] = out;
    return out;
}

SDL_Texture* SkinRenderer::inverted(SDL_Texture* t) {
    if (auto it = inverted_.find(t); it != inverted_.end()) return it->second;
    SDL_Texture* inv = nullptr;
    for (size_t i = 0; i < textures_.size(); ++i) {
        if (textures_[i] != t || !images_[i]) continue;
        SkinImage copy = *images_[i];
        reload_skin_image(copy);  // GPU に送ったあとで画素を捨てていたら、ファイルから読み直す
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

std::pair<float, float> SkinRenderer::ink_rows(const SkinSrc& src, int frame) {
    const SDL_FRect fr = frame_rect(src, frame);
    const std::array<int, 5> key{src.gr, static_cast<int>(fr.x), static_cast<int>(fr.y), static_cast<int>(fr.w), static_cast<int>(fr.h)};
    if (auto it = ink_.find(key); it != ink_.end()) return it->second;
    std::pair<float, float> out{0.0f, fr.h};
    if (src.gr >= 0 && src.gr < static_cast<int>(images_.size()) && images_[src.gr]) {
        SkinImage copy;
        const SkinImage* img = images_[src.gr];
        if (img->rgba.empty()) copy = *img, reload_skin_image(copy), img = &copy;  // 画素を捨てていたら読み直す
        int top = -1, bottom = -1;
        for (int y = static_cast<int>(fr.y); y < static_cast<int>(fr.y + fr.h) && y < img->h; ++y)
            for (int x = static_cast<int>(fr.x); x < static_cast<int>(fr.x + fr.w) && x < img->w; ++x) {
                const uint8_t* p = &img->rgba[(static_cast<size_t>(y) * img->w + x) * 4];
                if (p[3] > 64 && std::max({p[0], p[1], p[2]}) > 110) {
                    if (top < 0) top = y;
                    bottom = y;
                    break;
                }
            }
        if (top >= 0) out = {static_cast<float>(top) - fr.y, static_cast<float>(bottom - top + 1)};
    }
    ink_[key] = out;
    return out;
}

SDL_Texture* SkinRenderer::image_tex(int gr) const {
    if (auto it = overrides_.find(gr); it != overrides_.end()) return it->second;
    if (gr < 0 || gr >= static_cast<int>(textures_.size())) return nullptr;
    return textures_[gr];
}

// LR2: ReadSRC の DerivationGraph と同じ切り出し
void SkinRenderer::update_movies(const PlaySkinState& st) {
    if (movies_.empty() || !st.timers) return;
    double t = st.timers->lapse(0, st.now_ms);
    for (auto& [gr, v] : movies_) {
        VideoDecoder::Frame f;
        if (gr < static_cast<int>(textures_.size()) && textures_[gr] && v->take(t, f) && !f.rgba.empty())
            SDL_UpdateTexture(textures_[gr], nullptr, f.rgba.data(), f.w * 4);
    }
}

SDL_FRect SkinRenderer::frame_rect(const SkinSrc& src, int frame) const {
    // 動画と、譜面ごとに差し替える画像（STAGEFILE など）の SRC で幅・高さが -1 なら、絵の全体
    int sw = src.w, sh = src.h;
    if (sw < 0 || sh < 0) {
        if (auto it = movies_.find(src.gr); it != movies_.end()) {
            if (sw < 0) sw = it->second->width();
            if (sh < 0) sh = it->second->height();
        } else if (auto ov = overrides_.find(src.gr); ov != overrides_.end() && ov->second) {
            float tw = 0, th = 0;
            SDL_GetTextureSize(ov->second, &tw, &th);
            if (sw < 0) sw = static_cast<int>(tw);
            if (sh < 0) sh = static_cast<int>(th);
        }
    }
    if (sw < 0 || sh < 0) return SDL_FRect{0, 0, 0, 0};
    int bw = sw / src.div_x, bh = sh / src.div_y;
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
    if (tint_) {
        if (SDL_Texture* g = gray_tex(src.gr)) {
            tex = g;
            d.r = d.r * static_cast<int>((tint_ >> 16) & 0xff) / 255;
            d.g = d.g * static_cast<int>((tint_ >> 8) & 0xff) / 255;
            d.b = d.b * static_cast<int>(tint_ & 0xff) / 255;
        }
    }
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
    const int align = src.op[1];  // 0 右寄せ・1 左寄せ・2 中央
    if (dst.empty() || !src.valid || src.op[2] <= 0 || align < 0 || align > 2) return;
    // 絵の並び：10 コマ（0〜9）・11 コマ（0〜9 と空白）・24 コマ（0〜9・空白・＋ と、負の数用の 0〜9・空白・−）。
    // 何組かあれば、src のタイマーで組を送る。どれにも割り切れる数なら 24、11、10 の順に優先
    const int frames = src.graphcount();
    const int unit = frames % 24 == 0 ? 24 : frames % 11 == 0 ? 11 : frames % 10 == 0 ? 10 : 0;
    if (unit == 0) return;
    if (unit != 24 && number < 0) return;  // 符号のコマが無い絵では、負の数は出さない
    int base = 0;
    if (src.cycle > 0) base = (frames / unit) * (static_cast<int>(st.timers->lapse(src.timer, st.now_ms)) % src.cycle) / src.cycle * unit;
    SkinDraw d = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    if (!d.visible) return;
    d.x += x;
    d.y += y;
    int width = src.op[2];  // 桁数
    const int len = count_digit(number);
    if (st.grow_digits && len > width) {
        // スキンが決めた枠（桁数 × 1 文字の幅）の中に収まるよう、1 文字の幅を詰める（枠の外の絵に重ならないように）
        d.w = d.w * width / len;
        width = len;
    }
    // 桁を並べる（-1 は空白）。右寄せは 10 桁の枠に右から詰めて下の width 桁、左寄せ・中央は左から詰めて空白 10 個を足し、上の width 桁
    std::vector<int> cells;
    for (char ch : std::to_string(std::abs(number))) cells.push_back(ch - '0');
    if (align == 0) {
        cells.insert(cells.begin(), std::max(0, 10 - static_cast<int>(cells.size())), -1);
        if (static_cast<int>(cells.size()) > width) cells.erase(cells.begin(), cells.end() - width);
    } else {
        if (align == 2) d.x += std::max(0, width - len) * d.w * 0.5f;  // 中央：余った桁の半分だけ右へ
        cells.insert(cells.end(), 10, -1);
        if (static_cast<int>(cells.size()) > width) cells.resize(width);
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
    if (unit == 24) {
        put(base + (number < 0 ? 23 : 11));  // 符号
        d.x += d.w;
        if (number < 0 && frames == 24) base += 12;  // 負の数の組（組が 1 つだけの絵のとき）
    }
    for (int cell : cells) {
        if (cell >= 0) put(base + cell);
        else if (unit != 10) put(base + 10);  // 空白のコマ（10 コマの絵には無いので、場所だけ空ける）
        d.x += d.w;
    }
}

namespace {

// ノーツの高さを draw_h にする（下の端はそのまま。判定ラインにそろうのは下の端なので）
void fit_note_height(const SkinSrc& src, SkinDraw& d, float size_y) {
    if (src.draw_h <= 0) return;
    const float h = src.draw_h + size_y;
    d.y += d.h - h;
    d.h = h;
}

}  // namespace

// レーンの DST の場所に置く（ノーツ・小節線・判定ライン）。大きさの調整は中心を動かさずに足し、そのあとずらす。
// フィルタは無し、合成はアルファ
void SkinRenderer::put_on_lane(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st, const LanePlacement& at) {
    if (dst.empty() || !src.valid) return;
    SkinDraw d = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    d.w += at.grow_w, d.h += at.grow_h;
    d.x -= at.grow_w * 0.5f, d.y -= at.grow_h * 0.5f;
    d.x += at.dx;
    d.y += at.dy;
    d.filter = 0;
    d.blend = 1;
    if (at.alpha) d.a = *at.alpha;
    fit_note_height(src, d, at.grow_h);
    add(src, d, src_time(src, dst, st));
}

// LN：始点（head_y）と終点（tail_y）のノーツと、そのあいだの中身。中身は終点のノーツの下の端から始点のノーツの上の端まで
void SkinRenderer::add_ln(const LnParts& parts, const SkinDst& dst, const PlaySkinState& st, float head_y, float tail_y, LnLook look,
                          float dx, float grow_w, float grow_h) {
    if (dst.empty() || !parts.head->valid || !parts.tail->valid || !parts.body->valid) return;
    // 始点と終点は中身より手前に重ねる（始点がいちばん手前）
    constexpr float kTailLayer = 1, kHeadLayer = 2;
    SkinDraw lane = eval_dst(dst, st.timers->lapse(dst.timer, st.now_ms));
    lane.w += grow_w, lane.h += grow_h;
    lane.x -= grow_w * 0.5f - dx, lane.y -= grow_h * 0.5f;
    auto put = [&](const SkinSrc& src, SkinDraw d) {
        d.blend = 1;
        d.a = look == LnLook::Done ? 128 : 255;
        add(src, d, look == LnLook::Still ? -1.0 : src_time(src, dst, st));
    };
    const float tail_bottom = tail_y + lane.h;
    SkinDraw body = lane;
    body.y += tail_bottom;
    body.h = head_y - tail_bottom;
    // 始点のノーツの高さを変えていれば、その上の端は下がる（中身をそこまで伸ばしてすき間を作らない）
    if (parts.head->draw_h > 0) body.h += lane.h - (parts.head->draw_h + grow_h);
    put(*parts.body, body);
    SkinDraw head = lane;
    head.sort += kHeadLayer;
    head.y += head_y;
    fit_note_height(*parts.head, head, grow_h);
    put(*parts.head, head);
    SkinDraw tail = lane;
    tail.sort += kTailLayer;
    tail.y += tail_y;
    fit_note_height(*parts.tail, tail, grow_h);
    put(*parts.tail, tail);
}

// 1P 側（スキンのレーン番号 0..7）のノーツ・小節線
void SkinRenderer::add_notes(const Lr2Skin& skin, const PlaySkinState& st) {
    const Chart& chart = *st.chart;
    const JudgeEngine& judge = *st.judge;
    // 1拍=480（LR2の1小節=1920）。y = (今 − ノーツ) × ハイスピ / 600
    const double px_per_beat = 480.0 * st.speed_multiplier * st.hi_speed * (st.base_speed / 100.0) / 600.0;
    // これより上（鍵盤 1 のノーツの高さの分だけ画面の上の外）に来たノーツから先は描かない
    const float visible_top = skin.note_dst[1].empty() ? 0.0f : -skin.note_dst[1].keys.front().h;
    // 判定ラインから上へのずれ（画素。判定ラインより下には行かない）に、LIFT と位置の調整を入れたもの
    auto px_above_judge = [&](double pos) {
        const int rise = std::min(0, static_cast<int>((st.render_beat - pos) * px_per_beat));
        return rise - static_cast<int>(st.lift_px) + st.adjust.note_y + st.adjust.note_fine_y;
    };
    LanePlacement at;
    at.dx = static_cast<float>(st.adjust.note_x);
    at.grow_w = static_cast<float>(st.adjust.size_x);
    at.alpha = 255;

    // 小節線（高さの調整はしない）
    if (!skin.line[0].dst.empty()) {
        const float line_y = skin.line[0].dst.keys.front().y;
        for (const BarLine& bar : chart.bars) {
            if (bar.pos < st.render_beat) continue;
            const float y = static_cast<float>((st.render_beat - bar.pos) * px_per_beat) - st.lift_px;
            if (line_y + y <= visible_top) break;
            at.dy = y + st.adjust.note_y + st.adjust.note_fine_y;
            put_on_lane(skin.line[0].src, skin.line[0].dst, st, at);
        }
    }
    at.grow_h = static_cast<float>(st.adjust.size_y);

    // HIDDEN / SUDDEN（LR2: Play.cpp:328）。判定ラインまでの高さに対する割合で、見せる縦の範囲を決める。
    // OpenLR2 はここでスキンの行番号（sortID）を高さとして使っているが、移植の誤りと見て判定ラインの位置を使う
    struct Band {
        float top, bottom;
        bool to_screen_bottom;  // 下は画面の下の端まで
    };
    static constexpr Band kBands[3] = {{0.0f, 0.6f, false}, {0.6f, 0.0f, true}, {0.55f, 0.65f, false}};
    for (int lane = 0; lane < kLaneCount; ++lane) {
        const SkinDst& dst = skin.note_dst[lane];
        if (dst.empty()) continue;
        const float judge_y = dst.keys.front().y;
        clip_top_ = clip_bottom_ = -1;
        if (st.effect >= 1 && st.effect <= 3) {
            const Band& b = kBands[st.effect - 1];
            clip_top_ = b.top > 0 ? judge_y * b.top : 0.0f;
            clip_bottom_ = b.to_screen_bottom ? static_cast<float>(skin.height) : judge_y * b.bottom;
        }
        for (size_t i = 0; i < chart.notes.size(); ++i) {
            const Note& n = chart.notes[i];
            if (n.lane != lane || n.kind == NoteKind::Invisible) continue;
            const bool is_ln = n.kind == NoteKind::Long;
            const double end_pos = is_ln ? n.end_pos : n.pos;
            // 判定が終わって、判定ラインも過ぎたものは描かない
            if (judge.note_done(i) && n.pos <= st.render_beat && end_pos <= st.render_beat) continue;
            const double rise = (st.render_beat - n.pos) * px_per_beat;
            if (n.pos > st.render_beat && judge_y + rise - st.lift_px <= visible_top) break;  // 画面より上
            const float y = static_cast<float>(px_above_judge(n.pos));
            if (!is_ln) {
                at.dy = y;
                put_on_lane(n.kind == NoteKind::Mine ? skin.mine[lane] : skin.note[lane], dst, st, at);
                continue;
            }
            // LN の見た目：押さえている間は絵を動かす。判定が終わったもの（取り逃がし・途中で離した）は半透明
            const bool holding = st.autoplay ? n.pos <= st.render_beat && end_pos > st.render_beat : judge.ln_holding(i);
            const LnLook look = !st.autoplay && judge.note_done(i) ? LnLook::Done : holding ? LnLook::Moving : LnLook::Still;
            const LnParts parts{&skin.ln_start[lane], &skin.ln_end[lane], &skin.ln_body[lane]};
            add_ln(parts, dst, st, y, static_cast<float>(px_above_judge(end_pos)), look, at.dx, at.grow_w, at.grow_h);
        }
    }
    clip_top_ = clip_bottom_ = -1;
}

// LR2: AddDrawingBuffer_Gauge（skindraw:389）
void SkinRenderer::add_gauge(const SkinObject& o, const PlaySkinState& st) {
    if (o.dst.empty() || !o.src.valid) return;
    SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
    if (!d.visible) return;
    // ゲージは 50 個の目盛り（1 個 2%）。いまの値のすぐ下の 0〜2 個は、毎フレームちらつかせる（消えて見える）。
    // ちらつきの乱数は、描けない絵でも進める（同じ画面のほかのゲージと同じ並びにする）
    constexpr int kSegments = 50;
    const int filled_to = static_cast<int>(st.gauge_hp) / 2;
    flicker_ = flicker_ * 1103515245u + 12345u;
    const int flicker_from = filled_to - static_cast<int>((flicker_ >> 16) % 3);
    SDL_Texture* tex = image_tex(o.src.gr);
    if (!tex) return;
    // 絵のコマは 4 つで 1 組（0 線より上で点いている・1 線より下で点いている・2 線より上で消えている・3 線より下で消えている）。
    // 組が複数あれば、src のタイマーで順に送る（光って流れるゲージ）
    const int frames = o.src.graphcount();
    const int sets = frames / 4;
    int set_base = 0;
    if (sets > 1 && o.src.cycle > 0) {
        const int phase = static_cast<int>(st.timers->lapse(o.src.timer, st.now_ms)) % o.src.cycle;
        set_base = sets * phase / o.src.cycle * 4;
    }
    const int border = std::clamp(static_cast<int>(std::lround(st.gauge_border / 2)), 1, kSegments);  // クリアの線（80% なら 40 個目から上）
    for (int seg = 1; seg <= kSegments; ++seg) {
        const bool steady = seg < flicker_from || seg >= filled_to || seg == 1;  // 1 個目はちらつかない
        const bool lit = seg <= filled_to && steady;
        // 落ちたら終わりのゲージは、線の上下を分けない
        const bool below_border = !st.gauge_survival && seg < border;
        const int frame = set_base + (lit ? 0 : 2) + (below_border ? 1 : 0);
        Item it;
        it.sort = d.sort;
        it.tex = tex;
        it.src = frame_rect(o.src, std::min(frame, frames - 1));
        it.d = d;
        // カスタムゲージの色：線より上の目盛り（落ちたら終わりのゲージは全部）と、線より下の目盛りで色を分ける
        if (const uint32_t tint = below_border ? st.gauge_tint_low : st.gauge_tint) {
            if (SDL_Texture* g = gray_tex(o.src.gr)) {
                it.tex = g;
                it.d.r = d.r * static_cast<int>((tint >> 16) & 0xff) / 255;
                it.d.g = d.g * static_cast<int>((tint >> 8) & 0xff) / 255;
                it.d.b = d.b * static_cast<int>(tint & 0xff) / 255;
            }
        }
        items_.push_back(it);
        d.x += o.src.op[0];
        d.y += o.src.op[1];
    }
}

// LR2: DrawJudgeCombo / AddDrawingBuffer_JudgeCombo（skindraw:697）
void SkinRenderer::add_judge_combo(const Lr2Skin& skin, const PlaySkinState& st) {
    int judge = std::clamp(st.judge_draw, 0, 5);
    int combo = judge < 3 ? 0 : st.combo_draw;
    const SkinObject* jp = &skin.nowjudge[0][judge];
    const SkinObject* cp = &skin.nowcombo[0][judge];
    if (jp->dst.empty()) return;
    // 判定の表示の大きさ（RD）：判定の文字は真ん中を中心に、コンボの数字は判定の文字からの位置ごと大きくする
    SkinObject js, cs;
    if (st.adjust.judge_scale != 100) {
        const float s = st.adjust.judge_scale / 100.0f;
        js = *jp, cs = *cp;
        for (SkinDstKey& k : js.dst.keys) k.x += k.w * (1 - s) / 2, k.y += k.h * (1 - s) / 2, k.w *= s, k.h *= s;
        for (SkinDstKey& k : cs.dst.keys) k.x *= s, k.y *= s, k.w *= s, k.h *= s;
        jp = &js, cp = &cs;
    }
    const SkinObject& j = *jp;
    const SkinObject& c = *cp;
    const int digits = combo > 0 ? count_digit(combo) : 0;
    const double jt = st.timers->lapse(j.dst.timer, st.now_ms);
    const float digit_w = eval_dst(c.dst, jt).w;
    // 判定の SRC の op1 が 0 なら、判定の文字ごと左へずらして「文字＋数字」をまとめて真ん中にする。
    // そうでなければ文字は動かさず、数字だけを（1 桁ぶん引いた幅の）真ん中にする
    const bool shift_together = j.src.op[0] == 0 && combo > 0;
    const float judge_dx = shift_together ? -(digits * digit_w * 0.5f) : 0.0f;
    const float number_dx = shift_together ? judge_dx : -((digits - 1) * digit_w * 0.5f);
    // 判定の表示の位置の調整（LR2: Play.cpp:710 の judge_x / judge_y）
    const float ax = static_cast<float>(st.adjust.judge_x), ay = static_cast<float>(st.adjust.judge_y);
    add_object(j, st, judge_dx + ax, -st.lift_px + ay);
    if (combo > 0) {
        const SkinDraw jd = eval_dst(j.dst, jt);
        add_numbers(c.src, c.dst, st, combo, number_dx + jd.x + ax, jd.y - st.lift_px + ay);
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
    update_movies(st);
    adjust_ = st.adjust;
    // 集める順はLR2と同じ（ProcI_Play → メインループの各オブジェクト）。最後に描画順で並べ替える
    add_gauge(skin.gauge[0], st);
    if (st.notes_visible && st.chart && st.judge) {
        add_notes(skin, st);
        if (st.show_judge) add_judge_combo(skin, st);
    }
    // 判定ラインも LIFT の分だけ上げる（OpenLR2 と同じ）
    if (!skin.judgeline[0].dst.empty()) {
        LanePlacement at;
        at.dx = static_cast<float>(st.adjust.note_x);
        at.dy = static_cast<float>(st.adjust.note_y) - st.lift_px;
        at.grow_w = static_cast<float>(st.adjust.size_x);
        at.grow_h = static_cast<float>(st.adjust.size_y);
        put_on_lane(skin.judgeline[0].src, skin.judgeline[0].dst, st, at);
    }
    add_common(skin, st);
    flush(skin);
    adjust_ = SkinAdjust{};
}

// LR2: Proc_Result（Scene05_Result.cpp:30）。ゲージの推移グラフは80%未満とそれ以上で画像を描き分ける
void SkinRenderer::add_gauge_chart(const Lr2Skin& skin, const PlaySkinState& st) {
    const auto& chart = st.result_flip ? skin.gaugechart_2p : skin.gaugechart;
    const SkinObject& lo = chart[0];
    const SkinObject& hi = chart[1];
    if (!st.graph || lo.src.op[0] <= 0 || lo.dst.empty()) return;
    const PlayGraph& g = *st.graph;
    int gauge = std::clamp(st.graph_gauge, 0, kGaugeCount - 1);
    // GROOVE/EASY は 80%（カスタムゲージはそのクリアの線）で描き分ける。コースの中では GROOVE/EASY も落ちたら終わりのゲージなので、HARD と同じく分けない
    // （遊んでいる間のゲージの色とそろえる）
    bool groove = (gauge == kGroove || gauge == kEasy) && !st.gauge_survival;
    double t0 = st.timers->lapse(0, st.now_ms);
    int length = static_cast<int>(st.chart_length(lo.src.op[0], t0));
    int step = std::max(1, static_cast<int>(lo.dst.keys.front().w));
    int val = g.hp[gauge][0];
    for (int i = 0; i < length; i += step) {
        int axis = std::min(PlayGraph::kPoints - 1, i * 1000 / lo.src.op[0]);
        int last = 0;
        // 前の点との間は、1%ずつ縦に点を打って線にする
        while (val != g.hp[gauge][axis]) {
            const SkinObject& o = (groove && val < st.gauge_border) ? lo : hi;
            int y = o.src.op[1] * val / -100;
            if (last != y) {
                tint_ = groove && val < st.gauge_border ? st.gauge_tint_low : st.gauge_tint;
                add_object(o, st, static_cast<float>(i), static_cast<float>(y));
                tint_ = 0;
                last = y;
            }
            val += val < g.hp[gauge][axis] ? 1 : -1;
        }
        const bool low = groove && g.hp[gauge][axis] < st.gauge_border;
        const SkinObject& o = low ? lo : hi;
        tint_ = low ? st.gauge_tint_low : st.gauge_tint;
        add_object(o, st, static_cast<float>(i), static_cast<float>(o.src.op[1] * g.hp[gauge][axis] / -100));
        tint_ = 0;
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
    update_movies(st);
    items_.clear();
    add_gauge_chart(skin, st);
    add_score_chart(skin, st);
    add_common(skin, st);
    flush(skin);
}

// 選曲画面（docs/lr2_select_spec.md）
void SkinRenderer::draw_select(const Lr2Skin& skin, const PlaySkinState& st) {
    update_movies(st);
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
        // スキンに無い番号のランプ（PERFECT）は FULL COMBO の絵で出す
        int lamp = bar.lamp >= 0 && bar.lamp < 10 && !b.lamp[bar.lamp].src.valid ? skin_lamp(bar.lamp) : bar.lamp;
        tint_ = bar.lamp_tint;
        if (lamp >= 0 && lamp < 10 && b.lamp[lamp].src.valid) add_object(b.lamp[lamp], st, d.x, d.y);
        tint_ = 0;
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
    // 描画の制限（LR2: Main.cpp:1917）。DARK 1 はタイマーの付いた画像だけ、DARK 2 は判定や閉店などの演出の画像だけを描き、
    // BGA・ボタン・スライダー・数字・バーグラフも描かない
    const int dark = st.adjust.dark;
    auto dark_image = [&](int timer) {
        if (dark == 1) return timer != 0;
        if (dark == 2) return timer == 2 || timer == 3 || timer == 48 || timer == 49 || (timer >= 50 && timer < 70) || timer == 140;
        return true;
    };
    // LIFT：判定ラインの上で光るもの（ボム・ホールドの光・判定ラインの光）と、レーンの上に伸びるキービームも上げる。
    // キーを押したときの画像でも、判定ラインより下にある小さいもの（鍵盤の絵）は上げない（OpenLR2 と同じ）
    const float judge_y = skin.judgeline[0].dst.keys.empty() ? 0.0f : skin.judgeline[0].dst.keys.back().y;
    auto lift_of = [&](const SkinObject& o) -> float {
        if (st.lift_px <= 0 || o.dst.keys.empty()) return 0;
        const int t = o.dst.timer;
        if (t == 48 || (t >= 50 && t < 60) || (t >= 70 && t < 80)) return -st.lift_px;
        if ((t >= 100 && t < 110) || (t >= 120 && t < 130)) {
            const float h0 = o.dst.keys.front().h, hn = o.dst.keys.back().h;
            const bool small = std::abs(h0) < 100 && std::abs(hn) < 100;
            if (small && !skin.judgeline[0].dst.keys.empty() && o.dst.keys.front().y >= judge_y) return 0;
            return -st.lift_px;
        }
        return 0;
    };
    for (const SkinObject& o : skin.image_objs) {
        if (!dst_on(o.dst, st) || !dark_image(o.dst.timer)) continue;
        if (o.dst.op[3] == 1) {
            // 皿の回転（LR2: AddDrawingBuffer_Scratch）
            SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
            d.angle = st.scratch_angle;
            add(o.src, d, src_time(o.src, o.dst, st));
        } else {
            add_object(o, st, 0, lift_of(o));
        }
    }
    // BGA（LR2: Main.cpp:2045）。ミスした直後はPOORの層だけ、それ以外は一番下の層と重ねる層
    if (st.notes_visible && dark != 2) {
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
        if (dark == 2 || !dst_on(o.dst, st) || !o.src.valid || o.src.w == 0 || o.src.h == 0) continue;
        SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
        if (!d.visible) continue;
        const int extra = st.button_extra_frame ? st.button_extra_frame(o.src.op[0]) : -1;
        const bool has_extra = extra >= 0 && extra < o.src.graphcount();
        if (st.button_label && !has_extra) {
            // スキンの絵に無い値：同じ場所に名前を書く（書体は曲名のバーのもの）
            const std::string label = st.button_label(o.src.op[0]);
            if (!label.empty()) {
                Item it;
                it.sort = d.sort;
                it.is_text = true;
                it.str = label;
                it.font = skin.bar.title[0].src.font;
                it.align = 1;
                // ボタンの絵の中の文字と同じ大きさ・高さに書く。曲名の書体は字の上下に余白があるので、
                // 字の高さの 2.4 倍の大きさで書くと、大文字の高さがそろう（W-MIX の下の欄で合わせた値）
                const int frame = std::clamp(st.button ? st.button(o.src.op[0]) : 0, 0, o.src.graphcount() - 1);
                const auto [ink_top, ink_h] = ink_rows(o.src, frame);
                const float fh = frame_rect(o.src, frame).h;
                const float scale = fh > 0 ? d.h / fh : 1.0f;
                it.d = d;
                it.d.x = d.x + d.w / 2;
                it.d.h = ink_h * scale * 2.4f;
                it.d.y = d.y + (ink_top + ink_h / 2) * scale - it.d.h / 2;
                items_.push_back(it);
                continue;
            }
        }
        SDL_Texture* tex = image_tex(o.src.gr);
        if (!tex) continue;
        int v = has_extra ? extra : st.button ? st.button(o.src.op[0]) : 0;
        int count = o.src.graphcount();
        v = std::clamp(v, 0, count - 1);
        Item it;
        it.sort = d.sort;
        it.tex = tex;
        it.src = frame_rect(o.src, v);
        it.d = d;
        // 染める（足したコマの白い字をゲージの色に、今回のクリアをランプの色に）
        if (const uint32_t tint = st.button_tint ? st.button_tint(o.src.op[0], has_extra) : 0) {
            if (SDL_Texture* g = gray_tex(o.src.gr)) {
                it.tex = g;
                it.d.r = d.r * static_cast<int>((tint >> 16) & 0xff) / 255;
                it.d.g = d.g * static_cast<int>((tint >> 8) & 0xff) / 255;
                it.d.b = d.b * static_cast<int>(tint & 0xff) / 255;
            }
        }
        items_.push_back(it);
    }
    for (const SkinObject& o : skin.sliders) {
        if (dark == 2 || !dst_on(o.dst, st) || o.src.op[1] < 1) continue;
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
    // LIFT のカバー：スキンに LIFT のスライダー（27）が無ければ、レーンカバー（スライダー 4）の絵を、
    // 上げた判定ラインから元の判定ラインまでのところに敷く（SUDDEN+ と同じ絵。はみ出す分は切る）
    bool has_lift_slider = false;
    for (const SkinObject& o : skin.sliders) has_lift_slider = has_lift_slider || o.src.op[2] == 27;
    if (st.lift_px > 0 && dark != 2 && !has_lift_slider && !skin.judgeline[0].dst.keys.empty()) {
        for (const SkinObject& o : skin.sliders) {
            if (o.src.op[2] != 4 || o.dst.keys.empty()) continue;
            SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
            if (!d.visible) continue;
            // 判定ラインの絵の下の端から下を覆う（判定ラインはカバーの上に見える）
            const float bottom = judge_y + skin.judgeline[0].dst.keys.back().h + static_cast<float>(st.adjust.note_y),
                        top = bottom - st.lift_px;
            d.y = top;
            clip_top_ = top, clip_bottom_ = bottom;
            add(o.src, d, src_time(o.src, o.dst, st));
            clip_top_ = clip_bottom_ = -1;
            break;
        }
    }
    for (const SkinObject& o : skin.numbers) {
        if (dark == 2 || !dst_on(o.dst, st)) continue;
        add_numbers(o.src, o.dst, st, st.number ? st.number(o.src.op[0]) : -1, 0, 0);
        if (o.src.op[0] == 14) {
            // 開発用：レーンカバーの数字を描いた値が変わったら書き出す（RD_DEBUG_INPUT）
            static const bool debug = dev_env("RD_DEBUG_INPUT") != nullptr;
            static int last = -1000;
            const int v = st.number ? st.number(14) : -1;
            if (debug && v != last) std::printf("カバーの数字を %d で描いた（%.1f ms）\n", v, st.now_ms), std::fflush(stdout), last = v;
        }
        if (!st.number_after) continue;
        for (const PlaySkinState::NumberExtra& e : st.number_after(o.src.op[0])) {
            if (e.digits > 0) {
                SkinSrc s = o.src;
                s.op[2] = e.digits;
                add_numbers(s, o.dst, st, e.value, e.dx, e.dy);
            } else if (e.image.valid) {
                SkinDraw d = eval_dst(o.dst, st.timers->lapse(o.dst.timer, st.now_ms));
                d.x += e.dx, d.y += e.dy;
                d.w = static_cast<float>(e.image.w), d.h = static_cast<float>(e.image.h);
                add(e.image, d, 0);
            }
        }
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
        if (dark == 2 || !dst_on(o.dst, st) || !st.bargraph) continue;
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
    view_w_ = skin.width;
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.sort < b.sort; });
    // スキン全体の位置と拡大率（LR2: LRDraw。位置を足してから、原点を中心に拡大する）
    if (!adjust_.is_default()) {
        const float rx = adjust_.rate_x / 100.0f, ry = adjust_.rate_y / 100.0f;
        for (Item& it : items_) {
            it.d.x = (it.d.x + adjust_.shift_x) * rx, it.d.y = (it.d.y + adjust_.shift_y) * ry;
            it.d.w *= rx, it.d.h *= ry;
            if (it.clip_top >= 0) it.clip_top = (it.clip_top + adjust_.shift_y) * ry, it.clip_bottom = (it.clip_bottom + adjust_.shift_y) * ry;
        }
    }
    if (record_) {
        // 絵は、読み込んだ順の番号で書く（テクスチャの場所は毎回違うので）
        auto tex_no = [&](SDL_Texture* t) {
            for (size_t i = 0; i < textures_.size(); ++i)
                if (textures_[i] == t) return static_cast<int>(i);
            return t ? -2 : -1;
        };
        char buf[256];
        for (const Item& it : items_) {
            const SkinDraw& d = it.d;
            std::snprintf(buf, sizeof(buf), "%s %d %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f %d %d %d %d %d %d %.2f %d %.1f %.1f %d %d %d ",
                          it.is_text ? "T" : "I", it.is_text ? it.font : tex_no(it.tex), it.src.x, it.src.y, it.src.w, it.src.h, d.x, d.y,
                          d.w, d.h, d.a, d.r, d.g, d.b, d.blend, d.filter, d.angle, d.center, it.clip_top, it.clip_bottom, it.align,
                          it.backbox ? 1 : 0, static_cast<int>(it.sort));
            *record_ += buf + it.str + "\n";
        }
        *record_ += "--\n";
    }
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
        // 幅はスキンの幅にする（大きすぎる値は Windows の Direct3D で効かないことがある）
        SDL_Rect cr{0, static_cast<int>(it.clip_top), view_w_, static_cast<int>(it.clip_bottom - it.clip_top)};
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
    SDL_SetTextureAlphaMod(tt.tex, a);
    SDL_SetTextureBlendMode(tt.tex, mode);
    SDL_SetTextureScaleMode(tt.tex, SDL_SCALEMODE_LINEAR);
    float tex_w = 0, tex_h = 0;
    SDL_GetTextureSize(tt.tex, &tex_w, &tex_h);
    SDL_FRect dst{x, d.y, tex_w * wl, tex_h * hl};
    // #FONT の種類 1・3 は縁取り（DxLib の EDGE。縁は黒）。周りに 1 画素ずらして黒で描いてから本体を描く
    if (f.type == 1 || f.type == 3) {
        SDL_SetTextureColorMod(tt.tex, 0, 0, 0);
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                SDL_FRect e{dst.x + ox, dst.y + oy, dst.w, dst.h};
                SDL_RenderTexture(r_, tt.tex, nullptr, &e);
            }
    }
    SDL_SetTextureColorMod(tt.tex, r, g, b);
    SDL_RenderTexture(r_, tt.tex, nullptr, &dst);
}

}  // namespace rd
