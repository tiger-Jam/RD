// 譜面を借りる（仕様書10章「取得とキャッシュ（借りて返す）」）。自分の PC（母艦）が曲データベースの譜面を貸し、
// ほかの PC が持っていない譜面を、遊ぶときだけ取ってくる。
//   GET /bmjoy/v1/charts               貸せる譜面の一覧（1 行に「md5<TAB>sha256」）
//   GET /bmjoy/v1/chart/<md5|sha256>   その譜面のフォルダ（「id」「name」「chart」の行と、「file<TAB>大きさ<TAB>相対パス」の行）
//   GET /bmjoy/v1/file/<id>/<相対パス>  ファイル
// どれも X-BMjoy-Key の頭に合言葉が要る。返すのは曲フォルダの中の、譜面・音・画像・動画・テキストだけ
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db/song_db.h"
#include "net/http_server.h"

namespace bmj {

// 貸す側：曲データベースの譜面から作り、HTTP の問い合わせに答える
class BorrowLibrary {
public:
    void build(const std::vector<ChartRecord>& charts);
    HttpResponse handle(const HttpRequest& req, const std::string& key) const;
    size_t size() const { return paths_.size(); }

private:
    std::vector<std::string> paths_;                     // 譜面のファイル
    std::unordered_map<std::string, size_t> by_hash_;    // md5・sha256 → paths_ の番号
    std::unordered_map<std::string, std::string> folders_;  // フォルダの id → フォルダ
    std::string list_;                                   // /charts の本文
};

// 貸せるファイルか（拡張子：譜面・音・画像・動画・テキスト）
bool borrowable_file(const std::string& name);
// 譜面のファイルか
bool borrow_chart_file(const std::string& name);

struct BorrowSource {
    std::string url;  // 例 http://tiger-pc:7275
    std::string key;  // 合言葉
    bool ok() const { return !url.empty(); }
};

struct BorrowFolder {
    std::string id, name, chart;                         // フォルダの id・元のフォルダの名前・譜面の相対パス
    std::vector<std::pair<int64_t, std::string>> files;  // 大きさと相対パス
};

// 貸せる譜面の一覧（md5 と sha256）
bool borrow_list(const BorrowSource& src, std::vector<std::pair<std::string, std::string>>& charts, std::string& error);
// 譜面（md5 か sha256）のフォルダ
bool borrow_info(const BorrowSource& src, const std::string& hash, BorrowFolder& out, std::string& error);
// フォルダのファイルを dir に取る。whole が偽なら、ほかの譜面は取らない（借りる）。真なら全部（もらう）。
// dir に同じ大きさのファイルがあれば取らない。progress（取った大きさ・全部の大きさ）が false を返したらやめる
bool borrow_fetch(const BorrowSource& src, const BorrowFolder& f, const std::string& dir, bool whole,
                  const std::function<bool(int64_t, int64_t)>& progress, std::string& error);
// 借りた譜面の置き場（root の下のフォルダ）を、遊んでいない順に消して limit バイト以内にする（keep は消さない）
void borrow_trim(const std::string& root, int64_t limit, const std::string& keep);
// フォルダを使ったしるし（borrow_trim の順番に使う）
void borrow_touch(const std::string& dir);

}  // namespace bmj
