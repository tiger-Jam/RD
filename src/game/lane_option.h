// 配置のオプション（LR2 の RANDOM 系）。譜面を読んだあと、判定の前に鍵盤のノーツを並べ替える
#pragma once

#include "chart/chart.h"
#include "util/lr2_random.h"

namespace bmj {

// LR2 の番号と同じ（optionstr.csv の #RANDOM の並び）
enum RandomOption : int { kRandomOff = 0, kMirror = 1, kRandom = 2, kSRandom = 3, kScatter = 4, kConverge = 5 };

// 配置の並べ替え（#RANDOM の評価を含む）の作り方の版（仕様書6章「ランダム配置の保存」）。
// 同じ種から違う譜面ができる変更をしたら上げ、古い版の作り方も残して、記録した版で作り直せるようにする
// 1: LR2（DxLib の乱数。5 鍵は 1〜5 鍵の中で並べ替える）
constexpr int kRandomAlgorithmVersion = 1;

// 譜面の鍵盤数（LR2: songmanage:3003）。6・7 鍵にノーツ（見えないノーツと LN を含み、地雷は除く）が無ければ 5
int key_count(const Chart& chart);

// 鍵盤（5 鍵の譜面はレーン1〜5、それ以外は1〜7）のノーツを並べ替える。皿と不可視ノーツは動かさない。
// rng は #RANDOM を評価した続きの乱数（LR2 と同じ並びにするため）。
// 並べ替えで重なったノーツは、譜面の読み込みと同じ規則で捨てる
void apply_random_option(Chart& chart, int option, Lr2Random& rng);


}  // namespace bmj
