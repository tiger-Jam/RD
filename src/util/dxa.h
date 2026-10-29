// DXライブラリのアーカイブ（.dxa）の読み込み。LR2スキンのフォントなどが格納されている
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rd {

class DxaArchive {
public:
    // 鍵文字列なし（DXライブラリの既定の鍵）で開く。Ver1〜7に対応
    bool open(const std::string& path, std::string* error = nullptr);
    // rel はアーカイブ内のパス。区切りは '/' か '\\'、大文字小文字は区別しない
    bool read(const std::string& rel, std::vector<uint8_t>& out) const;
    bool exists(const std::string& rel) const;
    // 全ファイルのパス（UTF-8、区切りは'/'）
    std::vector<std::string> list() const;
    int version() const { return version_; }

    struct Entry {
        std::string path;                  // 元の名前（UTF-8）
        uint64_t offset = 0;               // ファイル先頭からの位置
        uint64_t size = 0;                 // 展開後のサイズ
        uint64_t packed = 0;               // 圧縮後のサイズ（compressed のときのみ）
        bool compressed = false;
        uint64_t key_pos = 0;              // 鍵のXORを始める位置
        std::array<uint8_t, 32> key2{};    // Ver7以降のファイルごとの鍵
    };

private:
    std::string path_;
    int version_ = 0;
    std::array<uint8_t, 12> key_{};
    std::unordered_map<std::string, Entry> files_;  // 正規化したパス→ファイル情報
};

// path の実ファイルが無ければ、途中のディレクトリを「同名.dxa」とみなしてアーカイブ内から読む
// 例: .../font/title/font.lr2font → .../font/title.dxa の font.lr2font
bool read_file_or_dxa(const std::string& path, std::vector<uint8_t>& out);

}  // namespace rd
