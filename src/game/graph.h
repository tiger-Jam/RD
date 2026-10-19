// 演奏中のゲージとスコアの推移（リザルト画面のグラフに使う）。曲の長さを1000等分して記録する
// （参照: OpenLR2 LR2/LR2_statplay.cpp の LogGraphPlayData / LogGraphData / LogGraphPlayerDataToEnd）
#pragma once

#include <array>

#include "game/gauge.h"

namespace bmj {

struct PlayGraph {
    static constexpr int kPoints = 1000;
    std::array<std::array<int, kPoints>, kGaugeCount> hp{};  // LR2と同じく整数に切り捨てて持つ
    std::array<int, kPoints> combo{}, exscore{};
    std::array<int, kPoints> best{}, target{};  // 自己ベスト・ターゲットのゴーストのEX
    int cursor = 0, best_cursor = 0, target_cursor = 0;

    void start(const GaugeSet& g) {
        *this = PlayGraph{};
        for (int i = 0; i < kGaugeCount; ++i) hp[i][0] = static_cast<int>(g.hp(i));
    }
    // time / end_time はms。時刻が進んだ分だけ点を埋める
    void log(int time, int end_time, const GaugeSet& g, int now_combo, int ex, int best_ex, int target_ex) {
        if (end_time <= 0 || time > end_time) return;
        int until = time * kPoints / end_time;
        while (cursor < kPoints && cursor <= until) {
            for (int i = 0; i < kGaugeCount; ++i) hp[i][cursor] = static_cast<int>(g.hp(i));
            combo[cursor] = now_combo;
            exscore[cursor] = ex;
            ++cursor;
        }
        while (best_cursor < kPoints && best_cursor <= until) best[best_cursor++] = best_ex;
        while (target_cursor < kPoints && target_cursor <= until) target[target_cursor++] = target_ex;
    }
    // 演奏が終わったら、残りを最後の値で埋める。途中でやめたときはゲージを0にしてから呼ぶ（LR2: Play.cpp:1659）
    void finish(const std::array<double, kGaugeCount>& final_hp, int now_combo, int ex, int best_ex, int target_ex) {
        for (int i = cursor; i < kPoints; ++i) {
            for (int g = 0; g < kGaugeCount; ++g) hp[g][i] = cursor >= 1 ? static_cast<int>(final_hp[g]) : hp[g][0];
            combo[i] = cursor >= 1 ? now_combo : combo[0];
            exscore[i] = cursor >= 1 ? ex : exscore[0];
        }
        for (int i = best_cursor; i < kPoints; ++i) best[i] = best_ex;
        for (int i = target_cursor; i < kPoints; ++i) target[i] = target_ex;
        cursor = best_cursor = target_cursor = kPoints;
    }
    // カスタムゲージで遊んでいるとき、見せるゲージの列（slot）を遊んでいるゲージの値で上書きする。log・finish のすぐ後に呼ぶ
    int overlay_cursor = 0;
    void overlay(int slot, double v) {
        for (; overlay_cursor < cursor; ++overlay_cursor) hp[slot][overlay_cursor] = static_cast<int>(v);
    }
};

}  // namespace bmj
