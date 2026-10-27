// HTTP の GET（難易度表の取得と、譜面を借りる通信）。Windows は WinHTTP、ほかは libcurl
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

}  // namespace bmj
