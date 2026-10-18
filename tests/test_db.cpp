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
    }
    db.close();
    fs::remove_all(dir);
}
