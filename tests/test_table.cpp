#include <chrono>
#include "app/course.h"
#include "db/difficulty_table.h"
#include "test.h"

using namespace bmj;

TEST(table_resolve_url) {
    CHECK(resolve_url("https://a.b/x/table.html", "header.json") == "https://a.b/x/header.json");
    CHECK(resolve_url("https://a.b/x/table.html", "./header.json") == "https://a.b/x/header.json");
    CHECK(resolve_url("https://a.b/x/y/header.json", "../data.json") == "https://a.b/x/data.json");
    CHECK(resolve_url("https://a.b/x/table.html", "/h.json") == "https://a.b/h.json");
    CHECK(resolve_url("https://a.b/x/table.html", "https://c.d/data.json") == "https://c.d/data.json");
    CHECK(resolve_url("https://a.b", "h.json") == "https://a.b/h.json");
}

TEST(table_meta) {
    CHECK(find_bmstable_meta("<html><head><meta name=\"bmstable\" content=\"header.json\" /></head>") == "header.json");
    CHECK(find_bmstable_meta("<META content='Head.JSON' NAME=bmstable>") == "Head.JSON");
    CHECK(find_bmstable_meta("<meta name=\"x\" content=\"y\">").empty());
}

TEST(table_parse) {
    std::string header = "\xEF\xBB\xBF{\"name\":\"T\",\"symbol\":\"t\",\"data_url\":\"d.json\"}";
    std::string data = R"([{"md5":"ABC","level":"2","title":"b"},{"md5":"def","level":"10","title":"c"},
                           {"md5":"123","level":1,"title":"a"},{"level":"1","title":"no hash"}])";
    DifficultyTable t;
    std::string err;
    CHECK(parse_table(header, data, t, err));
    CHECK(t.name == "T");
    CHECK_EQ(t.charts.size(), size_t(3));
    CHECK(t.charts[0].md5 == "abc");  // 小文字にそろえる
    // level_order が無ければ数値順
    CHECK_EQ(t.levels.size(), size_t(3));
    if (t.levels.size() == 3) CHECK(t.levels[0] == "1" && t.levels[1] == "2" && t.levels[2] == "10");
    std::string header2 = R"({"name":"T","data_url":"d","level_order":["10","2",1]})";
    DifficultyTable t2;
    CHECK(parse_table(header2, data, t2, err));
    CHECK(t2.levels.size() == 3 && t2.levels[0] == "10" && t2.levels[2] == "1");
    DifficultyTable bad;
    CHECK(!parse_table(header, "<html>", bad, err));
}

TEST(table_courses) {
    std::string data = R"([{"md5":"a1","level":"1"}])";
    // 並びの並び。charts の形と、古い md5 の並びの形
    std::string header = R"({"name":"T","symbol":"t","course":[[
        {"name":"初段","constraint":["grade_mirror","gauge_lr2"],"md5":["AA","bb"]},
        {"name":"EX","charts":[{"md5":"cc"},{"sha256":"DD"}]}]]})";
    DifficultyTable t;
    std::string err;
    CHECK(parse_table(header, data, t, err));
    CHECK_EQ(t.courses.size(), size_t(2));
    if (t.courses.size() == 2) {
        CHECK(t.courses[0].name == "初段");
        CHECK(t.courses[0].charts.size() == 2 && t.courses[0].charts[0].md5 == "aa");
        CHECK(t.courses[0].constraints.size() == 2);
        CHECK(t.courses[1].charts.size() == 2 && t.courses[1].charts[1].sha256 == "dd");
        CHECK(t.courses[1].constraints.empty());
    }
    // 古い grade は、段位・MIRROR 可として読む
    std::string old = R"({"name":"T","grade":[{"name":"G","md5":["x"]}]})";
    DifficultyTable t2;
    CHECK(parse_table(old, data, t2, err));
    CHECK(t2.courses.size() == 1 && t2.courses[0].constraints.size() == 2 && t2.courses[0].constraints[0] == "grade_mirror");
}

// 難易度表の更新は 1 日 1 回：その日（この機械の時刻の日付）に取ったかどうか
TEST(table_checked_today) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(is_today(now));
    CHECK(!is_today(now - 2 * 86400));
    CHECK(!is_today(0));
}

// コースの組の名前（実際の難易度表に入っているコースの名前で）
TEST(table_course_group_label) {
    auto label = [](std::vector<std::string> names, const std::string& first_md5 = "") {
        std::vector<TableCourse> cs(names.size());
        for (size_t i = 0; i < names.size(); ++i) cs[i].name = names[i];
        if (!first_md5.empty()) cs[0].charts.push_back(TableChart{}), cs[0].charts[0].md5 = first_md5;
        std::vector<const TableCourse*> list;
        for (const TableCourse& c : cs) list.push_back(&c);
        return course_group_label(list, "表");
    };
    CHECK(label({"GENOCIDE 2018 発狂初段", "GENOCIDE 2018 発狂二段", "GENOCIDE 2018 overjoy"}) == "GENOCIDE 2018");
    CHECK(label({"GENOCIDE 2018 No Good発狂初段", "GENOCIDE 2018 No Good発狂二段", "GENOCIDE 2018 No good overjoy"}) ==
          "GENOCIDE 2018 No Good");
    CHECK(label({"GENOCIDE 2018 ノースピ発狂初段", "GENOCIDE 2018 ノースピ発狂二段", "GENOCIDE 2018 ノースピoverjoy"}) ==
          "GENOCIDE 2018 ノースピ");
    CHECK(label({"NEW GENERATION 発狂入門", "NEW GENERATION 発狂初段", "NEW GENERATION 発狂皆伝"}) == "NEW GENERATION");
    CHECK(label({"Satellite Skill Analyzer 2nd sl0", "Satellite Skill Analyzer 2nd sl1", "Satellite Skill Analyzer 2nd sl12"}) ==
          "Satellite Skill Analyzer 2nd");
    CHECK(label({"Solomon 第1期 発狂九段", "Solomon 第2期 発狂六段"}) == "Solomon");
    CHECK(label({"Renda Skill Simulator RENDAMAN", "Renda Skill Simulator Lv10"}) == "Renda Skill Simulator");
    CHECK(label({"Stella Skill Simulator 4th st0", "STELLA技能訓練 st Boys"}) == "Stella");  // 大文字小文字は区別しない
    CHECK(label({"Alpha", "Beta"}) == "表");  // 共通の頭が無ければ表の名前
    CHECK(label({"発狂初段", "発狂二段", "overjoy"}, "c07125de4ed7fbe7cb066cc41e50e51e") == "発狂BMS段位認定 2012");
}

TEST(table_course_hash) {
    TableCourse tc;
    tc.constraints = {"grade_mirror", "gauge_lr2"};
    tc.charts.resize(2);
    tc.charts[0].md5 = std::string(32, 'a'), tc.charts[1].md5 = std::string(32, 'b');
    // LR2 の段位と同じ形（頭の 11 文字目が種類）。LR2 から取り込んだ記録はこれで引く
    const std::string normal = make_table_course(tc, {}, {}).hash;
    CHECK(normal == "00000000002" + std::string(21, '0') + std::string(32, 'a') + std::string(32, 'b'));
    // ノースピ・No Good は、同じ曲でも普通の段位と別の記録にする
    tc.constraints = {"grade_mirror", "no_speed"};
    const std::string nospeed = make_table_course(tc, {}, {}).hash;
    tc.constraints = {"grade_mirror", "no_good"};
    const std::string nogood = make_table_course(tc, {}, {}).hash;
    CHECK(nospeed != normal && nogood != normal && nospeed != nogood);
}
