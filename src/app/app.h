// アプリ全体で共有するもの（ウィンドウ、音声、設定、スキン）
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include "app/config.h"
#include "audio/mixer.h"
#include "db/song_db.h"
#include "input/keyconfig.h"
#include "skin/lr2_skin.h"
#include "skin/skin_renderer.h"

namespace bmj {

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Mixer mixer;
    SongDb db;
    KeyConfig keycfg = KeyConfig::lr2_default();
    AppConfig cfg;
    std::string cfg_path;
    std::string lr2_root;  // 実際に使うLR2のフォルダ（見つからなければ空）
    std::unique_ptr<Lr2Skin> play_skin, result_skin;
    std::unique_ptr<SkinRenderer> skin_renderer, result_renderer;
    int view_w = 640, view_h = 480;
    double refresh_hz = 60.0;

    ~App();
    bool init(const char* argv0);
    // プレイスキンを読む。options はスキン読み込み時の #IF の判定に使う値
    void load_play_skin(const std::array<bool, 1000>& options);
    void load_result_skin(const std::array<bool, 1000>& options);
    // 描画の論理解像度を変える。resize_window ならウィンドウの大きさも合わせる
    void set_view(int w, int h, bool resize_window = true);
    void save_config() const { cfg.save(cfg_path); }
};

}  // namespace bmj
