// RD で足したスキンのボタンの種類（LR2 には無い。tools/skin のスクリプトで W-MIX に足す）
#pragma once

namespace rd {

// 選曲画面の設定パネルの判定の欄：押すとカスタム判定を順に切り替える
constexpr int kJudgeButton = 900;
// リザルトの「今回のクリア」：コマはゲージの種類（ボタン 40）と同じ。付いたランプの色に染めて出す
constexpr int kClearLabelButton = 901;
// 選曲画面の設定パネルのメトロノームの欄：押すとメトロノームの MOD を使う・使わないを切り替える（0 OFF、1 ON）
constexpr int kMetronomeButton = 902;

}  // namespace rd
