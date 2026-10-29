// プレースキンの部品を、ほかのプレースキンから持ってくる（スキンの横断利用）。
// 持ってこられるのは、ノーツ・ボム・キービーム・判定の文字だけ。遊ぶスキン（フレーム）の「場所」（レーンの位置と幅、判定ラインの高さ、レーン全体の真ん中）に、
// 持ってくる部品を、元のスキンでの場所からのずれごと付け替える。挙動（タイマー・アニメーション）は元の部品のまま
#pragma once

#include <string>
#include <vector>

#include "skin/lr2_skin.h"

namespace rd {

// 差し込み口（設定の鍵と、スキン選択に出す名前）
struct PlayPartSlot {
    const char* key;
    const char* name;
};
inline constexpr PlayPartSlot kPlayPartSlots[] = {
    {"notes", "NOTES"},          // ノーツ
    {"ln", "LN"},                // LN の始点・中・終点
    {"mine", "MINE"},            // 地雷
    {"bomb", "BOMB"},            // ボムと LN を押している間のボム
    {"beam", "KEY BEAM"},        // キービーム
    {"judge", "JUDGE"},          // 判定の文字とコンボ
};
// フレーム（判定ライン・ゲージ・レーンカバーなど）は持ってこない。位置が少しずれると目立つうえ、スキンの世界観の一部なので
inline constexpr int kPlayPartSlotCount = static_cast<int>(sizeof(kPlayPartSlots) / sizeof(kPlayPartSlots[0]));

// 大きさの合わせ方：レーン全体の幅の比 / 画面の高さの比
enum class PartScale { Lane, Screen };

// from の part（kPlayPartSlots の鍵）を sk に付け替える。from にその部品が無ければ何もしない。付け替えた部品の数を返す
int compose_play_part(Lr2Skin& sk, const Lr2Skin& from, const std::string& part, PartScale scale);

// from の part の絵を選んでいる #CUSTOMFILE（ヘッダの customs の番号、小さい順）。画像を読まずに読んだスキンでよい
std::vector<int> play_part_customs(const Lr2Skin& from, const std::string& part);

}  // namespace rd
