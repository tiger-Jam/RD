// 作者の受け口（Cloudflare の Workers。tools/service/cloudflare）とのやりとり：ログを送る・新しい版を確かめて入れ替える。
// 送り先と取りに行く先は service_url（設定で替えられる。既定は kServiceUrl）。Mac mini は受け口からログを取ってくるだけ
#pragma once

#include <functional>
#include <string>

namespace rd {

struct AppConfig;

// 受け口を作ったら（tools/service/cloudflare/setup.sh が出す住所）ここに入れる。空の間は送らず、更新も確かめない
inline constexpr const char* kServiceUrl = "https://rd-service.rd-service.workers.dev";

// この PC の印（RDfiles/Config/client_id.txt。無ければ作る）。ログの送り主を分けるため
std::string client_id();

// 残っているログ（書き終わったもの）を裏で送る。送ったものは Log/Sent へ移す。cfg.log_upload が偽なら何もしない
void upload_logs_async(const AppConfig& cfg);

struct UpdateInfo {
    bool available = false;  // 今の版より新しい版がある
    std::string version;     // 新しい版
    std::string url;         // 入れ替える zip
    std::string sha256;      // zip の SHA-256
    std::string notes;       // 何が変わったか
    std::string moved_to;    // 受け取り口の新しい住所（引っ越したとき。設定の service_url に覚える）
    std::string error;       // 確かめられなかったときの理由
};

// 新しい版があるか確かめる（開発中の版 "dev" は確かめない）
UpdateInfo check_update(const AppConfig& cfg);

// 新しい版の zip を落として、実行ファイルのフォルダに展開して入れ替える（Windows だけ）。
// 動いている実行ファイルと DLL は名前を変えてどけ（.old）、次に起動したときに消す。
// progress には「何をしているか」を渡す。成功したら true（呼んだ側は自分を起動し直す）
bool apply_update(const UpdateInfo& u, const std::function<void(const std::string&)>& progress, std::string& error);

// 前の更新でどけた古いファイル（.old）を消す。起動のはじめに呼ぶ
void remove_old_update_files();

// 2 つの版（"2026.10.30.1" の形）を比べる。a が新しければ正、同じなら 0
int compare_versions(const std::string& a, const std::string& b);

}  // namespace rd
