// アプリ全体で共有するもの（ウィンドウ、音声、設定、スキン）
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <map>
#include <memory>
#include <random>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/config.h"
#include "audio/mixer.h"
#include "db/difficulty_table.h"
#include "db/folder_watch.h"
#include "db/song_db.h"
#include "mod/mod.h"
#include "input/keyconfig.h"
#include "skin/lr2_skin.h"
#include "skin/skin_renderer.h"

namespace bmj {

// LR2のフォルダ（LR2files があるところ）を探す。configured（設定の値）を最初に見る。見つからなければ空
std::string find_lr2_root(const std::string& configured, const char* argv0);

// #RESOLUTION の無いスキンの解像度を、画面全体を覆う DST から推定する
void guess_resolution(Lr2Skin& sk);

// 起動のときに始めた難易度表の確かめ（取りに行くのはその日の最初だけ）。応答の遅いサイトがあっても起動の画面では待ちきらず、
// 残りは裏で続ける。間に合わなかった表が新しくなったら、選曲画面が一覧を作り直す
struct TableUpdates {
    std::mutex mu;
    std::vector<std::optional<DifficultyTable>> got;  // 登録の順。終わったものから入る
    std::atomic<int> remaining{0};
    std::atomic<bool> cut{false};           // 起動の画面が待つのをやめた（このあとに終わったものは「遅れて」）
    std::atomic<bool> late_changed{false};  // 遅れて終わった表に、新しくなったものがある
};

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Mixer mixer;
    SongDb db;
    KeyConfig keycfg = KeyConfig::lr2_default();
    AppConfig cfg;
    std::mt19937 bgm_rng_{std::random_device{}()};  // BGM のセットの選び方
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
    // 使っている MOD のカスタムランプ（鍵は「MOD の id/ランプの id」）
    std::map<std::string, const CustomLampDef*> custom_lamps() const;
    // 本体のランプ（1 FAILED … 6 PERFECT）を染める色（0xff000000 | RGB。0 ならスキンの絵のまま）。ms はその時刻（点滅）
    uint32_t standard_lamp_tint(int lamp, double ms) const;
    // 本体のゲージ（kGroove …）の色の上書き（使っている MOD のうち最初に変えているもの。無ければ nullptr）
    const GaugeColors* standard_gauge_colors(int gauge_type) const;
    // ゲージを前後に替える（LR2 のゲージ 6 つのあとに、使っている MOD のカスタムゲージが続く）
    void cycle_gauge(int dir);
    // 遊んでいるゲージの名前（カスタムゲージならその名前。LR2 のゲージなら空）
    std::string custom_gauge_name() const;
    // GAS の MOD を使っていれば、落ちていくゲージの順番（使っていなければ空）。GAUGE で「GAS」を選んだときに使う
    std::vector<std::string> gas_order() const;
    // 使っている MOD のプレースキンの部品の色（MOD の並びの順）
    std::vector<SkinColorFilter> skin_colors() const;
    // プレースキンの部品を、スキン選択で選んだほかのスキンから持ってきて、使っている MOD の色を当てる。
    // path は設定に書いたそのスキンのパス、options はスキンを読むときの値
    // part_cache があれば、部品を持ってくるスキンを読んだものを覚えて使い回す（スキンセレクトの試し見。鍵に条件とカスタマイズを含める）
    using PartSkinCache = std::map<std::string, std::shared_ptr<const Lr2Skin>>;
    void apply_skin_mods(Lr2Skin& sk, const std::string& path, const std::array<bool, 1000>& options,
                         PartSkinCache* part_cache = nullptr) const;
    // 使っているメトロノームの MOD（無ければ nullptr）
    const ModManifest* metronome() const;
    // 使っている MOD のうち、プレーに影響するものの組み合わせの記録の番号（0 は MOD 無し。初めてなら曲データベースに足す）。
    // seven_key なら 7k モードの記録の番号（MOD の組み合わせごとに別）
    int mod_profile(bool seven_key = false);
    int view_w = 640, view_h = 480;
    double refresh_hz = 60.0;
    std::shared_ptr<TableUpdates> table_updates;  // 起動のときに始めた難易度表の確かめ（無ければ始めていない）
    bool updated_at_start = false;  // 起動の画面で曲データベースと難易度表を更新した（選曲画面では読み直さない）
    double last_draw_ms = -1, last_present_ms = 0;  // 開発用の計測
    uint64_t scene_end_ns = 0;  // 前の画面を抜けた時刻（次の画面が最初の絵を出すまでの時間を測る）

    ~App();
    bool init(const char* argv0);
    // プレイスキンを読む。options はスキン読み込み時の #IF の判定に使う値
    void load_sound_set();
    // BGM のセット（MOD）の候補から、選曲・決定の BGM を選び直す（選曲画面に入るたび）
    void pick_bgm();
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
    void save_config() const { bmj::save_config(cfg); }
    // フルスクリーンとウインドウを切り替え、次の起動もその状態で始める
    void toggle_fullscreen();
    // 設定の音量をミキサーに渡す（スライダーで変えたときも呼ぶ）
    void apply_volume() { mixer.set_volume(cfg.volume_on, cfg.vol_master, cfg.vol_key, cfg.vol_bgm); }
    // 設定のイコライザ・ピッチ・エフェクタをミキサーに渡す（スライダーやボタンで変えたときも呼ぶ）
    void apply_sound_fx() { mixer.set_sound_fx(cfg.sound_fx); }
};

}  // namespace bmj
