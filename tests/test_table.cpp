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
