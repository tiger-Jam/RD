// ログ。標準出力と標準エラーに書いたものを、行ごとに時刻を付けて BMjoyfiles/Log/<日時>_<名前>_<プロセス番号>.txt に残す
// （コンソールやパイプに出していれば、そちらにもそのまま出す）。Windows では落ちたときにダンプ（.dmp）も残す。
// 残したログは、次に起動したときに送る（設定の log_upload。app/service.h）
#pragma once

#include <string>

namespace bmj {

// 起動のはじめに 1 回呼ぶ。who はファイル名に入れる名前（body・launcher・serve）
void start_log(const char* who);
// 標準出力がパイプかファイルに向いているか（開発用の道具として動かしているとき）
bool output_is_redirected();
// ログの置き場（末尾に区切りが付く）
std::string log_dir();
// 今のプロセスのログのファイル名（置き場からの名前。start_log の前は空）
const std::string& current_log_name();
// ファイル名のプロセス番号のプロセスが、まだ動いているか（書き途中のログは送らない）
bool log_owner_alive(const std::string& file_name);

}  // namespace bmj
