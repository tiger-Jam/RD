// BGM のセット（MOD「BGM のセット」）：選曲・決定の BGM を、それぞれ手元のサウンドセットの中から選ぶ。
// 選択肢はサウンドセットごと、その音を #CUSTOMFILE で選ぶサウンドセットなら、その項目の候補ごと。
// 選んだものは「サウンドセットのパス|#CUSTOMFILE の番号|選んだ名前」の文字列で持つ（番号は #CUSTOMFILE だけを数えた順。無ければ -1）
#pragma once

#include <string>
#include <vector>

#include "app/config.h"
#include "audio/sound_bank.h"

namespace bmj {

struct BgmChoice {
    std::string key;   // 設定に書く値
    std::string name;  // 画面に出す名前（サウンドセットの名前と、選んだ候補）
};

// 同じ候補か（パスの区切りと大文字小文字の違いは見ない）
bool same_bgm_key(const std::string& a, const std::string& b);
// その音（kSysSelect・kSysDecide）の選択肢。lr2_root の LR2files/Sound の下のサウンドセットを全部見る
std::vector<BgmChoice> bgm_choices(const std::string& lr2_root, int sys_id);
// 選んだ音を読む（読めなければ false）
bool load_bgm_choice(const std::string& lr2_root, const AppConfig& cfg, const std::string& key, int sys_id, uint32_t rate, Sample& out);

}  // namespace bmj
