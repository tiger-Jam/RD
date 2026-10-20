#include <cmath>
#include <filesystem>
#include <fstream>

#include "app/lr2_import.h"
#include "chart/bmson_parser.h"
#include "chart/osu_parser.h"
#include "test.h"
#include "util/hash.h"

using namespace bmj;

static const char* kOsu7 = R"(osu file format v14

[General]
AudioFilename: audio.mp3
Mode: 3

[Metadata]
Title:Song
TitleUnicode:曲
Artist:Someone
Version:Hard
Creator:mapper

[Difficulty]
CircleSize:7
OverallDifficulty:8

[Events]
0,0,"bg.jpg",0,0
Video,500,"bga.mp4"

[TimingPoints]
1000,500,4,1,0,100,1,0
3000,-50,4,1,0,100,0,0

[HitObjects]
36,192,1000,1,0,0:0:0:0:
475,192,2000,128,0,2500:0:0:0:0:kick.wav
256,192,3000,1,0,0:0:0:0:snare.wav
)";

TEST(osu_mania_7k) {
    BmsParseResult r = parse_osu_text(kOsu7);
    CHECK(r.ok);
    if (!r.ok) return;
    const Chart& c = r.chart;
    CHECK(c.meta.title == "曲");
    CHECK(c.meta.subtitle == "[Hard]");
    CHECK(c.meta.backbmp == "bg.jpg");
    CHECK_EQ(c.notes.size(), size_t(3));
    // 列 = floor(x × 7 / 512)。皿なし 7K はレーン 1〜7
    CHECK_EQ(int(c.notes[0].lane), 1);
    CHECK_EQ(int(c.notes[1].lane), 7);
    CHECK_EQ(int(c.notes[2].lane), 4);
    CHECK(c.notes[1].kind == NoteKind::Long && c.notes[1].end_us == 2500000);
    CHECK(c.wav[c.notes[1].sound] == "kick.wav");
    CHECK_EQ(int(c.notes[0].sound), 0);  // ファイル名の無いヒット音は鳴らさない
    // 音源は時刻 0 から
    CHECK(!c.bgm.empty() && c.bgm[0].time_us == 0 && c.wav[c.bgm[0].sound] == "audio.mp3");
    // BPM 120。3000ms から SV 2（beatLength -50）
    CHECK(std::abs(c.bpm_at(1500000) - 120.0) < 1e-9);
    double v1 = c.pos_at(2000000) - c.pos_at(1000000);  // 1 秒 = 2 拍
    double v2 = c.pos_at(4000000) - c.pos_at(3000000);  // SV 2 なので 4 拍
    CHECK(std::abs(v1 - 2.0) < 1e-6);
    CHECK(std::abs(v2 - 4.0) < 1e-6);
    CHECK(c.layout == KeyLayout::K7);
    CHECK(!c.bga.empty() && c.bmp[c.bga[0].image] == "bga.mp4");
}

TEST(osu_mania_8k_and_reject) {
    std::string s = kOsu7;
    s.replace(s.find("CircleSize:7"), 12, "CircleSize:8");
    BmsParseResult r = parse_osu_text(s);
    CHECK(r.ok);
    if (r.ok) {
        CHECK_EQ(int(r.chart.notes[0].lane), 0);  // 左端が皿
        CHECK(r.chart.layout == KeyLayout::K7S);
    }
    OsuParseOptions right;
    right.scratch_side = 1;
    BmsParseResult rr = parse_osu_text(s, right);
    CHECK(rr.ok && rr.chart.notes[1].lane == 0);  // 右端が皿
    std::string k4 = kOsu7;
    k4.replace(k4.find("CircleSize:7"), 12, "CircleSize:4");
    CHECK(!parse_osu_text(k4).ok);
    std::string std_mode = kOsu7;
    std_mode.replace(std_mode.find("Mode: 3"), 7, "Mode: 0");
    CHECK(!parse_osu_text(std_mode).ok);
}

TEST(osu_old_version_offset) {
    std::string s = kOsu7;
    s.replace(0, 21, "osu file format v4\n");
    BmsParseResult r = parse_osu_text(s);
    CHECK(r.ok && r.chart.notes[0].time_us == 1024000);
}

static const char* kBmson = R"({
  "version": "1.0.0",
  "info": {"title": "T", "artist": "A", "chart_name": "HYPER", "level": 7, "init_bpm": 120,
           "judge_rank": 50, "total": 200, "resolution": 240},
  "lines": [{"y": 0}, {"y": 960}],
  "bpm_events": [{"y": 960, "bpm": 240}],
  "stop_events": [{"y": 480, "duration": 240}],
  "sound_channels": [
    {"name": "stem.ogg", "notes": [
      {"x": 1, "y": 0, "l": 0, "c": false},
      {"x": 2, "y": 240, "l": 0, "c": true},
      {"x": 3, "y": 480, "l": 240, "c": false},
      {"x": 0, "y": 960, "l": 0, "c": true}]},
    {"name": "s.wav", "notes": [{"x": 8, "y": 960, "l": 0, "c": false}]}
  ]
})";

