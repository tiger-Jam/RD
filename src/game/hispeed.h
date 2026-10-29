// ハイスピと緑数字（仕様書6章「ハイスピ・LIFT・緑数字」）
#pragma once

#include "chart/chart.h"

namespace rd {

// 譜面の BPM の幅（LR2 の minBPM / maxBPM）。譜面の頭の #BPM から始めて、ノーツのある BPM で広げる
void note_bpm_range(const Chart& chart, double& lo, double& hi);
// ノーツがいちばん多い BPM（同数なら BPM の小さい方）。ノーツが無ければ 0
double main_bpm(const Chart& chart);
// フローティングの基準の BPM（0 = 開始 1 = 最大 2 = 最小 3 = 一番長く使われる BPM）
double floating_base_bpm(const Chart& chart, int base);

// 緑数字（1/600 秒）：ノーツが見え始めてから判定ラインに届くまでの時間。
// lane_px は判定ラインまでのレーンの長さ（スキンの座標）、visible は見えている割合（0〜1）、
// hs はハイスピ（100 = 1.00）、base_speed は基本スクロールスピード（%）
double green_number(double lane_px, double visible, int hs, double bpm, double base_speed);
// その緑数字になるハイスピ（フローティングの範囲 0.50〜10.00 に収める）
int hs_for_green(double lane_px, double visible, double green, double bpm, double base_speed);

constexpr int kFloatingHsMin = 50, kFloatingHsMax = 1000;

}  // namespace rd
