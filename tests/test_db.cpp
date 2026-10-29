#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "db/custom_folder.h"
#include "db/lr2_export.h"
#include "db/song_db.h"
#include "test.h"
#include "sqlite3.h"
#include "util/data_dir.h"
#include "util/hash.h"

using namespace rd;
namespace fs = std::filesystem;

TEST(hash_vectors) {
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string big(1000, 'a');
    CHECK(sha256_hex(big) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

TEST(song_db_scan_diff) {
    fs::path dir = fs::temp_directory_path() / "rd_test_songs";
    fs::remove_all(dir);
    write(dir / "a" / "a.bme", "#TITLE A\n#BPM 120\n#00111:01\n");
    write(dir / "b" / "b.bms", "#TITLE B\n#BPM 120\n#00111:0101\n");
    write(dir / "c" / "dp.bme", "#TITLE DP\n#BPM 120\n#00121:01\n");  // DP は持っている譜面として載せる（鍵盤数 10）
    std::string dbp = (dir / "songs.db").string();
    SongDb db;
    CHECK(db.open(dbp));
    ScanStats s1 = db.scan({dir.string()}, 2);
    CHECK_EQ(s1.found, 3);
    CHECK_EQ(s1.added, 3);
    CHECK_EQ(s1.failed, 0);
    CHECK_EQ(db.all_charts().size(), size_t(3));
    for (const ChartRecord& c : db.all_charts())
        if (c.title == "DP") CHECK_EQ(c.keys, 10);
    ScanStats s2 = db.scan({dir.string()}, 2);  // 変化なし
    CHECK_EQ(s2.added, 0);
    CHECK_EQ(s2.updated, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(dir / "a" / "a.bme", "#TITLE A2\n#BPM 120\n#00111:0101\n");
    fs::remove(dir / "b" / "b.bms");
    ScanStats s3 = db.scan({dir.string()}, 2);
    CHECK_EQ(s3.updated, 1);
    CHECK_EQ(s3.removed, 1);
    auto charts = db.all_charts();
    charts.erase(std::remove_if(charts.begin(), charts.end(), [](const ChartRecord& c) { return c.keys >= 10; }), charts.end());
    CHECK_EQ(charts.size(), size_t(1));
    if (!charts.empty()) {
        CHECK(charts[0].title == "A2");
        CHECK_EQ(charts[0].notes, 2);
        CHECK(charts[0].md5 == md5_hex("#TITLE A2\n#BPM 120\n#00111:0101\n"));
        ChartRecord r;
        CHECK(db.find_by_md5(charts[0].md5, r));
        // スコアは良いほうを残す
        db.save_score(r.sha256, 3, 100, 10, 50);
        db.save_score(r.sha256, 1, 80, 5, 60);
        ScoreRecord sc;
        CHECK(db.get_score(r.sha256, sc));
        CHECK_EQ(sc.clear, 3);
        CHECK_EQ(sc.exscore, 100);
        CHECK_EQ(sc.bp, 5);
        CHECK_EQ(sc.max_combo, 60);
        CHECK_EQ(sc.playcount, 2);
        // 判定の内訳は EX を更新したときだけ入れ替え、オプションの履歴は積み重ねる。プレイヤーの累計も数える
        PlayRecord p1;
        p1.clear = 4, p1.exscore = 120, p1.bp = 3, p1.max_combo = 70, p1.judge = {50, 20, 3, 1, 2}, p1.op_history = 0x2;
        db.save_score(r.sha256, p1);
        PlayRecord p2 = p1;
        p2.exscore = 90, p2.judge = {40, 10, 0, 0, 0}, p2.op_history = 0x100;
        db.save_score(r.sha256, p2);
        CHECK(db.get_score(r.sha256, sc));
        CHECK_EQ(sc.judge[0], 50);
        CHECK_EQ(sc.judge[4], 2);
        CHECK_EQ(static_cast<int>(sc.op_history), 0x102);
        PlayerStats ps = db.player_stats();
        CHECK_EQ(ps.playcount, 4);
        CHECK_EQ(ps.clear, 3);
        CHECK_EQ(ps.fail, 1);
        CHECK_EQ(static_cast<int>(ps.judge[0]), 90);
    }
    db.close();
    fs::remove_all(dir);
}

#include "sqlite3.h"

TEST(song_db_import_lr2) {
    fs::path dir = fs::temp_directory_path() / "rd_test_lr2";
    fs::remove_all(dir);
    std::string chart_text = "#TITLE A\n#BPM 120\n#00111:0101\n";
    write(dir / "songs" / "a.bme", chart_text);
    std::string md5 = md5_hex(chart_text);
    // LR2と同じ形の score 表を作る（LR2_songmanage.cpp:2696）
    std::string lr2 = (dir / "player.db").string();
    sqlite3* db = nullptr;
    sqlite3_open(lr2.c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE score(hash TEXT primary key,clear INTEGER,perfect INTEGER,great INTEGER,good INTEGER,"
                     "bad INTEGER,poor INTEGER,totalnotes INTEGER,maxcombo INTEGER,minbp INTEGER,playcount INTEGER,"
                     "clearcount INTEGER,failcount INTEGER,rank INTEGER,rate INTEGER,clear_db INTEGER,op_history INTEGER,"
                     "scorehash TEXT,ghost TEXT,clear_sd INTEGER,clear_ex INTEGER,op_best INTEGER, rseed INTEGER, complete INTEGER)",
                 nullptr, nullptr, nullptr);
    std::string ins = "INSERT INTO score(hash, clear, perfect, great, minbp, maxcombo, playcount, clearcount) VALUES('" +
                      md5 + "', 4, 1, 1, 3, 2, 10, 7), ('ffffffffffffffffffffffffffffffff', 2, 0, 0, 0, 0, 1, 1), "
                            "('0000000000200000000000000000519" + "0" + md5 + "', 3, 5, 5, 9, 9, 2, 1), "
                            // フルコンボで GOOD が 0（PGREAT＋GREAT＝最大コンボ）なら PERFECT。GOOD があればフルコンボのまま
                            "('eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee', 5, 3, 2, 1, 5, 1, 1), ('dddddddddddddddddddddddddddddddd', 5, 3, 1, 0, 5, 1, 1)";
    sqlite3_exec(db, ins.c_str(), nullptr, nullptr, nullptr);
    // プレイヤーの情報（ID・名前・段位・累計）
    sqlite3_exec(db, "CREATE TABLE player(id TEXT primary key,hash TEXT,name TEXT,irid INTEGER,playcount INTEGER,clear INTEGER,"
                     "fail INTEGER,perfect INTEGER,great INTEGER,good INTEGER,bad INTEGER,poor INTEGER,playtime INTEGER,grade_7 INTEGER);"
                     "INSERT INTO player VALUES('me','abcd','わたし',12345,100,60,40,1000,500,50,20,30,3600,9)",
                 nullptr, nullptr, nullptr);
    sqlite3_close(db);

    SongDb sdb;
    CHECK(sdb.open((dir / "songs.db").string()));
    sdb.scan({(dir / "songs").string()}, 1);
    CHECK_EQ(sdb.import_lr2_scores(lr2), 5);
    {
        sqlite3* rd = nullptr;
        sqlite3_open((dir / "songs.db").string().c_str(), &rd);
        auto lamp = [&](const char* md5v) {
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(rd, "SELECT clear FROM lr2_score WHERE md5 = ?", -1, &st, nullptr);
            sqlite3_bind_text(st, 1, md5v, -1, SQLITE_TRANSIENT);
            int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
            sqlite3_finalize(st);
            return v;
        };
        CHECK_EQ(lamp("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"), 6);
        CHECK_EQ(lamp("dddddddddddddddddddddddddddddddd"), 5);
        sqlite3_close(rd);
    }
    // 読んだ曲フォルダの下には譜面がある。まだ読んでいない曲フォルダには無い
    CHECK(sdb.has_charts_under((dir / "songs").string()));
    CHECK(!sdb.has_charts_under((dir / "other").string()));
    CHECK(!sdb.has_charts_under((dir / "song").string()));  // 名前の頭が同じだけの別のフォルダ
    auto charts = sdb.all_charts();
    CHECK_EQ(charts.size(), size_t(1));
    ScoreRecord sc;
    CHECK(sdb.get_score(charts[0].sha256, sc));
    CHECK_EQ(sc.clear, 4);
    CHECK_EQ(sc.exscore, 3);  // PG1×2 + GR1
    CHECK_EQ(sc.bp, 3);
    CHECK_EQ(sc.playcount, 10);
    // RDで遊ぶと合わせて表示する（ランプは高いほう、回数は足す）
    sdb.save_score(charts[0].sha256, 5, 4, 0, 2);
    CHECK(sdb.get_score(charts[0].sha256, sc));
    CHECK_EQ(sc.clear, 5);
    CHECK_EQ(sc.exscore, 4);
    CHECK_EQ(sc.bp, 0);
    CHECK_EQ(sc.playcount, 11);
    CHECK_EQ(sdb.all_scores().size(), size_t(2));  // 手元に無い譜面の分は表示には出ない（コースは出る）
    // コースの記録は、作者の欄を除いた RD のコースのハッシュで引ける
    CHECK(sdb.get_score("00000000002" + std::string(21, '0') + md5, sc));
    CHECK_EQ(sc.clear, 3);
    CHECK_EQ(sc.exscore, 15);
    // プレイヤーの情報を引き継ぎ、累計は LR2 の分に RD で遊んだ分（上の 1 回）を足す
    CHECK(sdb.lr2_player_info()["name"] == "わたし" && sdb.lr2_player_info()["grade_7"] == "9");
    CHECK_EQ(sdb.player_stats().playcount, 101);
    CHECK_EQ(sdb.own_player_stats().playcount, 1);
    // LR2 の形で書き出すと、ID・段位・プレイ時間はそのまま、コースは LR2 の元の鍵になる
    std::string err;
    CHECK(export_lr2_files(sdb, (dir / "out").string(), {(dir / "songs").string()}, "", false, &err));
    {
        sqlite3* rd = nullptr;
        sqlite3_open((dir / "out" / "Score" / "me.db").string().c_str(), &rd);
        auto one = [&](const char* sql) {
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(rd, sql, -1, &st, nullptr);
            std::string v = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)
                                ? reinterpret_cast<const char*>(sqlite3_column_text(st, 0)) : "";
            sqlite3_finalize(st);
            return v;
        };
        CHECK(one("SELECT id || ',' || name || ',' || irid || ',' || grade_7 || ',' || playtime || ',' || playcount FROM player") ==
              "me,わたし,12345,9,3600,101");
        CHECK(one("SELECT typeof(grade_7) FROM player") == "integer");
        CHECK(one(("SELECT clear FROM score WHERE hash = '0000000000200000000000000000519" + std::string("0") + md5 + "'").c_str()) == "3");
        sqlite3_close(rd);
    }
    sdb.close();
    fs::remove_all(dir);
}

TEST(song_db_ln_mode_and_course_scores) {
    fs::path dir = fs::temp_directory_path() / "rd_test_lnmode";
    fs::remove_all(dir);
    write(dir / "ln" / "ln.bms", "#TITLE LN\n#BPM 120\n#00151:0101\n#00112:01\n");
    write(dir / "fixed" / "cn.bms", "#TITLE CN\n#LNMODE 2\n#BPM 120\n#00151:0101\n");
    SongDb db;
    CHECK(db.open((dir / "songs.db").string()));
    db.scan({dir.string()}, 2);
    auto charts = db.all_charts();
    CHECK_EQ(charts.size(), size_t(2));
    const ChartRecord* ln = nullptr;
    const ChartRecord* cn = nullptr;
    for (const ChartRecord& c : charts) (c.title == "LN" ? ln : cn) = &c;
    if (!ln || !cn) return;
    CHECK(ln->ln_free == 1 && ln->notes == 2);
    CHECK(cn->ln_free == 0 && cn->notes == 2);  // #LNMODE 2 の LN は 1 本 = 2
    // 種類を決めていない LN のある譜面は、LN の種類ごとに別の記録
    PlayRecord a;
    a.clear = 3, a.exscore = 4, a.ln_mode = 1;
    db.save_score(ln->sha256, a);
    ScoreRecord r;
    CHECK(db.get_score(ln->sha256, r, 1) && r.exscore == 4);
    CHECK(!db.get_score(ln->sha256, r, 0));
    bool found = false;
    for (const ScoreRecord& s : db.all_scores(1)) found = found || s.sha256 == ln->sha256;
    CHECK(found);
    // コースの記録（曲データベースに無いキー）も読める
    PlayRecord c;
    c.clear = 4, c.exscore = 100;
    db.save_score("course-hash", c);
    CHECK(db.get_score("course-hash", r) && r.clear == 4);
    found = false;
    for (const ScoreRecord& s : db.all_scores(0)) found = found || s.sha256 == "course-hash";
    CHECK(found);
}

TEST(song_db_import_lr2_songs) {
    fs::path dir = fs::temp_directory_path() / "rd_test_lr2songs";
    fs::remove_all(dir);
    std::string seven = "#TITLE S\n#BPM 120\n#00111:01\n";
    write(dir / "songs" / "s" / "s.bms", seven);
    write(dir / "songs" / "d" / "dp.bme", "#TITLE D\n#BPM 120\n#00121:01\n");
    std::string lr2 = (dir / "song.db").string();
    sqlite3* db = nullptr;
    sqlite3_open(lr2.c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE song(hash TEXT ,title TEXT ,subtitle TEXT ,genre TEXT,artist TEXT,subartist TEXT,tag TEXT ,"
                     "path TEXT primary key ,type INTEGER,folder TEXT,stagefile TEXT,banner TEXT,backbmp TEXT,parent TEXT,"
                     "level INTEGER,difficulty INTEGER,maxbpm INTEGER,minbpm INTEGER,mode INTEGER,judge INTEGER,longnote INTEGER,"
                     "bga INTEGER,random INTEGER,date INTEGER,favorite INTEGER,txt INTEGER,karinotes INTEGER,adddate INTEGER,exlevel INTEGER)",
                 nullptr, nullptr, nullptr);
    std::string s_path = (dir / "songs" / "s" / "s.bms").string(), d_path = (dir / "songs" / "d" / "dp.bme").string();
    std::string ins = "INSERT INTO song(hash,title,path,level,difficulty,maxbpm,minbpm,mode,judge,longnote,karinotes) VALUES('" +
                      md5_hex(seven) + "','S（LR2）','" + s_path + "',5,2,120,120,7,2,0,1),('x','D','" + d_path + "',3,2,120,120,14,2,0,1)";
    sqlite3_exec(db, ins.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);

    SongDb sdb;
    CHECK(sdb.open((dir / "songs.db").string()));
    CHECK_EQ(sdb.import_lr2_songs(lr2), 1);  // 7 鍵だけ
    auto charts = sdb.all_charts();
    CHECK(charts.size() == 1 && charts[0].sha256 == "md5:" + md5_hex(seven) && charts[0].title == "S（LR2）");
    CHECK_EQ(sdb.provisional_count(), 1);
    // 曲フォルダを読み直しても、取り込んだ譜面と 7 鍵以外の譜面は読まない
    ScanStats st = sdb.scan({(dir / "songs").string()}, 2);
    CHECK_EQ(st.added + st.updated, 0);
    // 裏で読むと、本物の情報に置き換わる
    CHECK_EQ(sdb.fill_provisional(10, 2), 1);
    charts = sdb.all_charts();
    CHECK(charts.size() == 1 && charts[0].sha256 == sha256_hex(seven) && charts[0].title == "S");
    CHECK_EQ(sdb.provisional_count(), 0);
}

// 選曲の F8：そのフォルダの下だけを読み直し、ほかのフォルダの変化には触らない。F3 の書き換えも残る
TEST(song_db_scan_only_under) {
    fs::path dir = fs::temp_directory_path() / "rd_test_songs_f8";
    fs::remove_all(dir);
    write(dir / "x" / "a" / "a.bme", "#TITLE A\n#BPM 120\n#00111:01\n");
    write(dir / "y" / "b" / "b.bme", "#TITLE B\n#BPM 120\n#00111:01\n");
    SongDb db;
    CHECK(db.open((dir / "songs.db").string()));
    CHECK_EQ(db.scan({dir.string()}, 2).added, 2);
    write(dir / "x" / "a" / "a2.bme", "#TITLE A2\n#BPM 120\n#00111:01\n");
    fs::remove(dir / "y" / "b" / "b.bme");
    ScanStats s = db.scan({dir.string()}, 2, nullptr, (dir / "x").string());
    CHECK_EQ(s.added, 1);
    CHECK_EQ(s.removed, 0);  // y の下は見ない
    CHECK_EQ(db.all_charts().size(), size_t(3));
    db.set_chart_tag((dir / "x" / "a" / "a.bme").string(), 5, 12);
    bool tagged = false;
    for (const ChartRecord& c : db.all_charts())
        if (c.title == "A") tagged = c.difficulty == 5 && c.playlevel == 12;
    CHECK(tagged);
    CHECK_EQ(db.scan({dir.string()}, 2).removed, 1);
    db.close();
    fs::remove_all(dir);
}

// 皿の自動演奏や SCATTER などのプレイは、ランプ・回数・オプション履歴だけを残す（LR2: statplay.cpp:895）
TEST(song_db_lamp_only) {
    fs::path dir = fs::temp_directory_path() / "rd_test_lamp_only";
    fs::remove_all(dir);
    fs::create_directories(dir);
    SongDb db;
    CHECK(db.open((dir / "songs.db").string()));
    PlayRecord a;
    a.lamp_only = true, a.clear = 2, a.exscore = 500, a.bp = 3, a.max_combo = 100, a.op_history = 0x1000000;
    a.judge = {200, 100, 0, 0, 3};
    db.save_score("h", a);
    ScoreRecord r;
    CHECK(db.get_score("h", r));
    CHECK_EQ(r.clear, 2);
    CHECK_EQ(r.exscore, 0);
    CHECK_EQ(r.bp, -1);
    CHECK_EQ(r.max_combo, 0);
    CHECK_EQ(r.playcount, 1);
    CHECK(r.op_history == 0x1000000u);
    PlayRecord b;
    b.clear = 3, b.exscore = 300, b.bp = 10, b.max_combo = 50;
    db.save_score("h", b);
    a.clear = 2, a.exscore = 900;
    db.save_score("h", a);
    CHECK(db.get_score("h", r));
    CHECK_EQ(r.clear, 3);
    CHECK_EQ(r.exscore, 300);
    CHECK_EQ(r.bp, 10);
    CHECK_EQ(r.playcount, 3);
    db.close();
    fs::remove_all(dir);
}

// LR2 のカスタムフォルダの #COMMAND を、LR2 と同じ形の表で動かす（LR2: songmanage:725）
TEST(custom_folder_query) {
    std::vector<ChartRecord> charts(4);
    for (int i = 0; i < 4; ++i) {
        charts[i].md5 = "m" + std::to_string(i);
        charts[i].sha256 = "s" + std::to_string(i);
        charts[i].title = "T" + std::to_string(i);
        charts[i].playlevel = 3 + i * 3;  // 3, 6, 9, 12
        charts[i].keys = i == 3 ? 5 : 7;
        charts[i].notes = 100;
    }
    std::unordered_map<std::string, ScoreRecord> scores;
    scores["s0"].clear = 6;  // PERFECT は LR2 の FULL COMBO（5）として見える
    scores["s0"].playcount = 4;
    scores["s1"].clear = 3;
    scores["s1"].playcount = 9;
    std::unordered_map<std::string, int> favs{{"m2", 2}};
    CustomFolderQuery q;
    CHECK(q.build(charts, scores, favs));
    CustomFolderDef d;
    d.command = "clear = 5";
    CHECK(q.run(d) == std::vector<std::string>{"m0"});
    d.command = "playcount != 0 ORDER BY playcount DESC";
    CHECK(q.run(d) == (std::vector<std::string>{"m1", "m0"}));
    d.command = "level >= 5";  // IGNORE（m2）は出さない
    CHECK(q.run(d) == (std::vector<std::string>{"m1", "m3"}));
    d.command = "mode = 5";
    CHECK(q.run(d) == std::vector<std::string>{"m3"});
    d.command = "favorite = 2";  // IGNORE を集めるフォルダには出す
    CHECK(q.run(d) == std::vector<std::string>{"m2"});
    d.command = "level >= 0";
    d.max_tracks = 1;  // 1 曲を無作為に
    CHECK(q.run(d).size() == 1);
    std::string err;
    d.command = "no_such_column = 1";
    d.max_tracks = 0;
    CHECK(q.run(d, &err).empty() && !err.empty());
    // 結果は覚えておき（裏のスレッドで先に集めた分を画面の処理が読む）、記録を書き換えたら捨てる
    std::vector<std::string> got;
    d.command = "clear = 5";
    CHECK(q.cached(d, got) && got == std::vector<std::string>{"m0"});
    d.command = "level >= 0";
    d.max_tracks = 1;
    CHECK(!q.cached(d, got));  // 1 曲を無作為に選ぶフォルダは覚えない
    d.command = "clear = 5";
    d.max_tracks = 0;
    ScoreRecord r;
    r.clear = 5;
    q.update_score(charts[1], r);
    CHECK(!q.cached(d, got));
    CHECK(q.run(d).size() == 2);
}

// 保存したランダムの配置：同じものは 1 つ、新しい順、譜面ごとに使うのは 1 つまで、消せる
TEST(song_db_saved_random) {
    fs::path dir = fs::temp_directory_path() / "rd_test_saved_random";
    fs::remove_all(dir);
    fs::create_directories(dir);
    SongDb db;
    CHECK(db.open((dir / "songs.db").string()));
    SavedRandom a;
    a.sha256 = "aaa", a.random = 2, a.seed = 1234, a.version = 1, a.saved_at = 100;
    const int64_t ia = db.save_random(a);
    CHECK(ia > 0);
    CHECK_EQ(db.save_random(a), ia);  // 同じ配置は増やさない
    SavedRandom b = a;
    b.seed = 99, b.random = 3, b.saved_at = 200;
    const int64_t ib = db.save_random(b);
    SavedRandom other = a;
    other.sha256 = "bbb";
    db.save_random(other);
    std::vector<SavedRandom> list = db.saved_randoms("aaa");
    CHECK_EQ(list.size(), size_t(2));
    CHECK(list[0].id == ib && list[0].seed == 99u && list[0].random == 3);  // 新しい順
    SavedRandom used;
    CHECK(!db.used_random("aaa", used));
    db.use_saved_random("aaa", ia);
    db.use_saved_random("aaa", ib);  // 使うのは 1 つだけ
    CHECK(db.used_random("aaa", used) && used.id == ib);
    CHECK(!db.used_random("bbb", used));
    db.use_saved_random("aaa", 0);
    CHECK(!db.used_random("aaa", used));
    db.delete_saved_random(ia);
    CHECK_EQ(db.saved_randoms("aaa").size(), size_t(1));
    db.close();
    fs::remove_all(dir);
}

// LR2 の形の書き出し：フォルダの番号は LR2 が作ったものと同じ（値は LR2 の song.db にあったもの）。文字は UTF-8
TEST(lr2_export) {
    CHECK(lr2_folder_crc("E:\\BMSFolder\\torrent\\") == "bb0358c0");
    CHECK(lr2_folder_crc("E:/BMSFolder/torrent") == "bb0358c0");  // 区切りと末尾の違いは同じに
    CHECK(lr2_folder_crc("E:\\BMSFolder\\torrent\\[SHIKI] Crystal palace\\") == "15ae94f");
    CHECK(lr2_folder_crc("E:\\BMSFolder\\Music\\0-9\\1024 (by 風鈴奏)\\") == "4297e3f3");  // 日本語を含むパス
    fs::path dir = fs::temp_directory_path() / "rd_lr2_export";
    fs::remove_all(dir);
    std::vector<ChartRecord> charts(2);
    charts[0].path = "E:/BMSFolder/torrent/[SHIKI] Crystal palace/a.bme", charts[0].folder = "E:/BMSFolder/torrent/[SHIKI] Crystal palace";
    charts[0].md5 = "0123456789abcdef0123456789abcdef", charts[0].sha256 = "s1", charts[0].title = "曲名", charts[0].notes = 1000;
    charts[1].path = "E:/BMSFolder/torrent/b/c/x.bms", charts[1].folder = "E:/BMSFolder/torrent/b/c";
    charts[1].md5 = "fedcba9876543210fedcba9876543210", charts[1].sha256 = "s2", charts[1].title = "x";
    std::string err;
    CHECK(export_lr2_song_db((dir / "song.db").string(), charts, {"E:/BMSFolder/torrent"}, &err));
    ScoreRecord sc;
    sc.sha256 = "s1", sc.clear = 6, sc.judge = {800, 150, 30, 10, 10}, sc.playcount = 5, sc.clearcount = 2, sc.bp = 20;
    PlayerStats ps;
    ps.playcount = 5;
    CHECK(export_lr2_scores((dir / "Score" / "me.db").string(), "me", charts, {sc}, ps, {}, &err));
    CHECK(err.empty());
    sqlite3* db = nullptr;
    sqlite3_open((dir / "song.db").string().c_str(), &db);
    auto one = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        std::string v;
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
            v.assign(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), static_cast<size_t>(sqlite3_column_bytes(st, 0)));
        sqlite3_finalize(st);
        return v;
    };
    CHECK(one("SELECT folder || '/' || parent FROM song WHERE hash = '0123456789abcdef0123456789abcdef'") == "15ae94f/bb0358c0");
    CHECK(one("SELECT title FROM song WHERE hash = '0123456789abcdef0123456789abcdef'") == "曲名");
    CHECK(one("SELECT path FROM song WHERE title = 'x'") == "E:\\BMSFolder\\torrent\\b\\c\\x.bms");
    // フォルダ：登録した曲フォルダ（親は一番上の印）、譜面のフォルダ、途中のフォルダ b
    CHECK(one("SELECT count(*) FROM folder") == "4");
    CHECK(one("SELECT parent FROM folder WHERE path = 'E:\\BMSFolder\\torrent\\'") == kLr2RootParent);
    CHECK(one("SELECT parent FROM folder WHERE title = 'c'") == lr2_folder_crc("E:/BMSFolder/torrent/b"));
    sqlite3_close(db);
    sqlite3_open((dir / "Score" / "me.db").string().c_str(), &db);
    // EX 1750 / 2000 = 87.5%：rate 87、rank 7（AA）。PERFECT は FULL COMBO（5）にする
    CHECK(one("SELECT clear || ',' || rate || ',' || rank || ',' || failcount || ',' || totalnotes FROM score") == "5,87,7,3,1000");
    CHECK(one("SELECT name || ',' || playcount FROM player") == "me,5");
    sqlite3_close(db);
    fs::remove_all(dir);
}

// プレイヤーを分ける：前の songs.db の記録は、最初に開いたプレイヤーの score.db へ移る。別のプレイヤーの記録は別
TEST(song_db_players) {
    fs::path dir = fs::temp_directory_path() / "rd_test_players";
    fs::remove_all(dir);
    write(dir / "songs" / "a.bme", "#TITLE A\n#BPM 120\n#00111:0101\n");
    const std::string songs = (dir / "songs.db").string();
    {
        SongDb db;  // プレイヤーを分けていない（前の形）
        CHECK(db.open(songs));
        db.scan({(dir / "songs").string()}, 1);
        db.save_score(db.all_charts()[0].sha256, 4, 3, 1, 2);
    }
    const std::string sha = [&] {
        SongDb db;
        db.open(songs);
        return db.all_charts()[0].sha256;
    }();
    set_player_id("rd_test_player_a");
    fs::remove_all(fs::u8path(player_dir()));
    {
        SongDb db;
        CHECK(db.open(songs));
        ScoreRecord r;
        CHECK(db.get_score(sha, r) && r.clear == 4);  // 移った
        CHECK(fs::exists(fs::u8path(player_dir() + "score.db")));
        CHECK(fs::exists(songs + ".before-players"));
        CHECK_EQ(db.own_player_stats().playcount, 1);
    }
    const std::string dir_a = player_dir();
    set_player_id("rd_test_player_b");
    fs::remove_all(fs::u8path(player_dir()));
    {
        SongDb db;
        CHECK(db.open(songs));
        ScoreRecord r;
        CHECK(!db.get_score(sha, r) || r.clear == 0);  // 別のプレイヤーには記録が無い
        db.save_score(sha, 2, 1, 5, 1);
        CHECK(db.get_score(sha, r) && r.clear == 2);
        CHECK_EQ(db.all_charts().size(), size_t(1));  // 譜面は共通
    }
    set_player_id("rd_test_player_a");
    {
        SongDb db;
        CHECK(db.open(songs));
        ScoreRecord r;
        CHECK(db.get_score(sha, r) && r.clear == 4);  // A の記録は A のまま
    }
    fs::remove_all(fs::u8path(dir_a));
    set_player_id("rd_test_player_b");
    fs::remove_all(fs::u8path(player_dir()));
    set_player_id("");
    fs::remove_all(dir);
}

// 譜面の傾向：曲フォルダを読むと分析し、書き出したものを別の曲データベースに入れられる（手元の結果は置き換えない）
TEST(song_db_chart_attr_bundle) {
    fs::path dir = fs::temp_directory_path() / "rd_test_attr";
    fs::remove_all(dir);
    std::string bms = "#BPM 160\n";
    static const char* ch[] = {"11", "13", "15", "19", "12", "14", "18"};
    int k = 0;
    for (int bar = 1; bar <= 24; ++bar) {
        std::string lanes[7];
        for (std::string& l : lanes) l = std::string(32, '0');
        for (int s = 0; s < 16; ++s, ++k) lanes[k % 7].replace(s * 2, 2, "01");
        char head[16];
        for (int c = 0; c < 7; ++c) std::snprintf(head, sizeof(head), "#%03d%s:", bar, ch[c]), bms += head + lanes[c] + "\n";
    }
    write(dir / "songs" / "randa.bme", bms);
    std::string sha;
    {
        SongDb db;
        CHECK(db.open((dir / "a.db").string()));
        db.scan({(dir / "songs").string()}, 1);
        sha = db.all_charts()[0].sha256;
        auto attrs = db.chart_attrs();
        CHECK(attrs.count(sha) && attrs[sha].attr == kAttrRanda);
        CHECK_EQ(db.fill_chart_attrs(100, 1), 0);  // 読むときに分析してあるので、残りは無い
        CHECK_EQ(db.export_chart_attrs((dir / "chart_attr.tsv").string()), 1);
    }
    {
        SongDb db;
        CHECK(db.open((dir / "b.db").string()));
        CHECK_EQ(db.import_chart_attrs_if_changed((dir / "chart_attr.tsv").string(), "t"), 1);
        CHECK_EQ(db.import_chart_attrs_if_changed((dir / "chart_attr.tsv").string(), "t"), 0);  // 変わっていなければ入れ直さない
        auto attrs = db.chart_attrs();
        CHECK(attrs.count(sha) && attrs[sha].attr == kAttrRanda && attrs[sha].sub == kSubRandaMid && attrs[sha].grid_bpm > 150);
    }
    fs::remove_all(dir);
}
