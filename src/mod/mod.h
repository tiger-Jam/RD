// MOD（仕様書14章）。1 つの MOD は 1 つのフォルダで、定義ファイル mod.json と、必要ならスクリプト（Lua）を持つ。
// 本体は MOD に触れる場所を宣言させ、プレーに影響する MOD（判定・譜面の変換・入力）の組み合わせごとに記録を分ける
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
    // ランプ
    std::string lamp_name;        // 例 "EX-HARD"
    uint32_t lamp_color = 0xffffff;
    double lamp_rank = 4.5;       // ランプの並び（1 FAILED … 4 HARD、5 FULL COMBO）。カスタム同士もこの値の順
    int lamp_as = 4;              // LR2 のスキンや本体の一覧に見せるときのランプの番号（スキンは 0〜5 しか知らない）
};

// カスタム判定（判定の MOD。プレーに影響するので、記録は別に分ける）
struct CustomJudgeDef {
    std::string id, name;
    // 判定ごとの幅（ms）。早い側・遅い側
    int pgreat[2] = {18, 18}, great[2] = {40, 40}, good[2] = {100, 100}, bad[2] = {200, 200};
    int empty_poor = 1000;        // 早い側の空POOR の幅
    bool rank_scaled = false;     // true なら #RANK に合わせて伸び縮みさせる（NORMAL の幅に対する LR2 の比）
};

struct ModManifest {
    std::string id, name, version, author, description;
    std::string dir;              // MOD のフォルダ
    std::string hash;             // フォルダの中身の SHA-256（記録に残す）
    uint32_t touches = 0;         // ModTouch のビット
    bool official = false;        // 実行ファイルの横の mods/ にあったもの
    std::string script;           // スクリプト（mod.json からの相対。無ければ空）
    bool gas = false;             // GAS（ゲージの自動切り替え）を効かせる
    std::vector<CustomGaugeDef> gauges;
    std::vector<CustomJudgeDef> judges;
    bool affects_play() const { return (touches & kModAffectsPlay) != 0; }
};

// mod.json を読む。読めなければ false と、error に理由
bool load_mod_manifest(const std::string& dir, ModManifest& out, std::string* error = nullptr);
// フォルダの直下にある MOD を全部読む（official はそのまま ModManifest::official に入る）
std::vector<ModManifest> scan_mods(const std::string& root, bool official, std::vector<std::string>* errors = nullptr);

// 使う MOD の組み合わせの問題（同じ id、判定の MOD が 2 つ、GAS が 2 つ、など）。空なら問題なし
std::vector<std::string> mod_conflicts(const std::vector<const ModManifest*>& mods);
// プレーに影響する MOD の組み合わせを、記録の鍵にする文字列にする（「id@版#ハッシュの頭」を id の順に並べたもの。無ければ空）
std::string play_mod_key(const std::vector<const ModManifest*>& mods);

}  // namespace bmj
