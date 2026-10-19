// LR2スキン（CSV）の読み込み（仕様書7章「LR2プレイスキン互換層」）。
// 挙動はOpenLR2の ReadSkin / ReadSRC / ReadDST（LR2/LR2_skinload.cpp）に合わせている。
// 画像はここでCPU上にデコードしておき、GPUへのアップロードは描画側（メインスレッド）で行う。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmj {

struct SkinImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // 空なら読み込めていない
    std::string path;
};

struct SkinSrc {
    bool valid = false;
    int n = 0, gr = -1;
    int x = 0, y = 0, w = 0, h = 0, div_x = 1, div_y = 1;
    int cycle = 0, timer = 0;
    int op[5] = {};
    int graphcount() const { return div_x * div_y; }
};

struct SkinDstKey {
    int time = 0;
    float x = 0, y = 0, w = 0, h = 0;
    int acc = 0, a = 255, r = 255, g = 255, b = 255;
    int blend = 1, filter = 0;
    float angle = 0;
    int center = 0;
    float sort = 0;  // 描画順（LR2: sortID。行番号から決まり、キーフレーム間で補間される）
};

struct SkinDst {
    int n = 0, loop = 0, timer = 0;
    int op[5] = {};
    std::vector<SkinDstKey> keys;
    bool empty() const { return keys.empty(); }
};

// DSTを時刻で評価した結果
struct SkinDraw {
    bool visible = false;
    float x = 0, y = 0, w = 0, h = 0;
    int a = 255, r = 255, g = 255, b = 255;
    int blend = 1, filter = 0;
    float angle = 0;
    int center = 0;
    float sort = 0;
};

struct SkinObject {
    SkinSrc src;
    SkinDst dst;
};

struct SkinTextSrc {
    int n = 0, font = 0, st = 0, align = 0;
    int op[5] = {};
};

struct SkinText {
    SkinTextSrc src;
    SkinDst dst;
};

// LR2の画像フォント（.lr2font）
struct ImageFontChar {
    int image = -1, x = 0, y = 0, w = 0, h = 0;
};
struct ImageFont {
    bool loaded = false;
    int size = 0, kerning = 0;
    std::map<char32_t, ImageFontChar> chars;
    std::vector<SkinImage> images;  // #T の番号順
};

struct TtfFont {
    int size = 0, thick = 0, type = 0;
};

// 選曲画面などで変えられる、スキンのカスタマイズ項目（ヘッダの #CUSTOMOPTION / #CUSTOMFILE）
struct SkinCustomItem {
    bool is_file = false;
    std::string title;
    int op_start = 0;                  // #CUSTOMOPTION のとき
    std::vector<std::string> labels;   // 選択肢（#CUSTOMFILE ならファイル名）
    std::string pattern;               // #CUSTOMFILE の検索パターン
    std::string default_value;         // #CUSTOMFILE の既定
    int selected = 0;                  // labels の添字。#CUSTOMFILE で -1 なら、既定が候補に無いので既定の文字列をそのまま使う
};

struct SkinHeader {
    int type = -1;
    std::string title, maker;
    std::string thumbnail;  // #INFORMATION の 4 番目の欄（スキンセレクトのサムネイル）
    int width = 640, height = 480;
    bool has_resolution = false;
    std::vector<SkinCustomItem> customs;
};

struct Lr2Skin {
    SkinHeader header;
    int width = 640, height = 480;

    std::vector<SkinImage> images;  // #IMAGE の番号（0..99）と特殊番号（100以降）
    std::vector<SkinObject> image_objs, numbers, bargraphs, sliders, bgas, buttons;
    std::vector<SkinText> texts;

    std::array<SkinSrc, 20> note, mine, ln_start, ln_end, ln_body;
    std::array<SkinSrc, 20> auto_note, auto_mine, auto_ln_start, auto_ln_end, auto_ln_body;
    std::array<SkinDst, 20> note_dst;
    std::array<SkinObject, 2> line, judgeline, gauge;
    std::array<std::array<SkinObject, 6>, 2> nowjudge, nowcombo;
    // リザルト画面のグラフ（ゲージの推移：0=80%未満, 1=それ以上／スコアの推移：0=自分, 1=自己ベスト, 2=ターゲット）
    std::array<SkinObject, 2> gaugechart;
    std::array<SkinObject, 2> gaugechart_2p;  // 2P 側（#FLIPRESULT のとき自分のグラフをここに描く）
    std::array<SkinObject, 3> scorechart;

