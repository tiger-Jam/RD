#include "app/course.h"

#include <algorithm>

namespace bmj {

bool Course::playable() const {
    if (stages.empty()) return false;
    for (const CourseStage& s : stages)
        if (!s.chart) return false;
    return true;
}

Course make_table_course(const TableCourse& tc, const std::unordered_map<std::string, const ChartRecord*>& by_md5,
                         const std::unordered_map<std::string, const ChartRecord*>& by_sha256) {
    Course c;
    c.name = tc.name;
    // 段位の制約（grade / grade_mirror / grade_random）は、先に書かれたものを使う
    for (const std::string& k : tc.constraints) {
        if (k == "grade" || k == "grade_mirror" || k == "grade_random") {
            c.type = 2;
            c.grade_random = k == "grade" ? 0 : k == "grade_mirror" ? 1 : 2;
            break;
        }
    }
    // 判定の制約も、先に書かれたものを使う
    for (const std::string& k : tc.constraints) {
        if (k == "no_good" || k == "no_great") {
            (k == "no_good" ? c.no_good : c.no_great) = true;
            break;
        }
    }
    for (const std::string& k : tc.constraints) {
        if (k == "ln" || k == "cn" || k == "hcn") {
            c.ln_mode = k == "ln" ? 0 : k == "cn" ? 1 : 2;
            break;
        }
    }
    c.no_speed = std::find(tc.constraints.begin(), tc.constraints.end(), "no_speed") != tc.constraints.end();
    for (const TableChart& t : tc.charts) {
        CourseStage s;
        s.md5 = t.md5, s.sha256 = t.sha256;
        if (!t.md5.empty())
            if (auto it = by_md5.find(t.md5); it != by_md5.end()) s.chart = it->second;
        if (!s.chart && !t.sha256.empty())
            if (auto it = by_sha256.find(t.sha256); it != by_sha256.end()) s.chart = it->second;
        if (s.md5.empty() && s.chart) s.md5 = s.chart->md5;
        c.stages.push_back(std::move(s));
    }
    // LR2 のコースハッシュの形：32 文字の頭（つなぎ方 10 桁・種類 1 桁・予約・作者 4 文字）＋各ステージの md5
    std::string head(32, '0');
    head[10] = static_cast<char>('0' + c.type);
    c.hash = head;
    for (const CourseStage& s : c.stages) c.hash += s.md5.empty() ? s.sha256 : s.md5;
    return c;
}

namespace {
const ChartRecord* stage_chart(const Course* c, int i) {
    return c && i >= 0 && i < static_cast<int>(c->stages.size()) ? c->stages[i].chart : nullptr;
}
}  // namespace

bool course_option(const Course* c, int op, bool& out) {
    switch (op) {
    case 290: out = c != nullptr; return true;
    case 291: out = c && c->type == 1; return true;
    case 292: out = c && c->type == 0; return true;  // エキスパート（LR2 の付属資料とは逆。実際のスキンに合わせる）
    case 293: out = c && c->type == 2; return true;  // 段位認定
    default: break;
    }
    if (op >= 580 && op <= 589) {
        out = c && static_cast<int>(c->stages.size()) > op - 580;
        return true;
    }
    if (op >= 700 && op <= 749 && op % 10 <= 5) {
        // ステージ (op-700)/10 の #DIFFICULTY が op%10（0 は未定義）
        const ChartRecord* ch = stage_chart(c, (op - 700) / 10);
        int d = ch ? (ch->difficulty >= 1 && ch->difficulty <= 5 ? ch->difficulty : 0) : -1;
        out = d == op % 10;
        return true;
    }
    return false;
}

bool course_number(const Course* c, int id, int& out) {
    if (id >= 250 && id <= 259) {
        const ChartRecord* ch = stage_chart(c, id - 250);
        out = ch ? ch->playlevel : -1;
        return true;
    }
    return false;
}

bool course_text(const Course* c, int id, std::string& out) {
    if (id >= 150 && id <= 169) {
        int i = id % 10;
        out.clear();
        if (c && i < static_cast<int>(c->stages.size())) {
            const ChartRecord* ch = c->stages[i].chart;
            if (id < 160) out = ch ? ch->title : "この曲を所持していません";
            else out = ch ? ch->subtitle : "";
        }
        return true;
    }
    if (id == 170) {
        out = c ? c->name : "";
        return true;
    }
    return false;
}

}  // namespace bmj
