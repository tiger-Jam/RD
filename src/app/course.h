// コース（段位認定・エキスパート）。番号の意味は docs/lr2_course_spec.md
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "db/difficulty_table.h"
#include "db/song_db.h"

namespace bmj {

struct CourseStage {
    std::string md5, sha256;
    const ChartRecord* chart = nullptr;  // 持っていなければ nullptr
};

struct Course {
    std::string name;
    int type = 0;  // LR2 のコースの種類：0 エキスパート 1 ノンストップ 2 段位認定
    bool allow_mirror = false;  // 段位でも MIRROR を使える（難易度表の grade_mirror）
    std::vector<CourseStage> stages;
    std::string hash;  // スコアのキー

    // 全ステージの譜面を持っている
    bool playable() const;
};

// 難易度表のコースを、手元の譜面に結び付ける
Course make_table_course(const TableCourse& tc, const std::unordered_map<std::string, const ChartRecord*>& by_md5,
                         const std::unordered_map<std::string, const ChartRecord*>& by_sha256);

// コースのときだけ意味のあるスキンの値（選曲・決定・プレイ・リザルト・コースリザルトで同じ）。
// 扱う番号なら true を返して out に値を入れる。course が nullptr ならコースでない
bool course_option(const Course* course, int op, bool& out);
bool course_number(const Course* course, int id, int& out);
bool course_text(const Course* course, int id, std::string& out);

}  // namespace bmj
