// ファイル名の解決（仕様書12章）。大文字小文字を区別せず、拡張子の代替も扱う
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bmj {

// フォルダの中のファイルを、大文字小文字を区別せずに探す（LR2 は Windows のパスとして開くので区別しない）。
// 名前の区切りは '/' でも '\' でもよい。サブフォルダはどの深さでもよく、".." も使える。
// 読んだフォルダの一覧は覚えておく（1 つのインスタンスを複数のスレッドから同時に使わないこと）
class DirIndex {
public:
    explicit DirIndex(std::string dir);
    // name はディレクトリからの相対パス。見つからなければ拡張子を alt_exts（".ogg" など）に替えて探す。
    // それでも無ければ空文字列
    std::string resolve(const std::string& name, const std::vector<std::string>& alt_exts) const;
    // キー音（LR2: FindAltSound）。書かれた名前 → 拡張子を ogg・wav・flac・mp3 に替える
    // → 1 つ上のフォルダで書かれた名前 → wav・ogg・flac・mp3 に替える
    std::string find_sound(const std::string& name) const;
    // BGA の画像（LR2: FindAltImage）。書かれた名前 → png・jpg・bmp に替える → 1 つ上のフォルダで同じ手順
    std::string find_image(const std::string& name) const;

private:
    struct Listing {
        // 小文字にした名前 → 実際の名前
        std::unordered_map<std::string, std::string> files, dirs;
    };
    const Listing& listing(const std::string& dir) const;
    // name をそのまま（拡張子を替えずに）探す
    std::string find_exact(const std::string& name) const;
    // LR2 の拡張子の替え方。書かれた名前に ".ext" が含まれる拡張子は飛ばす
    std::string find_with_exts(const std::string& name, const std::vector<const char*>& exts) const;

    std::string dir_;
    mutable std::unordered_map<std::string, Listing> cache_;
};

std::string normalize_key(const std::string& rel);
std::string parent_dir(const std::string& path);
std::string join_path(const std::string& dir, const std::string& rel);

}  // namespace bmj
