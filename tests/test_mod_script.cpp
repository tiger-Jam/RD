#include <filesystem>
#include <fstream>
#include <set>

#include "mod/script.h"
#include "test.h"

using namespace bmj;
namespace fs = std::filesystem;

namespace {

// 一時フォルダに MOD を 1 つ作る
struct TempMod {
    fs::path root, data;
    ModManifest m;
    TempMod(const std::string& id, const std::string& touches, const std::string& lua) {
        root = fs::temp_directory_path() / ("bmj_mod_test_" + id);
        data = root / "data";
        fs::remove_all(root);
        fs::create_directories(root / id);
        std::ofstream(root / id / "mod.json") << "{\"id\":\"" << id << "\",\"touches\":[" << touches << "],\"script\":\"main.lua\"}";
        std::ofstream(root / id / "main.lua") << lua;
        load_mod_manifest((root / id).string(), m);
    }
    ~TempMod() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

// 使えない関数が外れていて、書けるのは自分のフォルダだけ
TEST(mod_script_sandbox) {
    TempMod t("sandbox", "\"visual\"", R"LUA(
        result = {}
        function on_play_start(info)
          result.io = io == nil and os == nil and dofile == nil and loadfile == nil and load == nil and require == nil
          result.up = bmj.write_file("../escape.txt", "x") == nil
          result.abs = bmj.write_file("/tmp/escape.txt", "x") == nil
          result.ok = bmj.write_file("sub/ok.txt", info.title) == true
          bmj.write_file("flags.txt", tostring(result.io and result.up and result.abs and result.ok))
        end
        function on_draw(s) bmj.draw_rect(1, 2, 3, 4, 0xff0000, 128); bmj.draw_text(5, 6, "x", 20) end
    )LUA");
    ModScripts s;
    s.load({&t.m}, t.data.string());
    CHECK(!s.empty());
    ModPlayInfo info;
    info.title = "曲";
    s.play_start(info);
    CHECK(s.take_errors().empty());
    std::ifstream f(t.data / "sandbox" / "flags.txt");
    std::string flags;
    f >> flags;
    CHECK(flags == "true");
    CHECK(fs::exists(t.data / "sandbox" / "sub" / "ok.txt"));
    CHECK(!fs::exists(t.data / "escape.txt"));
    const auto& d = s.draw({});
    CHECK_EQ(d.size(), size_t(2));
    if (d.size() == 2) {
        CHECK(d[0].kind == ModDrawCmd::Kind::Rect && d[0].rgb == 0xff0000u && d[0].alpha == 128);
        CHECK(d[1].kind == ModDrawCmd::Kind::Text && d[1].size == 20);
    }
}

// 止まらないスクリプトは打ち切り、その MOD だけ止める。見た目を宣言していない MOD は描けない
TEST(mod_script_limits) {
    TempMod t("loop", "\"evaluation\"", R"LUA(
        n = 0
        function on_judge(e) n = n + 1; if e.judge == 5 then while true do end end end
        function on_draw(s) bmj.draw_rect(0, 0, 1, 1) end
    )LUA");
    ModScripts s;
    s.load({&t.m}, t.data.string());
    ModJudgeInfo e;
    e.judge = 5;
    s.judge(e);
    CHECK_EQ(s.take_errors().size(), size_t(1));
    s.judge(e);  // 止めた MOD はもう呼ばない
    CHECK(s.take_errors().empty());
    CHECK(s.draw({}).empty());
}

// フォルダを作る MOD：返した表がフォルダになる
TEST(mod_script_folders) {
    TempMod t("folders", "\"control\"", R"LUA(
        function build_folders(charts)
          local hard, all = {}, {}
          for _, c in ipairs(charts) do
            all[#all + 1] = c.sha256
            if c.clear >= 4 then hard[#hard + 1] = c.sha256 end
          end
          return { { title = "全部", charts = all, folders = { { title = "HARD 以上", charts = hard } } } }
        end
    )LUA");
    std::vector<ChartRecord> charts(3);
    charts[0].sha256 = "a", charts[1].sha256 = "b", charts[2].sha256 = "c";
    std::unordered_map<std::string, ScoreRecord> scores;
    scores["b"].clear = 5;
    std::vector<std::string> errors;
    auto f = build_mod_folders({&t.m}, charts, scores, t.data.string(), &errors);
    CHECK(errors.empty());
    CHECK_EQ(f.size(), size_t(1));
    if (f.size() == 1) {
        CHECK(f[0].title == "全部");
        CHECK_EQ(f[0].charts.size(), size_t(3));
        CHECK_EQ(f[0].children.size(), size_t(1));
        if (!f[0].children.empty()) CHECK(f[0].children[0].charts == std::vector<std::string>{"b"});
    }
}

// 公式のスクリプトの MOD が読めて動く（判定の記録を書き出す）
TEST(mod_script_official_judge_log) {
    ModManifest m;
    CHECK(load_mod_manifest(std::string(BMJ_SOURCE_DIR) + "/mods/judge-log", m));
    fs::path data = fs::temp_directory_path() / "bmj_mod_test_judgelog";
    fs::remove_all(data);
    ModScripts s;
    s.load({&m}, data.string());
    ModPlayInfo info;
    info.title = "A, \"B\"", info.sha256 = "0123456789abcdef";
    s.play_start(info);
    ModJudgeInfo e;
    e.judge = 5, e.has_offset = true, e.offset_ms = -3, e.fast = true, e.combo = 1, e.exscore = 2;
    s.judge(e);
    ModPlayResult r;
    s.play_end(r);
    CHECK(s.take_errors().empty());
    int files = 0;
    std::string text;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(data / "judge-log", ec)) {
        ++files;
        std::ifstream in(f.path(), std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK_EQ(files, 1);
    CHECK(text.find("\"A, \"\"B\"\"\"") != std::string::npos);
    CHECK(text.find("PGREAT,-3,FAST,1,2") != std::string::npos);
    fs::remove_all(data, ec);
}

// 公式のランダムセレクト：譜面のあるフォルダにだけバーを出し、決定すると渡した譜面から 1 つを選ぶ
TEST(mod_script_official_random_select) {
    ModManifest m;
    CHECK(load_mod_manifest(std::string(BMJ_SOURCE_DIR) + "/mods/random-select", m));
    fs::path data = fs::temp_directory_path() / "bmj_mod_test_random";
    std::vector<std::string> errors;
    auto bars = mod_folder_bars({&m}, "★18", 3, data.string(), &errors);
    CHECK(errors.empty());
    CHECK_EQ(bars.size(), size_t(1));
    CHECK(mod_folder_bars({&m}, "フォルダだけ", 0, data.string()).empty());
    if (bars.size() != 1) return;
    CHECK(bars[0].title == "?????" && bars[0].roulette && bars[0].mod_id == "random-select");
    std::vector<ChartRecord> charts(3);
    charts[0].sha256 = "a", charts[1].sha256 = "b", charts[2].sha256 = "c";
    std::vector<const ChartRecord*> view{&charts[0], &charts[1], &charts[2]};
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        std::string err;
        std::string sha = mod_pick_chart(m, bars[0], "★18", view, {}, data.string(), &err);
        CHECK(err.empty());
        seen.insert(sha);
    }
    CHECK(seen == (std::set<std::string>{"a", "b", "c"}));  // 50 回で 3 つとも出る（毎回同じではない）
    CHECK(mod_pick_chart(m, bars[0], "空", {}, {}, data.string()).empty());
    // ルーレットに出ていた譜面を遊ぶ
    CHECK(mod_pick_chart(m, bars[0], "★18", view, {}, data.string(), nullptr, "b") == "b");
    std::error_code ec;
    fs::remove_all(data, ec);
}
