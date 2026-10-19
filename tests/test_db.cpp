#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "db/song_db.h"
#include "test.h"
#include "util/hash.h"

using namespace bmj;
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
    fs::path dir = fs::temp_directory_path() / "bmj_test_songs";
    fs::remove_all(dir);
    write(dir / "a" / "a.bme", "#TITLE A\n#BPM 120\n#00111:01\n");
    write(dir / "b" / "b.bms", "#TITLE B\n#BPM 120\n#00111:0101\n");
    write(dir / "c" / "dp.bme", "#TITLE DP\n#BPM 120\n#00121:01\n");  // DPは一覧に出さない
    std::string dbp = (dir / "songs.db").string();
    SongDb db;
    CHECK(db.open(dbp));
    ScanStats s1 = db.scan({dir.string()}, 2);
    CHECK_EQ(s1.found, 3);
    CHECK_EQ(s1.added, 3);
    CHECK_EQ(s1.failed, 1);
    CHECK_EQ(db.all_charts().size(), size_t(2));
    ScanStats s2 = db.scan({dir.string()}, 2);  // 変化なし（読めなかったDPも読み直さない）
    CHECK_EQ(s2.added, 0);
    CHECK_EQ(s2.updated, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(dir / "a" / "a.bme", "#TITLE A2\n#BPM 120\n#00111:0101\n");
    fs::remove(dir / "b" / "b.bms");
    ScanStats s3 = db.scan({dir.string()}, 2);
    CHECK_EQ(s3.updated, 1);
    CHECK_EQ(s3.removed, 1);
    auto charts = db.all_charts();
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
    fs::path dir = fs::temp_directory_path() / "bmj_test_lr2";
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
                      md5 + "', 4, 1, 1, 3, 2, 10, 7), ('ffffffffffffffffffffffffffffffff', 2, 0, 0, 0, 0, 1, 1)";
    sqlite3_exec(db, ins.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);

    SongDb sdb;
    CHECK(sdb.open((dir / "songs.db").string()));
    sdb.scan({(dir / "songs").string()}, 1);
    CHECK_EQ(sdb.import_lr2_scores(lr2), 2);
    auto charts = sdb.all_charts();
    CHECK_EQ(charts.size(), size_t(1));
    ScoreRecord sc;
    CHECK(sdb.get_score(charts[0].sha256, sc));
    CHECK_EQ(sc.clear, 4);
    CHECK_EQ(sc.exscore, 3);  // PG1×2 + GR1
    CHECK_EQ(sc.bp, 3);
    CHECK_EQ(sc.playcount, 10);
    // BMjoyで遊ぶと合わせて表示する（ランプは高いほう、回数は足す）
    sdb.save_score(charts[0].sha256, 5, 4, 0, 2);
    CHECK(sdb.get_score(charts[0].sha256, sc));
    CHECK_EQ(sc.clear, 5);
    CHECK_EQ(sc.exscore, 4);
    CHECK_EQ(sc.bp, 0);
    CHECK_EQ(sc.playcount, 11);
    CHECK_EQ(sdb.all_scores().size(), size_t(1));  // 手元に無い譜面の分は表示には出ない
    sdb.close();
    fs::remove_all(dir);
}

TEST(song_db_ln_mode_and_course_scores) {
    fs::path dir = fs::temp_directory_path() / "bmj_test_lnmode";
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
    fs::path dir = fs::temp_directory_path() / "bmj_test_lr2songs";
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
    fs::path dir = fs::temp_directory_path() / "bmj_test_songs_f8";
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
