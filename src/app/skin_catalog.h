// スキンの一覧と、スキンセレクト（スキンの種類・スキン・カスタマイズ項目を選ぶ）の状態。
// 選曲画面のパネル（WMIX のパネル2）とスキンセレクト画面の両方で使う。番号の意味は docs/lr2_skin_ids.md
#pragma once

#include <map>
#include <string>
#include <vector>

#include "app/config.h"
#include "skin/lr2_skin.h"

namespace bmj {

// 設定に書かれたカスタマイズの選択を、スキンのヘッダ（各項目の selected）に当てはめる。
// #CUSTOMFILE は名前で探し、候補に無ければ既定のまま。古い形式（番号）なら番号で選ぶ
void apply_skin_custom(const std::vector<SkinCustomChoice>& sel, SkinHeader& h);
void apply_skin_custom(const AppConfig& cfg, const std::string& skin_path, SkinHeader& h);
// ヘッダの selected を、設定に書く形にする
std::vector<SkinCustomChoice> skin_custom_choices(const SkinHeader& h);

struct SkinEntry {
    std::string path;
    SkinHeader header;
};

class SkinCatalog {
public:
    // LR2files/Theme の下の .lr2skin と、LR2files/Sound の下の .lr2ss を集める
    void scan(const std::string& lr2_root);

    // 種類（LR2 の #INFORMATION の番号。0=7鍵プレイ 5=選曲 6=決定 7=リザルト 10=サウンドセット …）
    int selected_type() const { return type_; }
    void select_type(int type, const AppConfig& cfg);
    // いま選んでいる種類で使っているスキン（無ければ nullptr）
    const SkinEntry* current(const AppConfig& cfg) const;
    // スキンを前後に替える（ボタン190）。設定を書き換えたら true
    bool step_skin(AppConfig& cfg, int dir);
    // カスタマイズ項目（スクロール位置 + k 番目）の選択を前後に替える（ボタン220〜229）。設定を書き換えたら true
    bool step_custom(AppConfig& cfg, int k, int dir);
    // カスタマイズ項目のスクロール（スライダー7）
    int scroll() const { return scroll_; }
    int scroll_max(const AppConfig& cfg) const;
    void set_scroll(int v, const AppConfig& cfg);

    // スキンに渡す値
    std::string text(int id, const AppConfig& cfg) const;
    int button(int type) const;

    // 種類ごとに、BMjoy の設定のどこにスキンのパスを持つか（BMjoy で替えられない種類は nullptr）
    static std::string* config_slot(AppConfig& cfg, int type);
    static const std::string* config_slot(const AppConfig& cfg, int type);

    // 集めたスキン（種類ごと）
    const std::map<int, std::vector<SkinEntry>>& entries() const { return by_type_; }

private:
    // 設定に書かれたカスタマイズの選択を、項目の並びに当てはめる
    std::vector<int> selections(const SkinEntry& e, const AppConfig& cfg) const;
    std::string default_path(int type) const;

    std::string root_;
    std::map<int, std::vector<SkinEntry>> by_type_;
    int type_ = 0;
    int scroll_ = 0;
};

}  // namespace bmj
