// アプリの設定（ハイスピードや判定タイミング、スキンなど）。起動のたびに読み、終了時に書き戻す
#pragma once

#include <map>
#include <string>
#include <vector>

namespace bmj {

struct AppConfig {
    std::string lr2_root;          // LR2のフォルダ（LR2files があるところ）
    std::string play_skin;         // プレイスキンの .lr2skin（空なら LR2 の既定）
    std::string result_skin;       // リザルトスキンの .lr2skin（空なら LR2 の既定）
    std::string courseresult_skin; // コースリザルトスキンの .lr2skin（空なら LR2 の既定）
    std::string select_skin;       // 選曲スキンの .lr2skin（空なら LR2 の既定。"none" なら BMjoy の仮の画面）
    std::string decide_skin;       // 決定画面のスキン（空なら LR2 の既定。"none" なら決定画面を出さない）
    std::string sound_set;         // サウンドセットの .lr2ss（空なら LR2 の既定）
    std::string skin_res;          // "", "sd", "hd", "fhd"
    std::map<std::string, std::vector<int>> skin_custom;  // スキンのパス → カスタマイズの選択
    std::vector<std::string> song_dirs;  // 曲フォルダ
    std::vector<std::string> tables;     // 難易度表のURL（アプリには同梱しない。ユーザーが登録する）
    int hi_speed = 100;            // LR2のハイスピ（100=1.00倍）
    int judge_timing = 0;          // LR2の判定タイミング（描画だけをずらす）
    // 判定タイミングの自動調整（LR2: autojudgeadjust）。0 = OFF 1 = ON 2 = SILENT（キー音・BGA・判定の表示なし）
    int auto_adjust = 0;
    // ハイスピードの範囲と刻み（LR2: hsmin / hsmax / hsmargin）
    int hs_min = 10, hs_max = 900, hs_margin = 10;
    // ハイスピード固定（LR2: hstype）。0=OFF 1=MAX BPM 2=MIN BPM 3=AVERAGE 4=CONSTANT 5=MAIN BPM（5 は OpenLR2 の拡張）。
    // フローティングでは CONSTANT（一定速度）だけを見る
    int hs_fix = 0;
    // レーンカバー（LR2: shuttertype / shutter / shuttermargin）。0=使わない 1=使う。量は 0〜100
    int lane_cover = 0, lane_cover_v = 0, shutter_margin = 10;
    // ハイスピのモード（仕様書6章）：0 = クラシック（LR2 と同じ）、1 = フローティング
    int hs_mode = 0;
    // フローティング：緑数字（1/600 秒）。曲を読み込むたびに、基準の BPM でこの緑数字になるようにハイスピを決める
    int fhs_green = 300;
    int fhs_base = 3;   // 基準の BPM：0 = 開始 1 = 最大 2 = 最小 3 = 一番長く使われる BPM
    int gear_mode = 0;  // ギアチェン：0 = カバー追従（皿で）、1 = 緑数字に戻す（START の 2 回押しで）
    // LIFT（判定ラインを持ち上げる）。lift_v はレーンの高さに対する %
    bool lift = false;
    int lift_v = 0;
    // LN の種類（仕様書6章）。譜面で種類を決めていない LN に使う：0 = LN 1 = CN 2 = HCN
    int ln_mode = 0;
    // BGA（LR2: bga）。0=OFF 1=ON 2=オートプレイのときだけ 3=NO MISS（ミスのときの絵を出さない）
    int bga = 1;
    // HIDDEN / SUDDEN（LR2: effect）。0=OFF 1=HIDDEN 2=SUDDEN 3=HID+SUD
    int effect = 0;
    // 配置（LR2: random）。0=OFF 1=MIRROR 2=RANDOM 3=S-RANDOM 4=SCATTER 5=CONVERGE
    int random = 0;
    bool assist = false;  // 皿の自動演奏（LR2: assist。AUTO SCRATCH）
    int gauge = 0;                 // GaugeType
    bool gas = false;
    // fps：0 はモニタのリフレッシュレート、-1 は上限なし。垂直同期は既定で使わない
    // （OpenLR2と同じく、描画を待つ間も入力と判定を回すため）
    int fps = 0;
    bool vsync = false;
    // 画面（LR2: screenmode / screenexrate）。window_scale は % で、0 ならスキンに合わせる（SD は 2 倍）
    bool fullscreen = false;
    int window_scale = 0;
    // ウインドウサイズ（LR2: windowsize_x / windowsize_y）。両方 1 以上なら、解像度や拡大率より優先する
    int window_w = 0, window_h = 0;
    // 曲データベースを起動時に更新するか（LR2: autoreload）。0=起動時 1=手動（選曲画面の F8 で今のフォルダだけ）
    int db_update = 0;
    // LR2 の設定ツールの OPTION / JUKEBOX2 / SELECT の数値
    int base_speed = 100;      // 基本スクロールスピード（LR2: basespeed。%）
    int poor_bga_ms = 500;     // ミスBGA表示時間（LR2: poorbga）
    int input_interval = 16;   // 最低連打間隔（LR2: inputinterval。ms）
    int search_max = 1000;     // 検索最大ヒット数（LR2: searchmax。0 で無制限）
    int levelbar_5 = 9, levelbar_7 = 12, levelbar_9 = 42;  // レベルバー最大値（LR2: levelbarflash_*）
    // 選曲のスクロール（LR2: speedfirst / speednext。ms）
    int select_speed_first = 300, select_speed_next = 70;
    bool preview = true;  // 選曲中に、カーソルの曲を頭から鳴らす（LR2: select.preview。既定は有り）
    // Windows で SDL の DirectInput を使うか。既定では使わず、コントローラーは Raw Input で直接読む
    // （DirectInput の機器の列挙に数十秒かかる環境があるため）
    bool joystick_directinput = false;
    // 音の出力（LR2: OUTPUT TYPE / バッファ数）。排他は Windows の WASAPI 排他モード（ほかのアプリの音は止まる）。
    // バッファは 1 回分のフレーム数。0 なら自動（共有 128、排他 64）
    bool audio_exclusive = false;
    int audio_buffer = 0;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// 設定ファイルの置き場所（macOS なら ~/Library/Application Support/BMjoy/config.txt）
std::string config_path();
// 曲データベースの置き場所（同じフォルダの songs.db）
std::string song_db_path();
// 難易度表を手元に保存しておくフォルダ
std::string table_cache_dir();

}  // namespace bmj
