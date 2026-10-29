#include <filesystem>
#include <fstream>

#include "chart/bms_parser.h"
#include "game/judge.h"
#include "mod/mod.h"
#include "test.h"

using namespace bmj;

static Chart mod_chart(const std::string& s) {
    BmsParseOptions o;
    o.random_seed = 1;
    return parse_bms_text(s, o).chart;
}

static int64_t mod_ms(int v) { return static_cast<int64_t>(v) * 1000; }

// 公式の MOD の定義が読めて、分類と中身が合っている
TEST(mod_official_manifests) {
    std::vector<std::string> errors;
    auto mods = scan_mods(std::string(BMJ_SOURCE_DIR) + "/mods", true, &errors);
    CHECK(errors.empty());
    ModManifest *exhard = nullptr, *assist = nullptr, *custom = nullptr, *judge = nullptr, *gas = nullptr;
    for (ModManifest& m : mods) {
        CHECK(m.official);
        CHECK(!m.hash.empty());
        if (m.id == "exhard") exhard = &m;
        if (m.id == "assist-easy") assist = &m;
        if (m.id == "custom-gauge") custom = &m;
        if (m.id == "custom-judge") judge = &m;
        if (m.id == "gas") gas = &m;
    }
    CHECK(exhard && assist && custom && gas && judge);
    if (!exhard || !assist || !custom || !gas || !judge) return;
    CHECK_EQ(exhard->gauges.size(), size_t(1));
    CHECK(exhard->gauges[0].id == "exhard" && exhard->gauges[0].frame == 7 && exhard->gauges[0].lamp == "exhard");
    CHECK_EQ(exhard->lamps.size(), size_t(1));
    CHECK(assist->gauges[0].frame == 8 && assist->gauges[0].colors == 2);
    CHECK(custom->gauges.empty() && custom->lamps.empty());  // 自作の分は詳細の設定で作る
    CHECK_EQ(custom->settings.size(), size_t(2));
    CHECK(custom->gauge_label == "CUSTOM" && custom->gauge_frame == 9);
    CHECK_EQ(gas->gas_order.size(), size_t(3));
    CHECK(!exhard->affects_play());  // ゲージは評価だけ：記録を分けない
    CHECK(gas->gas && !gas->affects_play());
    CHECK(play_mod_key({exhard, gas}).empty());
    // メトロノーム：音のファイルがあり、プレーには影響しない（記録を分けない）
    for (ModManifest& m : mods)
        if (m.id == "metronome") {
            CHECK(m.metronome && std::filesystem::exists(m.metronome_accent) && std::filesystem::exists(m.metronome_beat));
            CHECK((m.touches & static_cast<uint32_t>(ModTouch::Sound)) && !m.affects_play());
            m.metronome_volume = 35;
            const std::string f = (std::filesystem::temp_directory_path() / "bmj_metro.json").string();
            CHECK(save_mod_settings(m, f));
            ModManifest n = m;
            n.metronome_volume = 0;
            apply_mod_settings(n, f);
            CHECK_EQ(n.metronome_volume, 35);
            std::filesystem::remove(f);
        }
    // BGM のセット：音に触れる（記録は分けない）。セット（名前と選曲・決定の候補）と使うセットが設定に残る
    for (ModManifest& m : mods)
        if (m.id == "bgm-set") {
            CHECK(m.has_setting("bgm") && (m.touches & static_cast<uint32_t>(ModTouch::Sound)) && !m.affects_play());
            m.bgm_sets = {{"夜", {"LR2files/Sound/a.lr2ss|0|bgm2", "LR2files/Sound/a.lr2ss|0|bgm3"}, {"LR2files/Sound/b.lr2ss|-1|"}},
                          {"空", {}, {}}};
            m.bgm_use = "夜";
            const std::string f = (std::filesystem::temp_directory_path() / "bmj_bgm.json").string();
            CHECK(save_mod_settings(m, f));
            ModManifest n = m;
            n.bgm_sets.clear(), n.bgm_use.clear();
            apply_mod_settings(n, f);
            CHECK(n.bgm_use == "夜" && n.bgm_sets.size() == 2);
            if (n.bgm_sets.size() == 2)
                CHECK(n.bgm_sets[0].name == "夜" && n.bgm_sets[0].select == m.bgm_sets[0].select && n.bgm_sets[0].decide == m.bgm_sets[0].decide &&
                      n.bgm_sets[1].name == "空" && n.bgm_sets[1].select.empty());
            std::filesystem::remove(f);
        }
    // カスタム判定：遊ぶ判定を選ぶまでは、判定を変えず記録も分けない
    CHECK(judge->judges.empty() && !judge->affects_play() && play_mod_key({judge}).empty());
    judge->judges = standard_judge_samples();
    for (size_t i = 0; i < judge->judges.size(); ++i) judge->judges[i].id = "judge" + std::to_string(i + 1);  // 設定画面で足すと付く id
    judge->judge_use = judge->judges[1].id;  // VERY HARD と同じ幅
    CHECK(judge->affects_play());
    const std::string key = play_mod_key({judge});
    CHECK(key.rfind("custom-judge@judge#", 0) == 0);
    // 7k モードは MOD の組み合わせごとに別の記録
    CHECK(seven_key_record_key("") == "7k");
    CHECK(seven_key_record_key(key) == "7k," + key);
    judge->judges[1].name = "別の名前";          // 名前を変えても同じ記録
    CHECK(play_mod_key({judge}) == key);
    judge->judges[1].great[1] = 25;               // 幅を変えたら別の記録
    CHECK(play_mod_key({judge}) != key);
    judge->judges[1].great[1] = 24;               // 戻せば前の記録
    CHECK(play_mod_key({judge}) == key);
    // 本体の判定も直接選べる（#RANK に関係なくその幅）。同じ幅の自作の判定とは同じ記録
    judge->judge_use = "VERY HARD";
    CHECK(judge->active_judge() && judge->active_judge()->good[0] == 40 && !judge->active_judge()->rank_scaled);
    CHECK(play_mod_key({judge}) == key);
    judge->judge_use = "#RANK どおり";  // 選択肢にない見本（id が見本の名前のもの）は選べない
    CHECK(!judge->active_judge());
    judge->judge_use = judge->judges[1].id;
    CHECK(mod_conflicts({judge, judge}).size() >= 2);  // 同じ id と、判定の MOD が 2 つ
    CHECK(mod_conflicts({exhard, gas}).empty());
}

