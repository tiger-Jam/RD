// スキンの部品の色を変える（MOD の skin_colors）。読み込んだあと、選んだ部品の絵だけを色を変えた写しに差し替える。
// 部品の見分け方は、命令の種類と、#SRC_IMAGE ならタイマーの番号（ボム・キービームなど）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "skin/lr2_skin.h"

namespace rd {

// #SRC_IMAGE の部品の種類（タイマーから決める）
enum class SkinImagePart { Frame, Bomb, Hold, Beam, Key, JudgeFx, GaugeFx };

// 判定ラインの下の端（無ければ 0）
float judge_line_bottom(const Lr2Skin& sk);
// #SRC_IMAGE の部品の種類。lane はボム・キービーム・鍵盤のレーン（0 が皿、10〜 が 2P）、on はキーオンのタイマーか。
// 判定ラインより上で光るものはキービーム、下は鍵盤
SkinImagePart image_part(const SkinObject& o, float judge_bottom, int& lane, bool& on);

struct SkinColorFilter {
    // 部品：note・ln（始点・中・終点）・mine・bomb・hold（LN を押している間のボム）・beam・key・judge（判定の文字）・combo・
    // judgeline・line（小節線）・gauge・frame（そのほか全部）
    std::vector<std::string> parts;
    // レーン（scratch・white・blue か、1〜9 の数字。空なら全部）。ノーツ・ボム・キービーム・鍵盤だけに効く
    std::vector<std::string> lanes;
    // 判定（pgreat・great・good・bad・poor・empty_poor。空なら全部）。judge と combo だけに効く
    std::vector<std::string> judges;
    // 効かせるプレースキン（.lr2skin のパス。実行ファイルのフォルダからの相対か絶対。空なら全部のスキン）
    std::vector<std::string> skins;
    // 方式：hue（色相をずらす）・range（近い色だけずらす）・map（グラデーションマップ）・mul（灰色にして掛ける）
    std::string mode = "hue";
    double hue = 0, sat = 100, lit = 0;  // hue・range：色相のずれ（度）、彩度（%）、明るさ（-50〜50。hue だけ）
    double from = 210, width = 35;       // range：変える色（色相の度）と幅（度）
    uint32_t color = 0xffffff;           // map・mul：色（RGB）
    double mix = 100;                    // map・mul：強さ（%）
    double white = 100;                  // map：いちばん明るい所の白さ（%。100 なら白、0 なら color のまま）
};

// 1 つの画素の色を変える（rgba の並び。透明度は変えない）
void recolor_pixel(uint8_t* px, const SkinColorFilter& f);
// フィルターを順に当てる。当てた部品の数を返す
int apply_skin_colors(Lr2Skin& sk, const std::vector<SkinColorFilter>& filters);
// from の (x, y, w, h) を写した絵を sk の画像に足して、その番号を返す（写せなければ -1）。from は sk の画像でもよい
int add_skin_crop(Lr2Skin& sk, const SkinImage& from, int x, int y, int w, int h);

}  // namespace rd
