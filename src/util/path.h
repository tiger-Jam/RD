// ファイル名の解決（仕様書12章）。大文字小文字を区別せず、拡張子の代替も扱う
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bmj {

class DirIndex {
public:
    explicit DirIndex(std::string dir);
    // name はディレクトリからの相対パス。見つからなければ空文字列
    std::string resolve(const std::string& name, const std::vector<std::string>& alt_exts) const;

private:
    std::string dir_;
    // 正規化した相対パス（小文字・区切りは'/'）→実際の相対パス
    std::unordered_map<std::string, std::string> files_;
};

std::string normalize_key(const std::string& rel);
std::string parent_dir(const std::string& path);
std::string join_path(const std::string& dir, const std::string& rel);

}  // namespace bmj