// ゲージのランプを引く：同じ MOD のランプ、ほかの MOD のランプ（「MOD/id」）、本体のランプ。番号の付くゲージの名前とコマ
TEST(mod_resolve_gauge_lamps) {
    std::vector<std::string> errors;
    auto mods = scan_mods(std::string(BMJ_SOURCE_DIR) + "/mods", true, &errors);
    ModManifest* custom = nullptr;
    for (ModManifest& m : mods)
        if (m.id == "custom-gauge") custom = &m;
    CHECK(custom);
    if (!custom) return;
    CustomLampDef purple;
    purple.id = "purple", purple.name = "PURPLE", purple.color = 0x8040ff, purple.rank = 4.2, purple.as = 3;
    custom->lamps = {purple};
    CustomGaugeDef a, b, c;
    a.id = "a", a.lamp = "purple";
    b.id = "b", b.lamp = "exhard/exhard";
    c.id = "c", c.lamp = "EASY", c.survival = false;
    custom->gauges = {a, b, c};
    resolve_gauge_lamps(mods);
    const auto& g = custom->gauges;
    CHECK(g[0].name == "CUSTOM1" && g[0].frame == 9 && g[2].name == "CUSTOM3" && g[2].frame == 11);
    CHECK(g[0].lamp_key == "custom-gauge/purple" && g[0].lamp_as == 4 && g[0].lamp_color == 0x8040ffu);  // 4.2 のすぐ下は HARD
    CHECK_EQ(g[0].lamp_rank, 4.2);
    CHECK(g[1].lamp_key == "exhard/exhard" && g[1].lamp_as == 4 && g[1].lamp_color == 0xffd800u);
    CHECK(g[2].lamp_key.empty() && g[2].lamp_as == 2);  // 本体の EASY：カスタムランプの記録は残さない
    for (const ModManifest& m : mods)
        if (m.id == "exhard") CHECK(m.gauges[0].lamp_key == "exhard/exhard" && m.gauges[0].name == "EX-HARD");
}

