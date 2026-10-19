// どの画面でも効く操作：F6 のスクリーンショットと F7 の FPS 表示（LR2 と同じ）。
// 各画面は SDL_RenderPresent の直前に frame_hooks_before_present を呼ぶ
#pragma once

#include <SDL3/SDL.h>

#include <functional>

namespace bmj {

// キーの監視を始める。on_shot はスクリーンショットを保存したときに呼ぶ（シャッターの音を鳴らす）
void frame_hooks_install(std::function<void()> on_shot);
// FPS を描き、頼まれていればスクリーンショットを保存する（frame_present が呼ぶ）
void frame_hooks_before_present(SDL_Renderer* r);

// 画面は、スキンの解像度（w×h）の絵に描いてから、出すときにウィンドウの大きさへなめらかに拡大する（LR2 と同じ見え方。
// 絵を 1 枚ずつウィンドウの大きさで描くと、なめらかにしない絵が荒れ、つなげた絵の継ぎ目に線が出る）。
// w か h が 0 以下なら、ウィンドウに直接描く（自分で拡大のしかたを決める画面）
void frame_target_set(SDL_Renderer* r, int w, int h);
// 1 フレームを出す（frame_hooks_before_present、拡大、SDL_RenderPresent）。各画面は SDL_RenderPresent の代わりにこれを呼ぶ
void frame_present(SDL_Renderer* r);

}  // namespace bmj
