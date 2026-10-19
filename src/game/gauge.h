// LR2のゲージ（参照: OpenLR2 LR2/LR2_bmsload.cpp:3790-3920, LR2/Scene04_Play.cpp:160-307, LR2/LR2_statplay.cpp:163）
#pragma once

#include <array>

namespace bmj {

// 判定の番号はLR2と同じ（0=空POOR, 1=POOR, 2=BAD, 3=GOOD, 4=GREAT, 5=PGREAT）
enum Judge : int { kEmptyPoor = 0, kPoor = 1, kBad = 2, kGood = 3, kGreat = 4, kPGreat = 5 };
// ゲージの番号もLR2と同じ
enum GaugeType : int { kGroove = 0, kHard = 1, kDeath = 2, kEasy = 3, kPAttack = 4, kGAttack = 5 };
constexpr int kGaugeCount = 6;

class GaugeSet {
public:
    // 初期値と増減表を作る。total は整数のTOTAL、notes は総ノーツ数。
    // course はコースの種類（0 エキスパート 1 ノンストップ 2 段位、-1 は単曲）。コースでは GROOVE/EASY も 100% から始まり、
    // 増減の一部が固定の値になる（LR2: bmsload:3879）
    void init(int total, int notes, int course = -1);
    // コースの 2 ステージ目からは、前のステージの終わりの値から始める
    void set_hp(const std::array<double, kGaugeCount>& hp) { hp_ = hp; }
    const std::array<double, kGaugeCount>& hp_all() const { return hp_; }
    // 判定1つ分を全ゲージに反映する。mul は増減の倍率（HCN の押している間の増減は 0.5）
    void apply_judge(int judge, double mul = 1.0);
    // 地雷のダメージ（%）を全ゲージに反映する
    void apply_mine(int damage);

    double hp(int gauge) const { return hp_[gauge]; }
    double delta(int gauge, int judge) const { return table_[gauge][judge]; }

private:
    std::array<double, kGaugeCount> hp_{};
    bool course_ = false;
    double table_[kGaugeCount][6] = {};
};

// ゲージが0になって落ちたか（LR2: HP < 2.0 で終了）
inline bool gauge_failed(const GaugeSet& g, int type) { return g.hp(type) < 2.0; }

// GAS（OpenLR2で追加されたゲージ自動切り替え）。configured から順に生きているゲージを返す
int gas_select(const GaugeSet& g, int configured);

// course はコース中（GROOVE/EASY/DEATH も 2% 残ればクリア）。
// クリアランプ（1 FAILED, 2 EASY, 3 NORMAL, 4 HARD, 5 FULL COMBO, 6 PERFECT）。
// 5 までは LR2 の番号。6 は FULL COMBO のうち GOOD が 0 のもの（空POORはコンボを切らないので構わない）
int clear_type(const GaugeSet& g, int gauge_type, int total_notes, int note_current, int max_combo, int good,
               bool course = false);
// スキンに渡すランプ（LR2 のスキンは 5 までしか知らないので、PERFECT は FULL COMBO として見せる）
inline int skin_lamp(int lamp) { return lamp > 5 ? 5 : lamp; }

}  // namespace bmj
