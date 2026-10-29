// HTTP の GET と POST（難易度表の取得・譜面を借りる通信・更新の確かめ・ログを送る）。Windows は WinHTTP、ほかは libcurl
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bmj {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// 本文を全部受け取る。400 以上は失敗
bool http_get(const std::string& url, std::string& body, std::string& error, const HttpHeaders& headers = {});
// 本文を受け取ったそばから sink に渡す（大きなファイル向け）。sink が false を返したらやめて失敗にする
bool http_get_stream(const std::string& url, const std::function<bool(const char*, size_t)>& sink, std::string& error,
                     const HttpHeaders& headers = {});
// data を POST で送り、返事の本文を reply に入れる。400 以上は失敗
bool http_post(const std::string& url, const std::string& data, std::string& reply, std::string& error,
               const HttpHeaders& headers = {});

}  // namespace bmj
