// どの画面でも効く操作：F6 のスクリーンショットと F7 の FPS 表示（LR2 と同じ）。
// 各画面は SDL_RenderPresent の直前に frame_hooks_before_present を呼ぶ
#pragma once

#include <SDL3/SDL.h>

#include <functional>

namespace bmj {

// キーの監視を始める。on_shot はスクリーンショットを保存したときに呼ぶ（シャッターの音を鳴らす）
void frame_hooks_install(std::function<void()> on_shot);
// FPS を描き、頼まれていればスクリーンショットを保存する
void frame_hooks_before_present(SDL_Renderer* r);

}  // namespace bmj
