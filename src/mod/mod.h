// MOD（仕様書14章）。1 つの MOD は 1 つのフォルダで、定義ファイル mod.json と、必要ならスクリプト（Lua）を持つ。
// 本体は MOD に触れる場所を宣言させ、プレーに影響する MOD（判定・譜面の変換・入力）の組み合わせごとに記録を分ける
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "skin/skin_color.h"

namespace bmj {

// MOD が触れる場所の分類（仕様書14章「触れる場所の分類」）
enum class ModTouch : uint32_t {
    Visual = 1 << 0,      // 見た目（スキン、表示の追加）
    Sound = 1 << 1,       // 音
    Control = 1 << 2,     // 選曲の操作、ショートカット、フォルダの作成
    Evaluation = 1 << 3,  // 評価（カスタムゲージ・ランプ、GAS、出力の監視）
    Judge = 1 << 4,       // 判定（カスタム判定）
    Chart = 1 << 5,       // 譜面の変換
    Input = 1 << 6,       // 入力
};
constexpr uint32_t kModAffectsPlay = static_cast<uint32_t>(ModTouch::Judge) | static_cast<uint32_t>(ModTouch::Chart) |
                                     static_cast<uint32_t>(ModTouch::Input);

// 判定ごとの増減（% の値。total が 0 でなければ「total × TOTAL ÷ 総ノーツ数」を足す。GROOVE の増え方と同じ形）
struct GaugeDelta {
    double fixed = 0, total = 0;
};

// カスタムランプ（評価の MOD）。ゲージはクリアしたときのランプをこの一覧から選ぶ（いくつのゲージで同じランプを使ってもよい）
struct CustomLampDef {
    std::string id, name;         // id は MOD の中で一意。記録の鍵は「MOD の id/ランプの id」
    uint32_t color = 0xffffff;    // 選曲画面のランプの色（スキンのランプの絵を、この色に染めて出す）
    int colors = 1;               // 2 なら color と color2 を blink_ms ごとに入れ替える（点滅）。本体のランプの色の上書きでは 0 = スキンの絵のまま
    uint32_t color2 = 0xffffff;
    int blink_ms = 500;
    double rank = 4.5;            // ランプの並び（1 FAILED … 4 HARD、5 FULL COMBO）。カスタム同士もこの値の順
    int as = 4;                   // LR2 のスキンや本体の一覧に見せるときのランプの番号（スキンは 0〜5 しか知らない）
    // その時刻（ms）の色
    uint32_t color_at(double ms) const {
        if (colors != 2 || blink_ms <= 0) return color;
        return static_cast<int64_t>(ms / blink_ms) % 2 ? color2 : color;
    }
};

// 本体のランプのうち、ゲージがクリアしたときに選べるもの（鍵と番号）
inline const char* const kStandardLampKeys[] = {"FAILED", "EASY", "CLEAR", "HARD"};

// カスタムゲージ（評価の MOD。判定の並びだけから計算できるので、プレーには影響しない扱い）
struct CustomGaugeDef {
    std::string id, name;         // id は MOD の中で一意。記録の鍵は「MOD の id/ゲージの id」
    double initial = 100;         // 始まりの値（%）
    double border = 80;           // 曲の終わりにこの値以上ならクリア（survival のゲージは 0 より上で残ればクリア）
    bool survival = true;         // 0 になったら落ちる（HARD 系）。false なら最後まで遊び、終わりの値で決める（GROOVE 系）
    double floor = 0;             // これより下がらない（GROOVE の 2%。survival では使わない）
    std::array<GaugeDelta, 6> delta{};  // 空POOR, POOR, BAD, GOOD, GREAT, PGREAT
    double mine_mul = 1.0;        // 地雷のダメージの倍率
    double low_hp = 0, low_hp_mul = 1.0;  // 残りが low_hp 以下のとき、減る量に low_hp_mul を掛ける（LR2 の HARD は 30 と 0.6）
    // ゲージの色。colors が 1 なら全部 color、2 ならクリアの線より上が color・下が color_low（落ちたら終わりのゲージは color だけ）
    int colors = 1;
    uint32_t color = 0xff3030, color_low = 0x30a0ff;
    int frame = -1;               // スキンの GAUGE のボタンの絵のコマ（-1 なら無し。絵に無ければ名前を文字で出す）
    // クリアしたときのランプ：本体のランプ（kStandardLampKeys）か、カスタムランプ（同じ MOD なら id、ほかの MOD なら「MOD/id」）
    std::string lamp;
    // 以下は lamp から引いた中身（resolve_gauge_lamps が埋める）。lamp_key はカスタムランプの記録の鍵（本体のランプなら空）
    std::string lamp_key, lamp_name;
    uint32_t lamp_color = 0xffffff;
    double lamp_rank = 4;
    int lamp_as = 4;
};

// カスタム判定（判定の MOD。プレーに影響するので、記録は別に分ける）
struct CustomJudgeDef {
    std::string id, name;
    // 判定ごとの幅（ms）。早い側・遅い側
    int pgreat[2] = {18, 18}, great[2] = {40, 40}, good[2] = {100, 100}, bad[2] = {200, 200};
    int empty_poor = 1000;        // 早い側の空POOR の幅
    bool rank_scaled = false;     // true なら #RANK に合わせて伸び縮みさせる（NORMAL の幅に対する LR2 の比）
};

// MOD の詳細の設定（起動の設定画面で変えられる項目）。値は MOD ごとの settings.json に残す
struct ModSetting {
    std::string type;  // "gauge_order"：GAS のゲージの順番、"gauges"：カスタムゲージの一覧、"lamps"：カスタムランプの一覧、
                       // "judges"：カスタム判定の一覧、"metronome"：メトロノームの音量、"bgm"：選曲・決定の BGM
    std::string name;  // 画面に出す名前
};

// 本体のゲージ（GROOVE … G-ATTACK）の色の上書き。colors が 0 ならスキンの絵のまま、1 なら全部 color、
// 2 なら 80% 以上が color・80% 未満が color_low（落ちたら終わりのゲージは color だけ）
struct GaugeColors {
    int colors = 0;
    uint32_t color = 0xff3030, color_low = 0x30a0ff;
};

// 本体のランプの色を変えていない状態（どれもスキンの絵のまま）
inline std::array<CustomLampDef, 6> skin_standard_lamps() {
    std::array<CustomLampDef, 6> a{};
    for (CustomLampDef& l : a) l.colors = 0;
    return a;
}

constexpr int kMaxNumberedGauges = 9;
// メトロノームの音量の上限（%）
constexpr int kMaxMetronomeVolume = 500;

// 本体のゲージの、設定や記録での名前（LR2 のゲージの番号の順）
inline const char* const kGaugeKeys[] = {"GROOVE", "HARD", "HAZARD", "EASY", "P-ATTACK", "G-ATTACK"};

struct ModManifest {
    std::string id, name, version, author, description;
    std::string dir;              // MOD のフォルダ
    std::string hash;             // フォルダの中身の SHA-256（記録に残す）
    uint32_t touches = 0;         // ModTouch のビット
    bool official = false;        // 実行ファイルの横の mods/ にあったもの
    std::string script;           // スクリプト（mod.json からの相対。無ければ空）
    bool chart_attr = false;      // build_folders で譜面の傾向（chart/chart_attr.h）を使う（無い譜面は裏で分析する）
    bool gas = false;             // GAS（ゲージの自動切り替え）を選べるようにする
    // メトロノーム：遊んでいる間、拍に合わせて音を鳴らす（小節の頭は accent、ほかの拍は beat。どちらも MOD のフォルダの中の音）
    bool metronome = false;
    std::string metronome_accent, metronome_beat;  // 音のファイル（絶対パス）
    int metronome_volume = 400;                    // 音量（%。kMaxMetronomeVolume まで）
    // BGM のセット（bgm の設定を持つ MOD）：名前を付けたセットごとに、選曲・決定の BGM の候補をいくつでも持つ。
    // 使うセット（bgm_use の名前。空なら使わず、サウンドセットのまま）の候補から、選曲画面に入るたびにランダムに 1 つずつ選ぶ。
    // 候補は「サウンドセットのパス|#CUSTOMFILE の番号|選んだ名前」（app/bgm_set.h）
    struct BgmSet {
        std::string name;
        std::vector<std::string> select, decide;
    };
    std::vector<BgmSet> bgm_sets;
    std::string bgm_use;
    // GAS で落ちていくゲージの順番。本体のゲージは kGaugeKeys の名前、カスタムゲージは「MOD の id/ゲージの id」
    std::vector<std::string> gas_order;
    std::vector<CustomGaugeDef> gauges;
    std::vector<CustomLampDef> lamps;
    // 本体のランプ（1 FAILED … 6 PERFECT の順）の色を変える。colors が 0 ならスキンの絵のまま（lamps の設定を持つ MOD の詳細で変える）
    std::array<CustomLampDef, 6> standard_lamps = skin_standard_lamps();
    // 本体のゲージ（GROOVE … G-ATTACK の順）の色を変える（gauges の設定を持つ MOD の詳細で変える）
    std::array<GaugeColors, 6> standard_gauges{};
    // ほかの MOD のランプ（鍵は「MOD の id/ランプの id」）の色を変える。colors が 0 ならその MOD の色のまま
    std::map<std::string, CustomLampDef> lamp_colors;
    // gauge_label があれば、ゲージの名前は並びの順に「gauge_label + 番号」（CUSTOM1 …）、絵のコマは gauge_frame + 番号 − 1。
    // 番号は kMaxNumberedGauges まで（スキンに用意したコマの数）
    std::string gauge_label;
    int gauge_frame = -1;
    std::vector<ModSetting> settings;
    std::vector<CustomJudgeDef> judges;
    // プレースキンの部品の色を変える（読み込んだときに、上から順に当てる）
    std::vector<SkinColorFilter> skin_colors;
    // 遊ぶ判定の id（judges の設定を持つ MOD。空なら判定を変えない）。本体の判定（kStandardJudgeIds）も選べる
    std::string judge_use;
    bool has_setting(const char* type) const {
        for (const ModSetting& s : settings)
            if (s.type == type) return true;
        return false;
    }
    // 遊ぶ判定：judges の設定を持つ MOD は judge_use で選んだもの（本体の判定か一覧のもの。無ければ使わない）、
    // 持たない MOD は最初のもの
    const CustomJudgeDef* active_judge() const;
    // 判定を選んでいない「カスタム判定」は、プレーに影響しない（記録を分けない）
    bool affects_play() const {
        uint32_t t = touches;
        if (!active_judge()) t &= ~static_cast<uint32_t>(ModTouch::Judge);
        return (t & kModAffectsPlay) != 0;
    }
};

// mod.json を読む。読めなければ false と、error に理由
bool load_mod_manifest(const std::string& dir, ModManifest& out, std::string* error = nullptr);
// フォルダの直下にある MOD を全部読む（official はそのまま ModManifest::official に入る）
std::vector<ModManifest> scan_mods(const std::string& root, bool official, std::vector<std::string>* errors = nullptr);

// 詳細の設定（settings.json）を読み、MOD の値（gauges・gas_order）を置き換える。無ければ mod.json の値のまま
void apply_mod_settings(ModManifest& m, const std::string& settings_path);
// 詳細の設定を settings.json の中身にする（変わったかを比べるのにも使う）
std::string mod_settings_json(const ModManifest& m);
// 詳細の設定を書く（gauges・gas_order のうち、MOD が設定に持つもの）
bool save_mod_settings(const ModManifest& m, const std::string& settings_path);

// 本体のゲージ（GROOVE … G-ATTACK）を、カスタムゲージの形で書いたもの（設定画面の見本）。
// HARD 系の減り方は、ノーツが少ない譜面ほど大きくなる（ここに書いたのは補正のかからない 1000 ノーツ以上の値）
std::vector<CustomGaugeDef> standard_gauge_samples();
// 本体の判定（#RANK の VERY HARD … EASY）を、カスタム判定の形で書いたもの（設定画面の見本）と、
// #RANK に合わせて伸び縮みする NORMAL（譜面どおりの幅）
std::vector<CustomJudgeDef> standard_judge_samples();
// 遊ぶ判定として選べる本体の判定（譜面の #RANK に関係なくこの幅で遊ぶ）。選曲画面の判定の欄ではこの順に OFF のあとに並ぶ
inline const char* const kStandardJudgeIds[] = {"VERY HARD", "HARD", "NORMAL", "EASY"};
// 本体の判定（kStandardJudgeIds の id）。無ければ nullptr
const CustomJudgeDef* standard_judge(const std::string& id);
// 判定の幅の値（名前は入れない）。記録を分ける鍵に使う（幅が変われば別の記録）
std::string judge_values_key(const CustomJudgeDef& j);
// 本体のランプ（FAILED … PERFECT）を、カスタムランプの形で書いたもの（設定画面の見本。rank と as は本体の番号）
std::vector<CustomLampDef> standard_lamp_samples();

// 使っている MOD（enabled）の lamp_colors を、相手の MOD のランプの色に重ねる（ゲームを始めるときに 1 回）
void apply_lamp_colors(std::vector<ModManifest>& mods, const std::vector<std::string>& enabled);

// ゲージのランプ（lamp の鍵）を引いて lamp_* を埋め、gauge_label のある MOD のゲージに名前と絵のコマを付ける。
// 見つからないランプは本体の HARD（落ちたら終わり）か CLEAR
void resolve_gauge_lamps(std::vector<ModManifest>& mods);

// 使う MOD の組み合わせの問題（同じ id、判定の MOD が 2 つ、GAS が 2 つ、など）。空なら問題なし
std::vector<std::string> mod_conflicts(const std::vector<const ModManifest*>& mods);
// プレーに影響する MOD の組み合わせを、記録の鍵にする文字列にする（「id@版#ハッシュの頭」を id の順に並べたもの。無ければ空）
std::string play_mod_key(const std::vector<const ModManifest*>& mods);
// 7k モード（皿を除いて遊ぶ）の記録の鍵：play_mod_key の前に「7k」を付ける（MOD の部分には @ があるので重ならない）
std::string seven_key_record_key(const std::string& mod_key);

}  // namespace bmj
