// アプリの設定（ハイスピードや判定タイミング、スキンなど）。起動のたびに読み、終了時に書き戻す
#pragma once

#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "skin/skin_adjust.h"

namespace bmj {

// スキンのカスタマイズの 1 項目の選択（LR2: LR2files/SkinCustomize の xml）。
// #CUSTOMOPTION は選択肢の番号、#CUSTOMFILE は LR2 と同じく選んだ名前で持つ（ファイルが増減しても選択がずれない）
struct SkinCustomChoice {
    int index = 0;      // 選択肢の番号（file が空のとき。古い形式で保存した #CUSTOMFILE もこれ）
    std::string file;   // #CUSTOMFILE で選んだ名前（空なら index を使う）
    bool operator==(const SkinCustomChoice&) const = default;
};

struct AppConfig {
    std::string player;            // 今のプレイヤーの ID（BMjoyfiles/Player/<ID>。共通の設定に書く）
    std::string lr2_root;          // LR2のフォルダ（LR2files があるところ）
    // 外部ツールのために、LR2 の形の曲データベースとスコアを LR2files/Database に書き出すか
    bool lr2_export = false;
    // LR2 の形で書き出すスコアの名前（LR2files/Database/Score/<名前>.db。空なら LR2 から引き継いだ ID、それも無ければ BMjoy）
    std::string lr2_player;
    bool log_upload = true;        // ログを作者へ送る（app/service.h）
    bool auto_update = true;       // 起動するときに新しい版を確かめて入れ替える
    std::string service_url;       // ログの送り先と更新の置き場（空なら既定。開発用）
    std::string play_skin;         // プレイスキンの .lr2skin（空なら LR2 の既定）
    std::string result_skin;       // リザルトスキンの .lr2skin（空なら LR2 の既定）
    std::string courseresult_skin; // コースリザルトスキンの .lr2skin（空なら LR2 の既定）
    std::string select_skin;       // 選曲スキンの .lr2skin（空なら LR2 の既定。"none" なら BMjoy の仮の画面）
    std::string decide_skin;       // 決定画面のスキン（空なら LR2 の既定。"none" なら決定画面を出さない）
    std::string keyconfig_skin;    // キーコンフィグのスキン（空なら LR2 の既定。"none" ならスキンを使わない画面）
    std::string skinselect_skin;   // スキンセレクトのスキン（空なら LR2 の既定）
    std::string sound_set;         // サウンドセットの .lr2ss（空なら LR2 の既定）
    std::string skin_res;          // "", "sd", "hd", "fhd"
    std::map<std::string, std::vector<SkinCustomChoice>> skin_custom;  // スキンのパス → カスタマイズの選択（項目の順）
    // プレースキンの部品をほかのスキンから持ってくる（スキンのパス → 差し込み口の鍵 → 持ってくるスキンのパスと、その絵の種類）と、大きさの合わせ方
    struct SkinParts {
        std::map<std::string, std::string> from;
        // 差し込み口の鍵 → 持ってくるスキンの #CUSTOMFILE のうち、その部品の絵を選ぶ項目（番号 → 選んだ名前）。
        // 書いていない項目は、そのスキン自身のカスタマイズのまま
        std::map<std::string, std::map<int, std::string>> custom;
        bool screen_scale = false;  // true：画面の高さの比で合わせる。false：レーン全体の幅の比
        bool empty() const { return from.empty() && !screen_scale; }
    };
    std::map<std::string, SkinParts> skin_parts;
    std::vector<std::string> song_dirs;  // 曲フォルダ
    std::vector<std::string> custom_folder_dirs;  // カスタムフォルダ（.lr2folder）の置き場（LR2 の LR2files/CustomFolder など）
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
    bool seven_key = false;  // 7k モード：7 鍵の譜面の皿のノーツを除いて遊ぶ（記録は 7k+s と別）
    int gauge = 0;                 // GaugeType
    bool gas = false;
    // ターゲット（LR2: target）。0=既定（2P 側は動かさない） 1=自己ベスト 2=AAA 3=AA 4=A 5=割合（6〜8 の IR は無い）
    int target = 0;
    int target_percent = 90;  // 既定のターゲットの割合（LR2: defaulttarget。50〜100）
    int ghost = 0;            // ゴーストの位置（LR2: ghost）。0=OFF 1=TYPE A 2=TYPE B 3=TYPE C
    bool score_graph = false; // スコアグラフ（LR2: scoregraph）
    int bga_size = 0;         // BGA の大きさ（LR2: bgasize）。0=NORMAL 1=EXTEND（スキンの op 30 / 31 で描き分ける）
    // リプレイの保存（LR2: replaysave）。0=OFF 1=ALWAYS 2=HISCORE（EX がベスト以上） 3=CLEAR 4=FULLCOMBO
    int replay_save = 0;
    // 音量（LR2: volumeflag / master / key / bgm）。volume_on が false なら全部 100 として鳴らす
    bool volume_on = true;
    int vol_master = 100, vol_key = 100, vol_bgm = 100;
    // イコライザ・ピッチ・エフェクタ（LR2: eqflag / eqp0〜6 / pitchflag / pitchp / pitchtype / fxflag・fxtype・fxtarget・fxp1・fxp2 の _0〜_2）。
    // 既定は全部 0（オフ）
    SoundFx sound_fx;
    // ピッチで曲の速さが変わる（FREQ か SPEED で 0 以外。LR2 ではスコアを保存しない）
    bool pitch_changes_speed() const { return sound_fx.changes_speed(); }
    // 選曲の絞り込みとソート（LR2: select.difficulty など）。0=ALL / OFF
    int select_difficulty = 0, select_mode = 0, select_sort = 0;
    bool no_right_click_exit = false;
    bool folder_lamp = false;
    // MOD（仕様書14章）。使う MOD の id と、カスタムゲージで遊ぶならその鍵（「MOD の id/ゲージの id」。空なら gauge の LR2 のゲージ）
    std::vector<std::string> enabled_mods;
    std::string custom_gauge;
    bool watch_folders = true;  // 曲フォルダを見張って、変わったフォルダを選曲画面のまま読み直す（BMjoy の拡張）
    std::map<std::string, SkinAdjust> skin_adjust;  // プレイスキンのパス → 位置の調整（LR2: SkinCustomize の shift など）
    int title_flash = 24;  // この時間（時間）より後に見つけた譜面を新曲にする（LR2: titleflash）  // フォルダのバーにランプを付ける（LR2: folderlamp）  // 演奏中の右クリックで曲を終えない（LR2: disableleftclickexit）
    // fps：0 はモニタのリフレッシュレート、-1 は上限なし。垂直同期は既定で使わない
    // （OpenLR2と同じく、描画を待つ間も入力と判定を回すため）
    int fps = 0;
    bool vsync = false;
    // 画面（LR2: screenmode / screenexrate）。window_scale は % で、0 ならスキンに合わせる（SD は 2 倍）
    bool fullscreen = false;
    int window_scale = 0;
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
    bool sound_16bit = false;  // キー音を 16 ビットで持つ（float の半分のメモリ。音は変わらない）
    // メモリを節約する：GPU に送ったスキンの画像の画素を捨てる・フォルダを開くときに中身を写さない（見た目は変わらない）
    bool low_memory = true;
    // Windows で SDL の DirectInput を使うか。既定では使わず、コントローラーは Raw Input で直接読む
    // （DirectInput の機器の列挙に数十秒かかる環境があるため）
    bool joystick_directinput = false;
    // 音の出力（LR2: OUTPUT TYPE / バッファ数）。排他は Windows の WASAPI 排他モード（ほかのアプリの音は止まる）。
    // バッファは 1 回分のフレーム数。0 なら自動（共有 128、排他 64）
    bool audio_exclusive = false;
    int audio_buffer = 0;
    // 譜面を貸す（配信。仕様書10章「借りて返す」）。設定アプリでオンにすると、ログインしている間ずっと裏で貸す。合言葉が要る
    bool serve = false;
    int serve_port = 7275;
    std::string serve_key;
    bool serve_public = false;  // インターネットからも借りられるようにする（Tailscale の funnel で、貸す口だけを外に出す）
    // 譜面を借りる先（例 http://tiger-pc:7275）と合言葉、借りた譜面の置き場の上限（MB）
    std::string borrow_url, borrow_key;
    int borrow_cache_mb = 20480;

