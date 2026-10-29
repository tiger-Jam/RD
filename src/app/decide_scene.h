// 決定画面（曲を決めてからプレイに入るまで）。LR2 の決定画面のスキンで描く
#pragma once

#include "app/app.h"
#include "app/select_context.h"

namespace rd {

enum class DecideResult { Play, Back, Quit };

// ctx は選曲画面のカーソルの曲の情報（スキンに渡す値）。決定画面のスキンが無ければすぐ Play を返す
DecideResult run_decide(App& app, const SelectContext& ctx);

}  // namespace rd
