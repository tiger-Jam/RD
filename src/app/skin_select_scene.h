// スキンセレクト画面（LR2 のスキンセレクトのスキンで描く）。選曲画面のボタン14から入る
#pragma once

#include "app/app.h"
#include "app/skin_catalog.h"

namespace bmj {

// 閉じるまで動く。選曲スキン（やそのカスタマイズ）を替えたら true
bool run_skin_select(App& app, SkinCatalog& catalog);

}  // namespace bmj
