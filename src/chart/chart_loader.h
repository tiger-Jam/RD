// 譜面ファイルの読み込み。拡張子で形式を選ぶ（.osu → osu!mania、.bmson → bmson、それ以外 → BMS）
#pragma once

#include <string>

#include "chart/bms_parser.h"

namespace rd {

// 対応している譜面ファイルの拡張子か（大文字小文字は区別しない）
bool is_chart_path(const std::string& path);
// bytes はファイルの中身そのまま
BmsParseResult parse_chart_bytes(const std::string& path, const std::string& bytes, const BmsParseOptions& opts);
// ファイルを読んでパースする。chart.base_dir にファイルのあるフォルダを入れる
BmsParseResult parse_chart_file(const std::string& path, const BmsParseOptions& opts);

}  // namespace rd
