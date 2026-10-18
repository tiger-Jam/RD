// 選曲画面（仕様書9章）。難易度表と曲フォルダを同じ「フォルダ」として並べる。
// 見た目はまだ仮のもの（選曲スキンの形式は未決）
#pragma once

#include "app/app.h"

namespace bmj {

// 閉じるまで動き続ける。選んだ譜面は run_play で遊び、終わったら戻ってくる
void run_select(App& app);

}  // namespace bmj