// カスタムゲージ：裏で全部計算し、選んだものはそれで遊び、ランプは lamp_as で見せる
TEST(mod_custom_gauge) {
    // 20 ノーツ（4 分おき。見逃したノーツを次のノーツの押下が拾わない間隔）
    std::string bms = "#BPM 120\n#RANK 2\n#TOTAL 200\n#WAV01 a.wav\n";
    for (int m = 1; m <= 5; ++m) bms += "#00" + std::to_string(m) + "11:01010101\n";
    Chart c = mod_chart(bms);
    CustomGaugeDef hard;
    hard.id = "x", hard.survival = true, hard.initial = 100;
    hard.delta[kPGreat].fixed = 0.1, hard.delta[kPoor].fixed = -60, hard.lamp_as = 4;
    CustomGaugeDef groove;
    groove.id = "y", groove.survival = false, groove.initial = 20, groove.border = 60, groove.floor = 2;
    groove.delta[kPGreat].total = 1.0, groove.delta[kPoor].fixed = -3;
    auto play = [&](int custom_play, int misses) {
        JudgeConfig cfg;
        cfg.custom_gauges = {hard, groove};
        cfg.custom_play = custom_play;
        JudgeEngine j(c, cfg);
        for (size_t i = 0; i < c.notes.size(); ++i)
            if (static_cast<int>(i) >= misses) j.press(c.notes[i].lane, c.notes[i].time_us);
        j.advance(c.notes.back().time_us + mod_ms(1000));
        return j;
    };
    {
        JudgeEngine j = play(0, 0);  // 全部 PGREAT
        CHECK_EQ(j.custom_count(), 2);
        CHECK(j.custom_cleared(0) && j.custom_cleared(1));
        CHECK_EQ(j.clear(), 6);  // フルコンボのランプはそのまま
        CHECK_EQ(j.display_gauge_type(), kHard);
        CHECK(j.custom_hp(1) > 60);  // 20% + 200×1.0
    }
    {
        JudgeEngine j = play(0, 2);  // 頭の 2 つを見逃す：−60 を 2 回で落ちる
        CHECK(j.failed());
        CHECK_EQ(j.custom_hp(0), 0.0);
        CHECK_EQ(j.play_hp(), 0.0);
    }
    {
        JudgeEngine j = play(1, 1);  // GROOVE 系で遊ぶ。1 つ見逃しても 60% を越えればクリア
        CHECK(!j.failed());
        CHECK(j.custom_cleared(1));
        CHECK_EQ(j.clear(), 4);  // lamp_as の既定（4）
        CHECK_EQ(j.display_gauge_type(), kGroove);
    }
    {
        JudgeConfig cfg;
        cfg.custom_gauges = {hard};
        cfg.custom_play = 0;
        cfg.course = 0;  // コースではカスタムゲージで遊ばない
        JudgeEngine j(c, cfg);
        CHECK_EQ(j.custom_play(), -1);
    }
}

// カスタム判定：早い側と遅い側で幅が違う
TEST(mod_custom_judge) {
    Chart c = mod_chart("#BPM 120\n#RANK 2\n#TOTAL 200\n#WAV01 a.wav\n#00111:01\n");
    CustomJudgeDef d;
    d.pgreat[0] = 10, d.pgreat[1] = 30;
    struct Case {
        int offset, judge;
    } cases[] = {{-10, kPGreat}, {-11, kGreat}, {30, kPGreat}, {31, kGreat}};
    for (auto cs : cases) {
        JudgeConfig cfg;
        cfg.custom_judge = d;
        JudgeEngine j(c, cfg);
        j.press(1, mod_ms(2000 + cs.offset));
        CHECK_EQ(j.events().size(), size_t(1));
        if (!j.events().empty()) CHECK_EQ(j.events()[0].judge, cs.judge);
    }
}

