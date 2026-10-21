// 難易度表（仕様書9章）。HTMLの bmstable メタタグ → header.json → data.json の一般的な形式を読む。
// 最後に取得できた版を手元に持ち続け、サーバが落ちていても、取得結果が空や壊れていても使い続ける
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

struct TableChart {
    std::string md5, sha256, level, title, artist, url, url_diff;
};

// 表のヘッダに書かれたコース（段位など）。charts は各ステージの譜面（md5 か sha256 のどちらか）
struct TableCourse {
    std::string name;
    std::vector<TableChart> charts;
    std::vector<std::string> constraints;  // grade / grade_mirror / gauge_lr2 / ln など
    int group = 0;  // ヘッダの course が並びの並びのとき、何番目の並びか（GENOCIDE 2018・NEW GENERATION などの組）
};

struct DifficultyTable {
    std::string url;  // 登録したURL
    std::string name, symbol;
    std::vector<std::string> levels;  // 表示の順
    std::vector<TableChart> charts;
    std::vector<TableCourse> courses;
    int64_t fetched = 0;  // 取得した時刻（UNIX秒）
    bool from_cache = false;
    std::string warning;  // 取得に失敗した・譜面が大きく減ったので更新しなかった、など
};

// header.json と data.json の中身から表を作る
bool parse_table(const std::string& header_json, const std::string& data_json, DifficultyTable& out, std::string& error);
// ネットワークから取得する（キャッシュは見ない）
bool fetch_table(const std::string& url, DifficultyTable& out, std::string& error);
// 取得を試み、駄目なら手元の版を使う。更新で譜面が半分未満に減ったら置き換えない。
// force でなければ、手元の版をその日（この機械の時刻の日付）のうちに取ってあれば、取りに行かずに手元の版を使う
DifficultyTable update_table(const std::string& cache_dir, const std::string& url, bool force = false);
// コースの組（ヘッダの course の並びの並びの 1 つ）の名前。コースの名前の共通の頭から付け、無ければ table_name
std::string course_group_label(const std::vector<const TableCourse*>& list, const std::string& table_name);
// t（UNIX 秒）が今日か（この機械の時刻の日付で比べる）
bool is_today(int64_t t);
// 手元の版だけを読む
bool load_cached_table(const std::string& cache_dir, const std::string& url, DifficultyTable& out);

// 相対URLを解決する（テスト用に公開）
std::string resolve_url(const std::string& base, const std::string& rel);
// HTMLから bmstable メタタグの content を取り出す
std::string find_bmstable_meta(const std::string& html);

}  // namespace bmj