TEST(bmson_basic) {
    BmsParseResult r = parse_bmson_text(kBmson);
    CHECK(r.ok);
    if (!r.ok) return;
    const Chart& c = r.chart;
    CHECK(c.meta.title == "T" && c.meta.difficulty == 3 && c.meta.playlevel == 7);
    CHECK(std::abs(c.judge_scale - 0.5) < 1e-9);
    // 時刻：1 拍 = 500ms（BPM 120）。y=480 は 1000ms、そこで 1 拍止まる（500ms）
    CHECK_EQ(c.notes.size(), size_t(4));
    CHECK(c.notes[1].time_us == 500000);
    CHECK(c.notes[2].time_us == 1000000);                         // STOP と同じ位置は止まり始め
    CHECK(c.notes[2].kind == NoteKind::Long && c.notes[2].end_us == 2000000);  // 1 拍 + 停止 500ms
    CHECK(c.notes[3].lane == 0 && c.notes[3].time_us == 2500000);  // x=8 は皿。y=960 は 2 拍 + 停止
    CHECK(c.layout == KeyLayout::K7S);
    // 切り出し：y=0 は頭から、y=240 は続き（0.5 秒の位置から）。y=480 で頭に戻すので、y=240 の切れ端は最後まで
    const SoundSlice& a = c.wav_slice[c.notes[0].sound];
    const SoundSlice& b = c.wav_slice[c.notes[1].sound];
    const SoundSlice& d = c.wav_slice[c.notes[2].sound];
    CHECK(a.start_us == 0 && a.end_us == 500000);
    CHECK(b.start_us == 500000 && b.end_us == -1);
    CHECK(d.start_us == 0);
    // y=960 の BGM（続き）は、y=480 から 1500ms（停止を含む）の位置
    CHECK(!c.bgm.empty() && c.wav_slice[c.bgm[0].sound].start_us == 1500000);
    CHECK_EQ(c.bars.size(), size_t(2));
}

TEST(bmson_old_and_reject) {
    std::string old = R"({"info": {"title": "O", "initBPM": 150},
      "soundChannel": [{"name": "a.wav", "notes": [{"x": 1, "y": 240, "l": 0, "c": false}]}]})";
    BmsParseResult r = parse_bmson_text(old);
    CHECK(r.ok && r.chart.notes.size() == 1 && r.chart.notes[0].time_us == 400000);
    std::string pms = R"({"version": "1.0.0", "info": {"title": "P", "init_bpm": 120, "mode_hint": "popn-9k"},
      "sound_channels": [{"name": "a.wav", "notes": [{"x": 1, "y": 0, "l": 0, "c": false}]}]})";
    CHECK(!parse_bmson_text(pms).ok);
}

// LR2 のスキンのカスタマイズ（LR2files/SkinCustomize/<スキンのパスの MD5>.xml）を引き継ぐ
TEST(lr2_skin_customize) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "bmj_test_skincustom";
    fs::remove_all(root);
    fs::create_directories(root / "LR2files" / "Theme" / "T" / "parts");
    fs::create_directories(root / "LR2files" / "SkinCustomize");
    for (const char* n : {"a.png", "b.png", "readme.txt"}) std::ofstream(root / "LR2files" / "Theme" / "T" / "parts" / n) << "x";
    std::ofstream(root / "LR2files" / "Theme" / "T" / "play.lr2skin")
        << "#INFORMATION,0,T,me\n#CUSTOMOPTION,Judge,900,ON,OFF,AUTO\n#CUSTOMFILE,Parts,LR2files\\Theme\\T\\parts\\*.png,a\n"
           "#CUSTOMOPTION,Lane,910,A,B\n#ENDOFHEADER\n";
    std::ofstream(root / "LR2files" / "SkinCustomize" / (md5_hex(std::string("LR2files\\Theme\\T\\play.lr2skin")) + ".xml"))
        << "<?xml version=\"1.0\" encoding=\"shift_jis\"?>\n<skincustomize>\n<skinconfig>\n<config_0>902</config_0>\n"
           "<config_1>0</config_1>\n<config_2>999</config_2>\n</skinconfig>\n<customfile>\n<filename_0>ERROR</filename_0>\n"
           "<filename_1>b</filename_1>\n</customfile>\n</skincustomize>\n";
    AppConfig cfg;
    CHECK(import_lr2_skin_custom(root.string(), root.string(), cfg) == 1);
    CHECK(cfg.skin_custom.size() == 1);
    const std::vector<SkinCustomChoice>& sel = cfg.skin_custom.begin()->second;
    CHECK(sel.size() == 3);
    CHECK(sel[0].file.empty() && sel[0].index == 2);  // 902 - 900
    CHECK(sel[1].file == "b");                        // #CUSTOMFILE は名前で残す
    CHECK(sel[2].file.empty() && sel[2].index == 0);  // 範囲外の値は既定のまま
    fs::remove_all(root);
}

