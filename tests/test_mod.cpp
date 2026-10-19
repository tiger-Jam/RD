#include <filesystem>

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
    const ModManifest *exhard = nullptr, *judge = nullptr, *gas = nullptr;
    for (const ModManifest& m : mods) {
        CHECK(m.official);
        CHECK(!m.hash.empty());
        if (m.id == "exhard") exhard = &m;
        if (m.id == "judge-sample") judge = &m;
        if (m.id == "gas") gas = &m;
    }
    CHECK(exhard && gas && judge);
    if (!exhard || !gas || !judge) return;
    CHECK_EQ(exhard->gauges.size(), size_t(1));
    CHECK(!exhard->affects_play());  // ゲージは評価だけ：記録を分けない
    CHECK(gas->gas && !gas->affects_play());
    CHECK(judge->affects_play());    // 判定は記録を分ける
    CHECK_EQ(judge->judges[0].pgreat[1], 22);
    CHECK(play_mod_key({exhard, gas}).empty());
    CHECK(play_mod_key({judge}).rfind("judge-sample@1#", 0) == 0);
    CHECK(mod_conflicts({judge, judge}).size() >= 2);  // 同じ id と、判定の MOD が 2 つ
    CHECK(mod_conflicts({exhard, gas}).empty());
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