// GAS：決めた順番の先頭で始め、尽きたら次へ。曲の終わりにクリアの線に届いていなければ、クリアできた一番上のゲージ
TEST(mod_gas_chain) {
    std::string bms = "#BPM 120\n#RANK 2\n#TOTAL 200\n#WAV01 a.wav\n";
    for (int m = 1; m <= 5; ++m) bms += "#00" + std::to_string(m) + "11:01010101\n";
    Chart c = mod_chart(bms);
    auto play = [&](std::vector<int> chain, int misses) {
        JudgeConfig cfg;
        cfg.gas_chain = chain;
        JudgeEngine j(c, cfg);
        for (size_t i = 0; i < c.notes.size(); ++i)
            if (static_cast<int>(i) >= misses) j.press(c.notes[i].lane, c.notes[i].time_us);
        j.advance(c.notes.back().time_us + mod_ms(1000));
        j.gas_settle();
        return j;
    };
    {
        JudgeEngine j = play({kHard, kGroove, kEasy}, 0);  // 全部 PGREAT：HARD のまま
        CHECK_EQ(j.gauge_type(), kHard);
    }
    {
        // 1 つ見逃す：HAZARD が尽きて次へ。20 ノーツでは HARD も 1 つで尽きる（LR2 の式）ので GROOVE まで落ちる
        JudgeEngine j = play({kDeath, kHard, kGroove}, 1);
        CHECK_EQ(j.gauge_type(), kGroove);
        CHECK(!j.failed());
    }
    {
        JudgeEngine j = play({kHard, kGroove, kEasy}, 12);  // 半分以上見逃す：HARD が尽き、GROOVE も 80% に届かない
        CHECK(j.gauge_type() != kHard);
    }
}

