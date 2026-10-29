// 譜面の傾向の分析（MOD「譜面の傾向」のため）。oraja-constellator（MIT。mods/constellator/NOTICE.txt）の
// 特徴量と分類（python/attr_core.py・analysis_packs/official_attribute_rules.json）を移したもの。
// ノーツの時刻と鍵盤から、16 分の間隔（grid）・同時押しの厚さ・微縦連・ズレ・密度などを数え、
// 主な傾向（16 分乱打・腕ガチ・指ガチ微縦連・ディレイ・判定保留）と、副分類・複合・乱打の形（3-3 / 4-2 / 5-1）を決める
#pragma once

#include <cstdint>

#include "chart/chart.h"

namespace rd {

// 分析の版。特徴量や分類を変えたら上げる（古い版の結果は読み直す）
constexpr int kChartAttrVersion = 1;

// 傾向（0 は判定保留）
enum ChartAttrCode : int { kAttrUndecided = 0, kAttrRanda = 1, kAttrUde = 2, kAttrYubi = 3, kAttrDelay = 4 };
// 副分類（0 は無し）
enum ChartAttrSub : int {
    kSubNone = 0,
    kSubRandaMid = 1, kSubRandaHigh = 2,                       // 中速乱打・高速乱打
    kSubUdeNoFull = 11, kSubUdeFullAlt = 12, kSubUdeFullMany = 13,  // 全押しなし・全押し交互・全押し多用
    kSubYubiLongJack = 21, kSubYubiChord = 22,                  // 長縦連混じり・同時押し微縦連
    kSubDelayUltraRanda = 31, kSubDelayUltraStair = 32,         // 超高速乱打・超高速階段
};

struct ChartAttr {
    bool ok = false;  // 分析できた（ノーツが 50 未満・10 秒以下などは分析しない）
    int attr = kAttrUndecided, attr2 = kAttrUndecided, sub = kSubNone;
    int mask = 0;           // 複合（傾向ごとのビット。乱打 1・腕 2・指 4・ディレイ 8）
    int practice_low = 0;   // 反復練習には向かない（回復地帯が長い・ラス殺し・局所的な発狂など）
    double conf = 0;        // 分類の確からしさ
    // 特徴量
    double grid_bpm = 0, avg_chord = 0, chord_ge3 = 0, micro_rate = 0, long_jack_rate = 0, subgrid_rate = 0, ongrid_rate = 0;
    double stream_sec = 0, d2 = 0, d10 = 0, d20 = 0, avg_density = 0, last_kill = 0, recovery_sec = 0, chart_seconds = 0;
    double full_rate = 0, full_alt_rate = 0, micro_thin = 0, micro_chord = 0, micro_full = 0;
    double tail_grid_bpm = 0, tail_chord = 0, tail_sub = 0;
    double randa_33 = 0, randa_42 = 0, randa_51 = 0, stair_rate = 0;
    int total_notes = 0;
};

// 譜面を分析する（SP の譜面だけ。DP は分析しない）
ChartAttr analyze_chart_attr(const Chart& chart);

}  // namespace rd
