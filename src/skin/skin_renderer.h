// LR2スキンの描画。挙動はOpenLR2の LR2/LR2_skindraw.cpp（AddDrawingBuffer_* と LRDrawImg / LRDrawText）と、
// LR2/Scene04_Play.cpp の DrawNotes / DrawJudgeCombo / DrawHPgauge に合わせている。
// 描画はすべてメインスレッドから呼ぶ（テクスチャの作成もここで行う）。
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "chart/chart.h"
#include "game/judge.h"
#include "skin/lr2_skin.h"

namespace bmj {

// スキンのタイマー（LR2: Timer の clock[]）。開始時刻をmsで持ち、-1 は停止中
class SkinTimers {
public:
    SkinTimers() { start_.fill(-1.0); }
    void set(int id, double now_ms) { if (valid(id)) start_[id] = now_ms; }
    void reset(int id) { if (valid(id)) start_[id] = -1.0; }
    bool on(int id) const { return valid(id) && start_[id] >= 0.0; }
    // 経過ms。停止中は -1（LR2: GetTimeLapse）
    double lapse(int id, double now_ms) const {
        if (id == 140) return rhythm;
        if (!valid(id) || start_[id] < 0.0) return -1.0;
        return now_ms - start_[id];
    }
    double rhythm = -1.0;  // タイマー140（1拍=1000）

private:
    static bool valid(int id) { return id >= 0 && id < 500; }
    std::array<double, 500> start_;
};

// プレイ画面の描画に必要な、その時点のゲームの状態
struct PlaySkinState {
    double now_ms = 0;  // シーンの時刻（タイマーの基準）
    const SkinTimers* timers = nullptr;
    std::function<bool(int)> option;      // LR2: GetOptionFlag_dst
    std::function<int(int)> number;       // LR2: SetObjectValue_Num（無ければ -1）
    std::function<bool(int, double&, double&)> bargraph;  // 種類 → 値と最大値
    std::function<bool(int, int&, int&, int&)> slider;    // 種類 → 値, 最小, 最大
    std::function<std::string(int)> text; // LR2の文字列ID → 文字列

    const Chart* chart = nullptr;
    const JudgeEngine* judge = nullptr;
    bool notes_visible = false;  // タイマー41が動いているか
    double render_beat = 0;      // 描画用のスクロール位置（拍）
    double hi_speed = 100;
    double speed_multiplier = 1.0;
    bool autoplay = false;
    int judge_draw = 0;          // 直近の判定（0=空POOR..5=PGREAT）
    int combo_draw = 0;
    double gauge_hp = 0;         // 表示用のゲージ（%）
    bool gauge_survival = false;
    float scratch_angle = 0;     // 皿の回転（度）
};

class SkinRenderer {
public:
    explicit SkinRenderer(SDL_Renderer* r) : r_(r) {}
    ~SkinRenderer();
    // スキンの画像をテクスチャにする（読み込み直後に1回）
    void upload(const Lr2Skin& skin);
    void draw_play(const Lr2Skin& skin, const PlaySkinState& st);
    void set_font_path(const std::string& dir) { font_dir_ = dir; }

private:
    struct Item {
        float sort = 0;
        SDL_Texture* tex = nullptr;
        SDL_FRect src{};
        SkinDraw d;
        // 文字のとき
        bool is_text = false;
        std::string str;
        int font = 0;
        int align = 0;
    };

    SDL_Texture* image_tex(int gr) const;
    SDL_FRect frame_rect(const SkinSrc& src, int frame) const;
    void add(const SkinSrc& src, SkinDraw d, double src_time);
    void add_object(const SkinObject& o, const PlaySkinState& st, float dx, float dy);
    void add_numbers(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st, int number, float x, float y);
    void add_play_area(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st, float sx, float sy, int alpha);
    void add_ln(const SkinSrc& s, const SkinSrc& e, const SkinSrc& b, const SkinDst& dst, const PlaySkinState& st,
                float shift_y, float long_y, int active);
    void add_notes(const Lr2Skin& skin, const PlaySkinState& st);
    void add_gauge(const SkinObject& o, const PlaySkinState& st);
    void add_judge_combo(const Lr2Skin& skin, const PlaySkinState& st);
    void flush(const Lr2Skin& skin);
    void draw_image(Item& it);
    void draw_text(const Lr2Skin& skin, Item& it);
    double src_time(const SkinSrc& src, const SkinDst& dst, const PlaySkinState& st) const;
    bool dst_on(const SkinDst& d, const PlaySkinState& st) const;

    // 文字列を1枚のテクスチャにしたもの（TTF）
    struct TextTex {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    TextTex ttf_text(const TtfFont& f, const std::string& s);
    const std::vector<uint8_t>* font_data(int thick);

    SDL_Renderer* r_;
    std::vector<SDL_Texture*> textures_;
    std::vector<std::vector<SDL_Texture*>> font_textures_;  // 画像フォントごとの画像
    std::vector<Item> items_;
    std::map<std::string, TextTex> text_cache_;
    std::map<std::string, std::vector<uint8_t>> font_files_;
    std::string font_dir_ = "/System/Library/Fonts";
    uint32_t rand_ = 12345;
};

}  // namespace bmj