// LR2 のスキンの位置の調整（SkinCustomize の shift・size など）も引き継ぐ
TEST(lr2_skin_adjust) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "bmj_test_skinadjust";
    fs::remove_all(root);
    fs::create_directories(root / "LR2files" / "Theme" / "P");
    fs::create_directories(root / "LR2files" / "SkinCustomize");
    std::ofstream(root / "LR2files" / "Theme" / "P" / "play.lr2skin") << "#INFORMATION,0,P,me\n#ENDOFHEADER\n";
    std::ofstream(root / "LR2files" / "SkinCustomize" / (md5_hex(std::string("LR2files\\Theme\\P\\play.lr2skin")) + ".xml"))
        << "<?xml version=\"1.0\" encoding=\"shift_jis\"?>\n<skincustomize>\n<shift>\n<x>0</x>\n<y>0</y>\n</shift>\n"
           "<judge>\n<x>10</x>\n<y>-20</y>\n</judge>\n<rate>\n<x>100</x>\n<y>100</y>\n</rate>\n<size>\n<x>0</x>\n<y>10</y>\n</size>\n"
           "<dark>\n<type>0</type>\n</dark>\n<note_1p>\n<x>0</x>\n<y>0</y>\n</note_1p>\n</skincustomize>\n";
    AppConfig cfg;
    CHECK(import_lr2_skin_custom(root.string(), root.string(), cfg) == 1);
    CHECK(cfg.skin_adjust.size() == 1);
    if (!cfg.skin_adjust.empty()) {
        const SkinAdjust& a = cfg.skin_adjust.begin()->second;
        CHECK_EQ(a.size_y, 10);
        CHECK_EQ(a.judge_x, 10);
        CHECK_EQ(a.judge_y, -20);
        CHECK_EQ(a.rate_x, 100);
    }
    fs::remove_all(root);
}

// LR2 の素材を BMjoy のフォルダに写し、設定の中の素材の場所を写した先に直す
TEST(import_copy_lr2_assets) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "bmj_asset_test";
    fs::remove_all(root);
    fs::path lr2 = root / "LR2", other = root / "LR2b", dst = root / "BMjoy";
    auto put = [](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << "x";
    };
    put(lr2 / "LR2files/Theme/WMIX/select/select.lr2skin");
    put(lr2 / "LR2files/Sound/lr2.lr2ss");
    put(lr2 / "LR2files/Bgm/a/select.wav");
    put(lr2 / "LR2files/Database/song.db");
    put(other / "LR2files/CustomFolder/clear.lr2folder");
    AppConfig cfg;
    cfg.lr2_root = lr2.string();
    cfg.select_skin = (lr2 / "LR2files/Theme/WMIX/select/select.lr2skin").string();
    cfg.sound_set = (lr2 / "LR2files/Sound/lr2.lr2ss").string();
    cfg.skin_custom[cfg.select_skin] = {};
    cfg.custom_folder_dirs = {(other / "LR2files/CustomFolder").string()};
    AssetCopyResult r = copy_lr2_assets(cfg, dst.string());
    CHECK(r.ok);
    CHECK_EQ(r.files, 4);
    CHECK(fs::exists(dst / "LR2files/Theme/WMIX/select/select.lr2skin"));
    CHECK(fs::exists(dst / "LR2files/Bgm/a/select.wav"));
    CHECK(fs::exists(dst / "LR2files/CustomFolder/clear.lr2folder"));
    CHECK(!fs::exists(dst / "LR2files/Database"));  // 曲・記録のデータは写さない
    CHECK(fs::path(cfg.lr2_root) == dst);
    CHECK(fs::path(cfg.select_skin) == dst / "LR2files/Theme/WMIX/select/select.lr2skin");
    CHECK(fs::path(cfg.sound_set) == dst / "LR2files/Sound/lr2.lr2ss");
    CHECK(cfg.skin_custom.count(cfg.select_skin) == 1 && cfg.skin_custom.size() == 1);
    CHECK(fs::path(cfg.custom_folder_dirs[0]) == dst / "LR2files/CustomFolder");
    CHECK(fs::exists(lr2 / "LR2files/Database/song.db"));  // 元はそのまま
    // 2 回目は写すものが無い
    AppConfig again;
    again.lr2_root = lr2.string();
    CHECK_EQ(copy_lr2_assets(again, dst.string()).files, 0);
    fs::remove_all(root);
}