    // 1 つのファイルを読む・書く（ふつうは load_config・save_config で、共通の設定とプレイヤーの設定を分けて読み書きする）
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void write(std::ostream& f) const;
};

// 全員で共通の設定ファイル（BMjoyfiles/Config/config.txt）
std::string config_path();
// 設定を読む：共通の設定（config.txt）から今のプレイヤーを決め（util/data_dir.h の set_player_id）、プレイヤーの設定を重ねる。
// プレイヤーがまだいなければ最初のプレイヤーを作り、前の置き場のキーコンフィグ・リプレイ・MOD のファイルを移す。共通の設定があれば true
// player を渡すと、共通の設定に書いたプレイヤーではなくその人の設定を読む（切り替え。cfg は新しいものを渡す）
bool load_config(AppConfig& cfg, const std::string& player = {});
// 設定を書く：鍵ごとに、共通の設定とプレイヤーの設定に分けて書く
bool save_config(const AppConfig& cfg);
// プレイヤーを作る（copy_from のプレイヤーの設定とキーコンフィグを写す。空なら写さない）・消す
bool create_player(const std::string& id, const std::string& copy_from, std::string* error = nullptr);
bool delete_player(const std::string& id, std::string* error = nullptr);
// プレイヤーごとの設定と記録（BMjoyfiles/Player/<ID>/config.txt・score.db）
std::string player_config_path();
std::string player_score_db_path();

// config.txt の skin_custom の値（"2,\"name\",0" の形）を読む・書く
std::vector<SkinCustomChoice> parse_skin_custom(const std::string& val);
std::string format_skin_custom(const std::vector<SkinCustomChoice>& sel);

// 置き場は実行ファイルの横の BMjoyfiles の中（util/data_dir.h）。
// 設定（BMjoyfiles/Config/config.txt）は config_path()
// 曲データベースと記録（BMjoyfiles/Database/songs.db）
std::string song_db_path();
// 難易度表を手元に保存しておくフォルダ（BMjoyfiles/Database/Table）
std::string table_cache_dir();
// LR2 から引き継いだ段位・コース（BMjoyfiles/Database/lr2_courses.txt。LR2 の曲データベースの grade・expert の表を写したもの）
std::string lr2_courses_path();
// LR2 の形で書き出す曲データベースとスコアの置き場（実行ファイルの横の LR2files/Database。外部ツールのため）
std::string lr2_database_dir();
// 譜面のリプレイのファイル（BMjoyfiles/Replay/<sha256>.bjr）
std::string replay_path(const std::string& sha256);
// 全プレーの入力の記録（BMjoyfiles/Replay/InputLog/<sha256>/<日時>.bjr）
std::string input_log_path(const std::string& sha256, int64_t played_at);
// MOD の置き場所：公式（実行ファイルの横の mods/）。自分で入れる MOD は今は読まない。
// MOD が書いてよいのは BMjoyfiles/Player/<ID>/ModData/<MOD の id>/ だけ
std::string official_mods_dir();
std::string mod_data_dir(const std::string& id);
// MOD の詳細の設定（BMjoyfiles/Player/<ID>/ModData/<MOD の id>/settings.json）
std::string mod_settings_path(const std::string& id);
// ゲージで「GAS」を選んだときの custom_gauge の値（カスタムゲージの鍵は「MOD/ゲージ」なので重ならない）
inline constexpr const char* kGasKey = "gas";

}  // namespace bmj
