// 曲フォルダの監視（BMjoy の拡張。LR2 は起動時か F8 でしか読み直さない）。
// 曲フォルダの下でファイルが増えた・変わった・消えたら、そのフォルダを覚えておき、変化が落ち着いてから渡す。
// Windows は ReadDirectoryChangesW、macOS は FSEvents。ほかの OS では何もしない
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bmj {

class FolderWatcher {
public:
    FolderWatcher();
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    // roots の下（全部の階層）を見始める。もう一度呼ぶと見る場所を替える
    void start(const std::vector<std::string>& roots);
    void stop();
    // 変化があったフォルダ（譜面の入っているフォルダ。消えたフォルダも含む）を取り出す。
    // 最後の変化から quiet だけ何も起きていなければ渡し、まだ続いていれば空を返す（コピーの途中で読まないように）
    std::vector<std::string> take_changed(std::chrono::milliseconds quiet = std::chrono::milliseconds(3000));

    // 監視の仕組みから呼ぶ。path は変わったファイルかフォルダ
    void notify(const std::string& path, bool is_dir);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mu_;
    std::set<std::string> changed_;
    std::chrono::steady_clock::time_point last_{};
};

}  // namespace bmj
