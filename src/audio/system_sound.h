// LR2のサウンドセット（LR2files/Sound/*.lr2ss）。決定音・選曲BGMなどのシステム音
// （参照: OpenLR2 LR2/LR2_audio.cpp の読み込み）
#pragma once

#include <string>
#include <vector>

#include "audio/sound_bank.h"

namespace bmj {

enum SystemSound : int {
    kSysSelect, kSysExSelect, kSysDecide, kSysExDecide, kSysFolderOpen, kSysFolderClose, kSysPanelOpen,
    kSysPanelClose, kSysOptionChange, kSysDifficulty, kSysScreenshot, kSysClear, kSysFail, kSysStop, kSysMine,
    kSysScratch, kSysCourseClear, kSysCourseFail, kSysSoundCount
};

// サウンドセットを読み、出力のサンプルレートに変換した音を返す（読めなかったものは空）
std::vector<Sample> load_system_sounds(const std::string& lr2_root, const std::string& lr2ss_path, uint32_t rate);

}  // namespace bmj