// 詳細の設定：書いて読めば同じ（GAS の順番、カスタムゲージ、カスタムランプ）
TEST(mod_settings_roundtrip) {
    std::vector<std::string> errors;
    auto mods = scan_mods(std::string(BMJ_SOURCE_DIR) + "/mods", true, &errors);
    ModManifest *cg = nullptr, *gas = nullptr;
    for (ModManifest& m : mods) {
        if (m.id == "custom-gauge") cg = &m;
        if (m.id == "gas") gas = &m;
    }
    CHECK(cg && gas);
    if (!cg || !gas) return;
    const std::string dir = (std::filesystem::temp_directory_path() / "bmj_mod_settings").string();
    CustomLampDef lamp;
    lamp.id = "l1", lamp.name = "紫", lamp.color = 0x8040ff, lamp.rank = 4.2, lamp.as = 3;
    lamp.colors = 2, lamp.color2 = 0xffffff, lamp.blink_ms = 400;
    CHECK(lamp.color_at(100) == 0x8040ffu && lamp.color_at(500) == 0xffffffu);  // 0.4 秒ごとに入れ替わる
    CHECK_EQ(standard_gauge_samples().size(), size_t(6));
    CHECK_EQ(standard_lamp_samples().size(), size_t(6));
    CustomGaugeDef a, b;
    a.id = "g1", a.delta[kPoor].fixed = -20, a.colors = 2, a.color = 0x112233, a.color_low = 0x445566, a.lamp = "l1";
    b.id = "g2", b.survival = false, b.delta[kGreat].total = 0.75, b.lamp = "CLEAR";
    cg->lamps = {lamp}, cg->gauges = {a, b};
    CHECK(cg->standard_lamps[3].colors == 0);  // 本体のランプは、変えなければスキンの絵のまま
    cg->standard_lamps[3].colors = 2, cg->standard_lamps[3].color = 0x123456, cg->standard_lamps[3].color2 = 0x654321;
    CustomLampDef exh_color;
    exh_color.colors = 1, exh_color.color = 0x00ffff;
    cg->lamp_colors["exhard/exhard"] = exh_color;
    cg->standard_gauges[kGroove].colors = 2, cg->standard_gauges[kGroove].color_low = 0x00ff00;
    gas->gas_order = {"custom-gauge/g1", "HARD", "EASY"};
    CHECK(save_mod_settings(*cg, dir + "/cg.json"));
    CHECK(save_mod_settings(*gas, dir + "/gas.json"));
    ModManifest cg2 = *cg, gas2 = *gas;
    cg2.gauges.clear(), cg2.lamps.clear(), gas2.gas_order.clear(), cg2.lamp_colors.clear();
    cg2.standard_lamps = skin_standard_lamps();
    cg2.standard_gauges = {};
    apply_mod_settings(cg2, dir + "/cg.json");
    apply_mod_settings(gas2, dir + "/gas.json");
    CHECK_EQ(cg2.gauges.size(), size_t(2));
    CHECK_EQ(cg2.lamps.size(), size_t(1));
    if (cg2.gauges.size() == 2 && cg2.lamps.size() == 1) {
        CHECK_EQ(cg2.gauges[0].delta[kPoor].fixed, -20.0);
        CHECK_EQ(cg2.gauges[1].delta[kGreat].total, 0.75);
        CHECK(cg2.gauges[0].colors == 2 && cg2.gauges[0].color == 0x112233u && cg2.gauges[0].color_low == 0x445566u);
        CHECK(cg2.gauges[0].lamp == "l1" && cg2.gauges[1].lamp == "CLEAR");
        CHECK(cg2.gauges[1].survival == false);
        CHECK(cg2.lamps[0].name == "紫" && cg2.lamps[0].color == 0x8040ffu && cg2.lamps[0].as == 4);  // 絵は強さ 4.2 のすぐ下の HARD
        CHECK(cg2.lamps[0].colors == 2 && cg2.lamps[0].color2 == 0xffffffu && cg2.lamps[0].blink_ms == 400);
        CHECK_EQ(cg2.lamps[0].rank, 4.2);
    }
    CHECK(cg2.standard_lamps[3].colors == 2 && cg2.standard_lamps[3].color == 0x123456u && cg2.standard_lamps[3].color2 == 0x654321u);
    CHECK(cg2.standard_lamps[0].colors == 0);
    CHECK(cg2.standard_gauges[kGroove].colors == 2 && cg2.standard_gauges[kGroove].color_low == 0x00ff00u);
    CHECK(cg2.standard_gauges[kHard].colors == 0);  // 変えていないゲージはスキンの絵のまま
    CHECK(cg2.lamp_colors.count("exhard/exhard") && cg2.lamp_colors["exhard/exhard"].color == 0x00ffffu);
    // 使っているときだけ、EX-HARD の MOD のランプの色に重なる
    std::vector<ModManifest> set = {cg2};
    for (const ModManifest& x : mods)
        if (x.id == "exhard") set.push_back(x);
    apply_lamp_colors(set, {"exhard"});
    CHECK(set[1].lamps[0].color == 0xffd800u);
    apply_lamp_colors(set, {"custom-gauge", "exhard"});
    CHECK(set[1].lamps[0].color == 0x00ffffu);
    CHECK(gas2.gas_order == gas->gas_order);
    // カスタム判定：判定の一覧と遊ぶ判定
    for (ModManifest& x : mods)
        if (x.id == "custom-judge") {
            x.judges = standard_judge_samples();
            x.judges[2].id = "mine", x.judges[2].good[1] = 77, x.judges[2].rank_scaled = true;
            x.judge_use = "mine";
            CHECK(save_mod_settings(x, dir + "/cj.json"));
            ModManifest y = x;
            y.judges.clear(), y.judge_use.clear();
            apply_mod_settings(y, dir + "/cj.json");
            CHECK(y.judges.size() == x.judges.size() && y.judge_use == "mine");
            CHECK(y.active_judge() && y.active_judge()->good[1] == 77 && y.active_judge()->rank_scaled);
        }
    // 前の形（ゲージの中にランプを書いたもの）も読める：ランプはゲージと同じ id で一覧に入る
    {
        std::filesystem::create_directories(dir);
        std::ofstream f(dir + "/old.json");
        f << R"({"gauges": [{"id": "x", "name": "X", "lamp": {"name": "XL", "color": "#123456", "rank": 4.4, "as": 4}}]})";
    }
    ModManifest old = *cg;
    old.lamps.clear();
    apply_mod_settings(old, dir + "/old.json");
    CHECK(old.gauges.size() == 1 && old.gauges[0].lamp == "x");
    CHECK(old.lamps.size() == 1 && old.lamps[0].id == "x" && old.lamps[0].name == "XL" && old.lamps[0].color == 0x123456u);
    std::filesystem::remove_all(dir);
}

