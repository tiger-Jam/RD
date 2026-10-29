#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

#include "mod/script.h"
#include "test.h"

using namespace rd;
namespace fs = std::filesystem;

namespace {

// 一時フォルダに MOD を 1 つ作る
struct TempMod {
    fs::path root, data;
    ModManifest m;
    TempMod(const std::string& id, const std::string& touches, const std::string& lua) {
        root = fs::temp_directory_path() / ("rd_mod_test_" + id);
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
          result.up = rd.write_file("../escape.txt", "x") == nil
          result.abs = rd.write_file("/tmp/escape.txt", "x") == nil
          result.ok = rd.write_file("sub/ok.txt", info.title) == true
          rd.write_file("flags.txt", tostring(result.io and result.up and result.abs and result.ok))
        end
        function on_draw(s) rd.draw_rect(1, 2, 3, 4, 0xff0000, 128); rd.draw_text(5, 6, "x", 20) end
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
        function on_draw(s) rd.draw_rect(0, 0, 1, 1) end
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

// 譜面に曲名の前の文字（label）を付けられる
TEST(mod_script_folder_labels) {
    TempMod t("labels", "\"control\"", R"LUA(
        function build_folders(charts)
          return { { title = "印", keep_order = true, charts = { "a", { sha256 = "b", label = "50.0%" }, { sha256 = "c" }, { label = "x" } } } }
        end
    )LUA");
    auto f = build_mod_folders({&t.m}, {}, {}, t.data.string());
    CHECK_EQ(f.size(), size_t(1));
    if (f.size() != 1) return;
    CHECK(f[0].charts == (std::vector<std::string>{"a", "b", "c"}));  // sha256 の無い表は飛ばす
    CHECK(f[0].labels == (std::vector<std::string>{"", "50.0%", ""}));
    CHECK(f[0].keep_order);
}

// 公式の MOD「リコメンド」：walkure-offline と同じ地力と確率になる（値は walkure-offline の JS で同じ記録から求めたもの）
TEST(mod_script_official_walkure) {
    ModManifest m;
    CHECK(load_mod_manifest(std::string(RD_SOURCE_DIR) + "/mods/walkure", m));
    // モデルの譜面を全部持っていることにし、7 つおきにランプを順に付け、600 番目から 7 つおきは FAILED にする
    std::ifstream in(std::string(RD_SOURCE_DIR) + "/mods/walkure/model.tsv");
    std::string line;
    std::getline(in, line);
    std::vector<ChartRecord> charts;
    std::unordered_map<std::string, ScoreRecord> scores;
    for (int i = 0; std::getline(in, line); ++i) {
        ChartRecord c;
        c.md5 = c.sha256 = line.substr(0, 32), c.title = "t" + std::to_string(i);
        if (i % 7 == 0) scores[c.sha256].clear = i / 7 % 5 + 1;
        if (i >= 600 && (i - 600) % 7 == 0) scores[c.sha256].clear = 1;
        charts.push_back(c);
    }
    CHECK_EQ(charts.size(), size_t(1252));
    fs::path data = fs::temp_directory_path() / "rd_mod_test_walkure";
    std::vector<std::string> errors;
    auto f = build_mod_folders({&m}, charts, scores, data.string(), &errors);
    CHECK(errors.empty());
    CHECK_EQ(f.size(), size_t(1));
    if (f.size() != 1) return;
    CHECK(f[0].title == "リコメンド  ★11.88");
    CHECK_EQ(f[0].children.size(), size_t(4));
    if (f[0].children.size() != 4) return;
    const ModFolder& hard = f[0].children[2];
    CHECK(hard.title == "HARD");
    CHECK_EQ(hard.charts.size(), size_t(494));  // walkure-offline の 496 から、入れていないコース（段位）の 2 つを除いたもの
    CHECK(hard.charts.size() >= 3 && hard.charts[0] == "4b2ef6fdb08c645bf82e5ee1c78737ae" &&
          hard.charts[1] == "1b4566b508cb58387480a0406d87cb1b" && hard.charts[2] == "6d5b9c4409daada59c8968f9129cb789");
    CHECK(!hard.labels.empty() && hard.labels[0] == "98.5%");
    CHECK_EQ(f[0].children[0].charts.size(), size_t(670));
    CHECK_EQ(f[0].children[1].charts.size(), size_t(506));  // 513 からコースの 7 つを除いたもの
    CHECK_EQ(f[0].children[3].charts.size(), size_t(2));
    std::error_code ec;
    fs::remove_all(data, ec);
}

// 公式のスクリプトの MOD が読めて動く（判定の記録を書き出す）
TEST(mod_script_official_judge_log) {
    ModManifest m;
    CHECK(load_mod_manifest(std::string(RD_SOURCE_DIR) + "/mods/judge-log", m));
    fs::path data = fs::temp_directory_path() / "rd_mod_test_judgelog";
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
    CHECK(load_mod_manifest(std::string(RD_SOURCE_DIR) + "/mods/random-select", m));
    fs::path data = fs::temp_directory_path() / "rd_mod_test_random";
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

// 公式の MOD「譜面の傾向」：難易度表 → レベル → 傾向・プレー状況などのフォルダ（中身の無いフォルダは出さない）
TEST(mod_script_official_constellator) {
    ModManifest m;
    CHECK(load_mod_manifest(std::string(RD_SOURCE_DIR) + "/mods/constellator", m));
    CHECK(m.chart_attr);
    std::vector<ChartRecord> charts(3);
    const char* ids[] = {"a", "b", "c"};
    for (int i = 0; i < 3; ++i) charts[i].sha256 = ids[i], charts[i].md5 = std::string("m") + ids[i], charts[i].notes = 1000, charts[i].length_ms = 120000;
    std::unordered_map<std::string, ScoreRecord> scores;
    scores["a"].clear = 4, scores["a"].playcount = 3, scores["a"].exscore = 1700, scores["a"].bp = 40, scores["a"].last_played = 1;
    std::unordered_map<std::string, ChartAttr> attrs;
    attrs["a"].ok = true, attrs["a"].attr = kAttrRanda, attrs["a"].sub = kSubRandaMid, attrs["a"].mask = 1, attrs["a"].randa_33 = 0.2;
    attrs["b"].ok = true, attrs["b"].attr = kAttrUde, attrs["b"].sub = kSubUdeNoFull, attrs["b"].mask = 2 | 4;
    std::unordered_map<std::string, std::vector<ModTableLevel>> tables;
    for (const char* id : ids) tables[std::string("m") + id] = {{"表", "★", "3", 0, 2}};
    ModChartExtra extra{&attrs, &tables};
    fs::path data = fs::temp_directory_path() / "rd_mod_test_constellator";
    std::vector<std::string> errors;
    auto f = build_mod_folders({&m}, charts, scores, data.string(), &errors, extra);
    CHECK(errors.empty());
    CHECK_EQ(f.size(), size_t(1));
    if (f.size() != 1) return;
    auto find = [](const std::vector<ModFolder>& v, const std::string& t) -> const ModFolder* {
        for (const ModFolder& x : v)
            if (x.title == t) return &x;
        return nullptr;
    };
    const ModFolder* table = find(f[0].children, "表");
    const ModFolder* level = table ? find(table->children, "★3") : nullptr;
    CHECK(level != nullptr);
    if (!level) return;
    CHECK(find(level->children, "すべて") && find(level->children, "すべて")->charts.size() == 3);
    const ModFolder* trend = find(level->children, "傾向");
    CHECK(trend && find(trend->children, "16分乱打") && find(trend->children, "腕ガチ") && !find(trend->children, "ディレイ"));
    if (trend && find(trend->children, "16分乱打")) {
        const ModFolder* randa = find(trend->children, "16分乱打");
        CHECK(find(randa->children, "16分乱打 中速乱打") && find(randa->children, "16分乱打 3-3") && !find(randa->children, "16分乱打 高速乱打"));
    }
    const ModFolder* comp = find(level->children, "複合");
    CHECK(comp && find(comp->children, "腕ガチ+指ガチ") && find(comp->children, "腕ガチ+指ガチ")->charts == std::vector<std::string>{"b"});
    const ModFolder* play = find(level->children, "プレー状況");
    CHECK(play && find(play->children, "未プレイ") && find(play->children, "未プレイ")->charts == (std::vector<std::string>{"b", "c"}));
    CHECK(play && find(play->children, "最終プレイから30日以上"));  // 1970 年に遊んだことになっている
    const ModFolder* clear = find(level->children, "クリア状況");
    CHECK(clear && find(clear->children, "HARD以上") && find(clear->children, "BP30以上") && !find(clear->children, "BP50以上"));
    const ModFolder* score = find(level->children, "スコア状況");
    CHECK(score && find(score->children, "AAA未満") && !find(score->children, "AA未満"));  // 85%
    std::error_code ec;
    fs::remove_all(data, ec);
}
