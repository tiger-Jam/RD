#include "game/gauge.h"

#include <algorithm>

namespace bmj {

void GaugeSet::init(int total, int notes, int course) {
    course_ = course >= 0;
    hp_ = {20.0, 100.0, 100.0, 20.0, 100.0, 100.0};
    if (course_) hp_[kGroove] = hp_[kEasy] = 100.0;
    if (notes <= 0) return;

    // HARD系のダメージ係数（LR2: bmsload:3853）
    double dmg_notebase;
    if (notes < 20) dmg_notebase = 10.0;
    else if (notes < 30) dmg_notebase = 10.0 - (notes - 20.0) / 10.0 * 2.0;
    else if (notes < 45) dmg_notebase = 7.0 - (notes - 30.0) / 15.0;
    else if (notes < 60) dmg_notebase = 6.0 - (notes - 45.0) / 15.0;
    else if (notes < 125) dmg_notebase = 5.0 - (notes - 60.0) / 65.0;
    else if (notes < 250) dmg_notebase = 4.0 - (notes - 125.0) / 125.0;
    else if (notes < 500) dmg_notebase = 3.0 - (notes - 250.0) / 250.0;
    else if (notes < 1000) dmg_notebase = 2.0 - (notes - 500.0) / 500.0;
    else dmg_notebase = 1;
    int recover = static_cast<int>((total - 80.0) * 0.125 / 2);
    if (recover <= 0) recover = 1;
    double dmg_totalbase = 100.0 / static_cast<double>(recover);
    double dmg = std::max(dmg_notebase * 10, dmg_totalbase) / 10.0;

    // LR2はTOTAL/ノーツ数をfloatで計算している
    double per = total / static_cast<float>(notes);
    double per_half = total / static_cast<float>(notes + notes);
    double t[kGaugeCount][6] = {
        // 空POOR, POOR, BAD, GOOD, GREAT, PGREAT
        {-2.0, -6.0, -4.0, per_half, per, per},                                            // GROOVE
        {dmg * -2.0, dmg * -10.0, dmg * -6.0, 0.05, 0.1, 0.1},                              // HARD
        {0.0, -100.0, -100.0, 0.0, 0.0, 0.0},                                               // DEATH
        {-1.6, -4.800000000000001, -3.2, static_cast<float>(per_half) * 1.2, static_cast<float>(per) * 1.2,
         static_cast<float>(per) * 1.2},                                                    // EASY
        {-100.0, -100.0, -100.0, -100.0, -1.0, 0.1},                                        // P-ATTACK
        {dmg * -2.0, dmg * -10.0, -6.0, 0.1, -1.0, dmg * -10.0},                            // G-ATTACK
    };
    for (int g = 0; g < kGaugeCount; ++g)
        for (int j = 0; j < 6; ++j) table_[g][j] = t[g][j];
    // コースの固定の増減（空POOR, POOR, BAD, GOOD, GREAT, PGREAT）
    auto set = [&](int g, std::array<double, 6> v) {
        for (int j = 0; j < 6; ++j) table_[g][j] = v[j];
    };
    if (course == 2) {
        // 段位認定
        set(kGroove, {-2.0, -3.0, -2.0, 0.04, 0.1, 0.1});
        set(kEasy, {-2.0, -3.0, -2.0, 0.04, 0.1, 0.1});
        table_[kHard][kGood] = 0.04;
    } else if (course >= 0) {
        // エキスパート・ノンストップ
        set(kGroove, {-1.5, -2.0, -1.5, 0.04, 0.1, 0.1});
        set(kEasy, {-1.2, -1.6, -1.2, 0.048, 0.12, 0.12});
    }
}

void GaugeSet::apply_judge(int judge, double mul) {
    for (int g = 0; g < kGaugeCount; ++g) {
        int hp = (static_cast<int>(hp_[g]) / 2) * 2;
        double damage = table_[g][judge] * mul;
        // 残り30%以下ではダメージを0.6倍にする（単曲ではGROOVE/EASY以外。コースでは全部）
        if (hp <= 30 && (!(g == kGroove || g == kEasy) || course_) && judge <= kBad) damage *= 0.6;
        hp_[g] += damage;
        // GROOVE/EASY は 2% で止まる（コースでは止まらない）
        if (!course_ && hp_[g] <= 2.0 && (g == kGroove || g == kEasy)) hp_[g] = 2.0;
        hp_[g] = std::clamp(hp_[g], 0.0, 100.0);
        if (hp <= 0) hp_[g] = 0;  // 一度0になったゲージは戻らない
    }
}

void GaugeSet::apply_mine(int damage) {
    for (int g = 0; g < kGaugeCount; ++g) {
        hp_[g] -= damage;
        // 地雷ではコース中も 2% で止まる（LR2 と同じ）
        if (hp_[g] <= 2.0 && (g == kGroove || g == kEasy)) hp_[g] = 2.0;
        hp_[g] = std::clamp(hp_[g], 0.0, 100.0);
    }
}

int gas_select(const GaugeSet& g, int configured) {
    if (configured == kGAttack) return kGAttack;
    static const int order[5] = {kPAttack, kDeath, kHard, kGroove, kEasy};
    int i = 0;
    while (i < 5 && order[i] != configured) ++i;
    for (; i < 5; ++i) {
        int t = order[i];
        double need = (t == kGroove || t == kEasy) ? 80.0 : 2.0;
        if (g.hp(t) >= need) return t;
    }
    return kEasy;
}

int clear_type(const GaugeSet& g, int gauge_type, int total_notes, int note_current, int max_combo, int good, bool course) {
    if (total_notes == max_combo) return good == 0 ? 6 : 5;
    if (gauge_type == kHard || gauge_type == kGAttack || gauge_type == kPAttack) {
        if (note_current == total_notes && g.hp(gauge_type) >= 2.0) return 4;
        return 1;
    }
    if (note_current == total_notes && g.hp(gauge_type) >= (course ? 2.0 : 80.0)) return gauge_type != kEasy ? 3 : 2;
    return 1;
}

}  // namespace bmj
