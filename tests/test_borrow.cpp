#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "net/borrow.h"
#include "net/http_server.h"
#include "test.h"
#include "util/http.h"

using namespace rd;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

// 貸す側と借りる側の往復：一覧・フォルダ・ファイル。借りるときはほかの譜面を取らず、もらうときは全部取る
TEST(borrow_roundtrip) {
    const fs::path dir = fs::temp_directory_path() / "rd_test_borrow";
    fs::remove_all(dir);
    const fs::path song = dir / "songs" / "Song A";
    write(song / "a_n.bme", "#TITLE A\n");
    write(song / "a_h.bme", "#TITLE A2\n");
    write(song / "kick.wav", std::string(100000, 'k'));
    write(song / "snd" / "hat.ogg", "hat");
    write(song / "tool.exe", "nope");
    write(dir / "secret.txt", "secret");
    ChartRecord c;
    c.path = (song / "a_n.bme").string(), c.md5 = "AABB", c.sha256 = "ccdd";
    BorrowLibrary lib;
    lib.build({c});
    CHECK_EQ(lib.size(), size_t(1));
    HttpServer server;
    std::string err;
    CHECK(server.start(0, [&](const HttpRequest& r) { return lib.handle(r, "pass"); }, err));
    BorrowSource src{"http://127.0.0.1:" + std::to_string(server.port()), "pass"};

    std::vector<std::pair<std::string, std::string>> charts;
    CHECK(borrow_list(src, charts, err));
    CHECK(charts.size() == 1 && charts[0].first == "aabb" && charts[0].second == "ccdd");
    BorrowFolder f;
    CHECK(borrow_info(src, "AABB", f, err));
    CHECK(f.name == "Song A" && f.chart == "a_n.bme");
    CHECK_EQ(f.files.size(), size_t(4));  // 譜面 2 つと音 2 つ（exe は出さない）

    const fs::path cache = dir / "cache" / f.id;
    CHECK(borrow_fetch(src, f, cache.string(), false, nullptr, err));
    CHECK(read(cache / "a_n.bme") == "#TITLE A\n");
    CHECK(read(cache / "kick.wav").size() == 100000u);
    CHECK(read(cache / "snd" / "hat.ogg") == "hat");
    CHECK(!fs::exists(cache / "a_h.bme"));  // 借りるときは、ほかの譜面は取らない
    const fs::path keep = dir / "keep" / f.name;
    CHECK(borrow_fetch(src, f, keep.string(), true, nullptr, err));
    CHECK(fs::exists(keep / "a_h.bme"));  // もらうときは全部

    // 合言葉が違えば断る。フォルダの外や、貸さないファイルは返さない
    BorrowSource wrong{src.url, "nope"};
    CHECK(!borrow_list(wrong, charts, err));
    std::string body;
    const std::string base = src.url + "/rd/v1/file/" + f.id + "/";
    const HttpHeaders key = {{"X-RD-Key", "pass"}};
    CHECK(http_get(base + "kick.wav", body, err, key));
    CHECK(!http_get(base + "..%2Fsecret.txt", body, err, key));
    CHECK(!http_get(base + "../secret.txt", body, err, key));
    CHECK(!http_get(base + "tool.exe", body, err, key));
    CHECK(!http_get(src.url + "/rd/v1/chart/ffff", body, err, key));
    server.stop();
    fs::remove_all(dir);
}

// 置き場は、遊んでいない順に消して上限に収める。今使うものは消さない
TEST(borrow_trim_order) {
    const fs::path dir = fs::temp_directory_path() / "rd_test_borrow_trim";
    fs::remove_all(dir);
    for (const char* n : {"old", "mid", "new"}) {
        write(dir / n / "x.wav", std::string(1000, 'x'));
        borrow_touch((dir / n).string());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    borrow_touch((dir / "old").string());  // old を最後に使った
    borrow_trim(dir.string(), 2500, (dir / "mid").string());
    CHECK(fs::exists(dir / "old"));
    CHECK(fs::exists(dir / "mid"));  // 一番古いが、今使うので消さない
    CHECK(!fs::exists(dir / "new"));
    fs::remove_all(dir);
}