// メトロノームの拍：BPM と STOP から数え、小節線を頭にする（3/4 の小節は 3 拍）
TEST(metronome_beats) {
    // BPM 120（1 拍 500 ms）。0 小節目は 4/4、1 小節目は 3/4（#00102:0.75）、2 小節目の頭にノーツ
    Chart c = mod_chart("#BPM 120\n#WAV01 a.wav\n#00102:0.75\n#00211:01\n");
    auto b = c.metronome_beats();
    CHECK(b.size() >= 8);
    if (b.size() >= 8) {
        CHECK(b[0].head && b[0].time_us == 0);
        CHECK(!b[1].head && b[1].time_us == 500000);
        CHECK(b[4].head && b[4].time_us == 2000000);  // 1 小節目の頭
        CHECK(!b[6].head && b[6].time_us == 3000000);
        CHECK(b[7].head && b[7].time_us == 3500000);  // 3 拍で次の小節（2 小節目）
    }
    // STOP の間は拍が進まない：#STOP01 192（1 小節＝4 拍ぶん止まる → 2000 ms）を 1 小節目の頭に
    Chart s = mod_chart("#BPM 120\n#WAV01 a.wav\n#STOP01 192\n#00109:01\n#00211:01\n");
    auto sb = s.metronome_beats();
    bool found = false;
    for (const auto& x : sb)
        if (x.head && x.time_us == 2000000) found = true;
    CHECK(found);
    // 1 小節目の 2 拍目は、止まっていた 2000 ms のあと
    for (const auto& x : sb)
        if (!x.head && x.time_us > 2000000 && x.time_us < 5000000) {
            CHECK_EQ(x.time_us, int64_t(4500000));
            break;
        }
}

// skin_colors を読むと、見た目の MOD になる（記録は分けない）
TEST(mod_skin_colors) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmj_mod_test_skin_colors";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "mod.json") << R"({"id":"note-color","skin_colors":[
        {"parts":["note","ln"],"lanes":["white","blue",3],"mode":"map","color":"#23d100","mix":80},
        {"parts":[],"mode":"hue"}]})";
    ModManifest m;
    CHECK(load_mod_manifest(dir.string(), m));
    CHECK_EQ(m.skin_colors.size(), size_t(1));  // 部品の無いものは読まない
    if (!m.skin_colors.empty()) {
        const SkinColorFilter& f = m.skin_colors[0];
        CHECK(f.parts.size() == 2 && f.lanes.size() == 3 && f.lanes[2] == "3");
        CHECK(f.mode == "map" && f.color == 0x23d100 && f.mix == 80);
    }
    CHECK((m.touches & static_cast<uint32_t>(ModTouch::Visual)) != 0);
    CHECK((m.touches & kModAffectsPlay) == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 部品の色の設定は settings.json に書いて、読み直すと同じになる（設定のほうが mod.json より優先）
TEST(mod_skin_colors_settings) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmj_mod_test_skin_colors_settings";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "mod.json") << R"({"id":"c","settings":[{"type":"skin_colors","name":"部品の色"}],
        "skin_colors":[{"parts":["note"],"mode":"map","color":"#23d100"}]})";
    ModManifest m;
    CHECK(load_mod_manifest(dir.string(), m));
    CHECK(m.has_setting("skin_colors"));
    CHECK_EQ(m.skin_colors.size(), size_t(1));
    m.skin_colors[0].parts = {"note", "ln"};
    m.skin_colors[0].lanes = {"white", "blue"};
    SkinColorFilter extra;
    extra.parts = {"judge"}, extra.judges = {"pgreat"}, extra.mode = "range", extra.from = 200, extra.hue = -45.5;
    extra.skins = {"LR2files/Theme/WMIX_HD/play/HDPLAY_AC.lr2skin"};
    m.skin_colors.push_back(extra);
    const std::string path = (dir / "settings.json").string();
    CHECK(save_mod_settings(m, path));
    ModManifest r;
    CHECK(load_mod_manifest(dir.string(), r));
    apply_mod_settings(r, path);
    CHECK_EQ(r.skin_colors.size(), size_t(2));
    if (r.skin_colors.size() == 2) {
        CHECK(r.skin_colors[0].parts.size() == 2 && r.skin_colors[0].lanes.size() == 2 && r.skin_colors[0].color == 0x23d100);
        CHECK(r.skin_colors[1].mode == "range" && r.skin_colors[1].from == 200 && r.skin_colors[1].hue == -45.5);
        CHECK(r.skin_colors[1].judges.size() == 1 && r.skin_colors[1].judges[0] == "pgreat");
        CHECK(r.skin_colors[0].skins.empty());
        CHECK(r.skin_colors[1].skins.size() == 1 && r.skin_colors[1].skins[0] == "LR2files/Theme/WMIX_HD/play/HDPLAY_AC.lr2skin");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