    // 選曲画面：曲リストのバー（本体の種類 0..29、タイトル 0..4、レベル 0..10、ランプ類 0..9）
    struct Bar {
        std::array<SkinSrc, 30> body;
        std::array<SkinDst, 30> body_on, body_off;  // 画面の上から何本目か（添字）ごとの位置。選んでいる行は ON
        std::array<SkinText, 5> title;
        std::array<SkinObject, 11> level;
        std::array<SkinObject, 10> lamp, my_lamp, rival_lamp, rival, rank;
        std::array<SkinObject, 6> star;
        SkinObject flash;
        SkinDst stagefile;
        int center = 0;                      // 選んでいる行（#BAR_CENTER）
        int available_from = 0, available_to = 0;  // クリックで選べる行の範囲（#BAR_AVAILABLE）
    } bar;
    std::vector<SkinObject> onmouse;     // マウスを乗せたときだけ出る絵（#SRC_ONMOUSE）
    SkinObject mousecursor;
    std::array<SkinText, 2> readme;
    std::vector<std::string> helpfiles;
    bool reload_banner = false;
    std::vector<ImageFont> image_fonts;  // #LR2FONT（最大10）
    std::vector<TtfFont> ttf_fonts;      // #FONT（最大10）

    int startinput = 0, loadstart = 0, loadend = 0, playstart = 2000, fadeout = 0, close = 0;
    int startinput_rank = 0, startinput_update = 0;  // リザルト画面：ランク表示・更新表示のあと、次の入力を受けるまで
    int scratch_side[2] = {0, 0};
    // リザルトのフリップ（#FLIPRESULT で立て、#DISABLEFLIP で下ろす。読み込み順で最後の値）。
    // 読み込みを始めた時点の値は、渡した options の 351
    bool flip_result = false;
    int scenetime = 0;      // 決定画面：この時間が経ったらプレイへ（#SCENETIME）
    int ignore_input = 0;   // 決定画面：この時間まで ESC 以外の入力を受けない（#IGNOREINPUT）

    std::array<bool, 1000> op{};  // スキン読み込み時点のオプション（#IF の判定に使う。900-999 はカスタム）
    std::vector<std::string> warnings;
};

// LR2のフォルダ（LR2files の親）を基準にしたパスを、実在するファイルに解決する。
// 区切りは '\\' でも '/' でもよく、大文字小文字は区別しない。ファイル名の '*' はランダムに1つ選ぶ。
std::string resolve_lr2_path(const std::string& lr2_root, const std::string& path, bool allow_wildcard = true);

// .lr2skin のヘッダ（#INFORMATION と カスタマイズ項目）だけを読む
bool read_skin_header(const std::string& lr2_root, const std::string& skin_path, SkinHeader& out);

// スキンを読み込む。options は読み込み時点のゲームのオプション（GetOptionFlag_dst 相当）。
// header.customs の selected をカスタマイズとして反映する
bool load_lr2_skin(const std::string& lr2_root, const std::string& skin_path, const SkinHeader& header,
                   const std::array<bool, 1000>& options, Lr2Skin& out, std::string& error);

// 画像を1枚読む。use_trans なら、アルファの無い画像の trans 色を透明にする（DxLibと同じ）
bool load_skin_image(const std::string& path, SkinImage& img, const uint8_t trans[3], bool use_trans);

// DSTを時刻で評価する（LR2: SetDSTdrawByTime）。time は timer の経過ms（タイマー停止中は -1）
SkinDraw eval_dst(const SkinDst& dst, double time);
// アニメーションのコマ番号（LR2: GetSRCcycleNow）
int src_frame(const SkinSrc& src, double time);

}  // namespace bmj
