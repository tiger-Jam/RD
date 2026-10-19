#include "util/text.h"

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace bmj {

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string cp932_to_utf8(std::string_view s) {
    if (s.empty()) return {};
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), len, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::string(s);
    std::string out;
    out.resize(s.size() * 3 + 16);
    char* in = const_cast<char*>(s.data());
    size_t in_left = s.size();
    char* op = out.data();
    size_t out_left = out.size();
    while (in_left > 0) {
        size_t r = iconv(cd, &in, &in_left, &op, &out_left);
        if (r == static_cast<size_t>(-1)) {
            // 変換できないバイトは '?' にして1バイト進める
            if (out_left == 0) break;
            *op++ = '?';
            --out_left;
            ++in;
            --in_left;
        }
    }
    out.resize(out.size() - out_left);
    iconv_close(cd);
    return out;
#endif
}

std::string to_utf8_auto(std::string_view s) {
    if (is_valid_utf8(s)) return std::string(s);
    return cp932_to_utf8(s);
}

std::string utf8_to_cp932(std::string_view s) {
    if (s.empty()) return {};
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
    int len = WideCharToMultiByte(932, 0, w.data(), wlen, nullptr, 0, "?", nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(932, 0, w.data(), wlen, out.data(), len, "?", nullptr);
    return out;
#else
    iconv_t cd = iconv_open("CP932", "UTF-8");
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::string(s);
    std::string out;
    out.resize(s.size() * 2 + 16);
    char* in = const_cast<char*>(s.data());
    size_t in_left = s.size();
    char* op = out.data();
    size_t out_left = out.size();
    while (in_left > 0) {
        size_t r = iconv(cd, &in, &in_left, &op, &out_left);
        if (r == static_cast<size_t>(-1)) {
            // 変換できない文字は '?' にして、UTF-8 の 1 文字ぶん進める
            if (out_left == 0) break;
            *op++ = '?';
            --out_left;
            unsigned char c0 = static_cast<unsigned char>(*in);
            size_t n = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
            n = std::min(n, in_left);
            in += n, in_left -= n;
        }
    }
    iconv_close(cd);
    out.resize(out.size() - out_left);
    return out;
#endif
}

std::string to_upper_ascii(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace bmj
