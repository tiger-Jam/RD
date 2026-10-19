// アプリ全体で共有するもの（ウィンドウ、音声、設定、スキン）
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>

#include "app/config.h"
#include "audio/mixer.h"
#include "db/folder_watch.h"
#include "db/song_db.h"
#include "mod/mod.h"
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
    std::unique_ptr<Lr2Skin> play_skin, result_skin, select_skin, decide_skin, courseresult_skin;
    // 最後に読んだスキン（次のスキンの #IMAGE,CONTINUE が画像を引き継ぐ。上のどれかを指す）
    const Lr2Skin* last_skin = nullptr;
    std::unique_ptr<SkinRenderer> skin_renderer, result_renderer, select_renderer, decide_renderer;
    // 曲フォルダの監視（起動の画面の前から見て、そのあいだの変化も取りこぼさない）
    FolderWatcher watcher;
    // MOD：見つけた全部（実行ファイルの横の mods/ が公式、設定フォルダの mods/ が利用者のもの）
    std::vector<ModManifest> mods;
    void load_mods();
    // 設定で使うことにしている MOD
    std::vector<const ModManifest*> active_mods() const;
    // 使っている MOD のカスタムゲージ（鍵は「MOD の id/ゲージの id」）
    std::vector<std::pair<std::string, const CustomGaugeDef*>> custom_gauges() const;
    // ゲージを前後に替える（LR2 のゲージ 6 つのあとに、使っている MOD のカスタムゲージが続く）
    void cycle_gauge(int dir);
    // 遊んでいるゲージの名前（カスタムゲージならその名前。LR2 のゲージなら空）
    std::string custom_gauge_name() const;
    // 使っている MOD のうち、プレーに影響するものの組み合わせの記録の番号（0 は MOD 無し。初めてなら曲データベースに足す）
    int mod_profile();
    int view_w = 640, view_h = 480;
    double refresh_hz = 60.0;
    bool updated_at_start = false;  // 起動の画面で曲データベースと難易度表を更新した（選曲画面では読み直さない）
    double last_draw_ms = -1, last_present_ms = 0;  // 開発用の計測
    uint64_t scene_end_ns = 0;  // 前の画面を抜けた時刻（次の画面が最初の絵を出すまでの時間を測る）

    ~App();
    bool init(const char* argv0);
    // プレイスキンを読む。options はスキン読み込み時の #IF の判定に使う値
    void load_sound_set();
    void load_play_skin(const std::array<bool, 1000>& options);
    void load_result_skin(const std::array<bool, 1000>& options);
    void load_courseresult_skin(const std::array<bool, 1000>& options);
    void load_select_skin(const std::array<bool, 1000>& options);
    void load_decide_skin(const std::array<bool, 1000>& options);
    // そのほかの画面のスキンを読む（configured が空なら LR2 の既定 default_rel。"none" なら読まない）。読めなければ nullptr
    std::unique_ptr<Lr2Skin> load_scene_skin(const std::string& configured, const char* default_rel,
                                             const std::array<bool, 1000>& options, SkinRenderer& renderer);
    // 描画の論理解像度を変える。resize_window ならウィンドウの大きさも合わせる
    void set_view(int w, int h, bool resize_window = true);
    void window_size(int skin_w, int skin_h, int& w, int& h) const;
    void save_config() const { cfg.save(cfg_path); }
    // フルスクリーンとウインドウを切り替え、次の起動もその状態で始める
    void toggle_fullscreen();
    // 設定の音量をミキサーに渡す（スライダーで変えたときも呼ぶ）
    void apply_volume() { mixer.set_volume(cfg.volume_on, cfg.vol_master, cfg.vol_key, cfg.vol_bgm); }
    // 設定のイコライザ・ピッチ・エフェクタをミキサーに渡す（スライダーやボタンで変えたときも呼ぶ）
    void apply_sound_fx() { mixer.set_sound_fx(cfg.sound_fx); }
};

}  // namespace bmj
