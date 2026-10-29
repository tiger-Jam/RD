// キーコンフィグ画面（LR2 のキーコンフィグのスキン・型 8 で描く）。選曲画面のボタン 13 から入る
#pragma once

#include "app/app.h"
#include "app/select_context.h"

namespace rd {

enum class KeyconfigExit { Back, Quit };

// スキンで描くキーコンフィグ画面。閉じるまで動く（割り当てるたびに保存する）。スキンを読めなければ何もせず false
bool run_keyconfig_skin(App& app, KeyconfigExit& exit);

// 選曲画面から入るときの open_keyconfig_from_select は render/keyconfig_scene.h にある

}  // namespace rd
