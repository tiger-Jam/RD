#include "util/dxa.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_set>

#include "util/text.h"

namespace fs = std::filesystem;

namespace rd {

namespace {

// ---- SHA-256（Ver7以降の鍵の生成に使う） ----

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(const uint8_t* p, uint32_t* h) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

std::array<uint8_t, 32> sha256(const std::string& s) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(s.begin(), s.end());
    uint64_t bits = uint64_t(s.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));
    for (size_t i = 0; i < m.size(); i += 64) sha256_block(&m[i], h);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 32; ++i) out[i] = uint8_t(h[i / 4] >> (24 - (i % 4) * 8));
    return out;
}

// ---- 鍵と復号 ----

// 鍵文字列なしのときの鍵（DXA_KeyCreate で 0xAA を変形したもの）
std::array<uint8_t, 12> default_key() {
    std::array<uint8_t, 12> k;
    k.fill(0xaa);
    auto swap4 = [](uint8_t v) { return uint8_t((v >> 4) | (v << 4)); };
    k[0] = uint8_t(~k[0]);
    k[1] = swap4(k[1]);
    k[2] = uint8_t(k[2] ^ 0x8a);
    k[3] = uint8_t(~swap4(k[3]));
    k[4] = uint8_t(~k[4]);
    k[5] = uint8_t(k[5] ^ 0xac);
    k[6] = uint8_t(~k[6]);
    k[7] = uint8_t(~((k[7] >> 3) | (k[7] << 5)));
    k[8] = uint8_t((k[8] >> 5) | (k[8] << 3));
    k[9] = uint8_t(k[9] ^ 0x7f);
    k[10] = uint8_t(swap4(k[10]) ^ 0xd6);
    k[11] = uint8_t(k[11] ^ 0xcc);
    return k;
}

// pos は鍵の位相（データの先頭が鍵の何バイト目に当たるか）
void xor_key(uint8_t* d, size_t n, uint64_t pos, const uint8_t* key, size_t klen) {
    size_t k = size_t(pos % klen);
    for (size_t i = 0; i < n; ++i) {
        d[i] ^= key[k];
        if (++k == klen) k = 0;
    }
}

// DXA_Decode（DXライブラリ独自のLZ圧縮）の展開
bool lz_decode(const std::vector<uint8_t>& src, std::vector<uint8_t>& out) {
    if (src.size() < 9) return false;
    auto u32 = [&](size_t i) { return uint32_t(src[i]) | (uint32_t(src[i + 1]) << 8) | (uint32_t(src[i + 2]) << 16) | (uint32_t(src[i + 3]) << 24); };
    uint32_t dest_size = u32(0);
    uint32_t src_size = u32(4);
    if (src_size < 9 || src_size > src.size()) return false;
    uint8_t keycode = src[8];
    out.clear();
    out.reserve(dest_size);
    size_t sp = 9, end = src_size;
    while (sp < end) {
        if (src[sp] != keycode) {  // 非圧縮のバイト
            out.push_back(src[sp++]);
            continue;
        }
        if (sp + 1 >= end) return false;
        if (src[sp + 1] == keycode) {  // キーコード自体
            out.push_back(keycode);
            sp += 2;
            continue;
        }
        uint32_t code = src[sp + 1];
        if (code > keycode) --code;  // キーコードとの衝突を避けるため+1されている
        sp += 2;
        uint32_t len = code >> 3;
        if (code & 4) {
            if (sp >= end) return false;
            len |= uint32_t(src[sp++]) << 5;
        }
        len += 4;  // 最低圧縮バイト数
        uint32_t index = 0;
        int isize = int(code & 3);
        if (isize == 3 || sp + isize + 1 > end) return false;
        for (int i = 0; i <= isize; ++i) index |= uint32_t(src[sp + i]) << (8 * i);
        sp += isize + 1;
        ++index;
        if (index > out.size()) return false;
        // 参照元と重なることがあるので1バイトずつ写す
        size_t from = out.size() - index;
        for (uint32_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
    return out.size() == dest_size;
}

// ---- パス ----

// 区切りを'/'に揃え、英字を小文字にし、"." と ".." を解決する
std::string normalize_dxa_path(const std::string& rel) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&] {
        if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!cur.empty() && cur != ".") {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') flush();
        else cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    flush();
    std::string r;
    for (const std::string& p : parts) {
        if (!r.empty()) r.push_back('/');
        r += p;
    }
    return r;
}

bool read_range(std::ifstream& f, uint64_t off, uint64_t size, std::vector<uint8_t>& out) {
    out.resize(size_t(size));
    f.clear();
    f.seekg(std::streamoff(off));
    if (size > 0) f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    return bool(f);
}

bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    return read_range(f, 0, uint64_t(n), out);
}

// ---- 情報テーブルの解析 ----

constexpr uint64_t kAttrDirectory = 0x10;

struct TableParser {
    const std::vector<uint8_t>& t;
    int version;
    bool wide;              // Ver6以降は各値が8バイト
    bool utf8_names;
    uint64_t file_table, dir_table;
    uint64_t data_start;
    std::unordered_map<std::string, DxaArchive::Entry>& files;
    std::unordered_set<uint64_t> visited;
    std::string error;

    bool has(uint64_t off, uint64_t n) const { return off <= t.size() && n <= t.size() - off; }
    uint64_t le(uint64_t off, int n) const {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(t[size_t(off) + i]) << (8 * i);
        return v;
    }
    int w() const { return wide ? 8 : 4; }
    uint64_t none() const { return wide ? ~uint64_t(0) : 0xffffffffu; }

    // 名前テーブルの1件（長さ÷4、パリティ、大文字化した名前、元の名前）
    bool name_at(uint64_t addr, std::string& upper, std::string& orig) const {
        if (!has(addr, 4)) return false;
        uint64_t packs = le(addr, 2);
        if (!has(addr + 4, packs * 8)) return false;
        auto cstr = [&](uint64_t off) {
            const char* p = reinterpret_cast<const char*>(t.data() + off);
            return std::string(p, strnlen(p, size_t(packs * 4)));
        };
        upper = cstr(addr + 4);
        orig = cstr(addr + 4 + packs * 4);
        return true;
    }

    std::string to_utf8(const std::string& s) const { return utf8_names ? s : cp932_to_utf8(s); }

    // chain は鍵用の、内側から並べたディレクトリの大文字名
    bool walk(uint64_t dir_off, const std::string& prefix, const std::string& chain) {
        if (!visited.insert(dir_off).second || visited.size() > 100000) {
            error = "ディレクトリ構造が壊れている";
            return false;
        }
        uint64_t d = dir_table + dir_off;
        if (!has(d, uint64_t(w()) * 4)) {
            error = "ディレクトリ情報が範囲外";
            return false;
        }
        uint64_t num = le(d + w() * 2, w());
        uint64_t head = file_table + le(d + w() * 3, w());
        uint64_t fh_size = wide ? 64 : (version >= 2 ? 44 : 40);
        if (num > t.size() / fh_size || !has(head, num * fh_size)) {
            error = "ファイル情報が範囲外";
            return false;
        }
        for (uint64_t i = 0; i < num; ++i) {
            if (!add_entry(head + i * fh_size, prefix, chain)) return false;
        }
        return true;
    }

    bool add_entry(uint64_t fh, const std::string& prefix, const std::string& chain) {
        int n = w();
        std::string upper, orig;
        if (!name_at(le(fh, n), upper, orig)) {
            error = "ファイル名が範囲外";
            return false;
        }
        uint64_t attr = le(fh + n, n);
        uint64_t p = fh + n * 2 + 24;  // 時刻情報の後
        uint64_t addr = le(p, n), size = le(p + n, n);
        std::string path = prefix + to_utf8(orig);
        if (attr & kAttrDirectory) return walk(addr, path + "/", upper + chain);

        DxaArchive::Entry e;
        e.path = path;
        e.offset = data_start + addr;
        e.size = size;
        if (version >= 2) {
            uint64_t packed = le(p + n * 2, n);
            e.compressed = packed != none();
            if (e.compressed) e.packed = packed;
        }
        // Ver5より前はファイル内の絶対位置、以降はファイルサイズが鍵の位相になる
        e.key_pos = version >= 5 ? size : e.offset;
        // Ver7以降は「鍵文字列＋ファイル名＋内側からのディレクトリ名」のSHA-256がファイルごとの鍵
        if (version >= 7) e.key2 = sha256(upper + chain);
        files[normalize_dxa_path(path)] = e;
        return true;
    }
};

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

}  // namespace

bool DxaArchive::open(const std::string& path, std::string* error) {
    files_.clear();
    path_ = path;
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return fail(error, "開けない: " + path);
    f.seekg(0, std::ios::end);
    std::streamoff file_size = f.tellg();
    std::vector<uint8_t> head;
    if (file_size < 24 || !read_range(f, 0, std::min<uint64_t>(48, uint64_t(file_size)), head))
        return fail(error, "ヘッダが短い");

    // 鍵バージョン2、従来の鍵、Ver2以前の鍵(0xFF)の順に IDが "DX" になるものを探す
    const std::array<uint8_t, 32> key2 = sha256("DXARC");
    key_ = default_key();
    auto id_ok = [&](const uint8_t* key, size_t klen) {
        uint8_t id[4];
        std::memcpy(id, head.data(), 4);
        xor_key(id, 4, 0, key, klen);
        version_ = id[2] | (id[3] << 8);
        return id[0] == 'D' && id[1] == 'X';
    };
    bool v2key_ok = id_ok(key2.data(), 32);
    if (!v2key_ok && !id_ok(key_.data(), 12)) {
        key_.fill(0xff);
        if (!id_ok(key_.data(), 12)) return fail(error, "DXアーカイブではない");
    }
    if (version_ < 1 || version_ > 7) return fail(error, "未対応のバージョン: " + std::to_string(version_));

    // ヘッダの残りを復号する（IDとバージョンの4バイトは上で確認済み）
    bool wide = version_ >= 6;
    size_t head_size = wide ? 48 : (version_ >= 4 ? 28 : 24);
    if (head.size() < head_size) return fail(error, "ヘッダが短い");
    head.resize(head_size);
    if (version_ >= 7) xor_key(head.data(), head.size(), 0, key2.data(), 32);
    else xor_key(head.data(), head.size(), 0, key_.data(), 12);
    auto le = [&](size_t off, int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(head[off + i]) << (8 * i);
        return v;
    };
    int n = wide ? 8 : 4;
    uint64_t table_size = le(4, 4);
    uint64_t data_start = le(8, n);
    uint64_t name_table = le(8 + n, n);
    uint64_t file_table = le(8 + n * 2, n);
    uint64_t dir_table = le(8 + n * 3, n);
    uint64_t char_code = version_ >= 4 ? le(8 + n * 4, n) : 932;
    if (char_code == 1200 || char_code == 1201 || char_code == 32766 || char_code == 32767)
        return fail(error, "UTF-16/32 のファイル名には未対応");

    // 情報テーブル（名前・ファイル・ディレクトリ）を読んで復号する
    std::vector<uint8_t> table;
    if (table_size > (uint64_t(1) << 30) || !read_range(f, name_table, table_size, table))
        return fail(error, "情報テーブルが読めない");
    if (version_ >= 7) xor_key(table.data(), table.size(), 0, key2.data(), 32);
    else xor_key(table.data(), table.size(), version_ >= 5 ? 0 : name_table, key_.data(), 12);

    TableParser p{table, version_, wide, char_code == 65001, file_table, dir_table, data_start, files_, {}, {}};
    if (!p.walk(0, "", "")) {
        files_.clear();
        return fail(error, p.error);
    }
    return true;
}

bool DxaArchive::read(const std::string& rel, std::vector<uint8_t>& out) const {
    auto it = files_.find(normalize_dxa_path(rel));
    if (it == files_.end()) return false;
    const Entry& e = it->second;
    std::ifstream f(fs::path(path_), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> raw;
    if (!read_range(f, e.offset, e.compressed ? e.packed : e.size, raw)) return false;
    if (version_ >= 7) xor_key(raw.data(), raw.size(), e.key_pos, e.key2.data(), 32);
    else xor_key(raw.data(), raw.size(), e.key_pos, key_.data(), 12);
    if (!e.compressed) {
        out = std::move(raw);
        return true;
    }
    return lz_decode(raw, out) && out.size() == e.size;
}

bool DxaArchive::exists(const std::string& rel) const {
    return files_.count(normalize_dxa_path(rel)) > 0;
}

std::vector<std::string> DxaArchive::list() const {
    std::vector<std::string> r;
    r.reserve(files_.size());
    for (const auto& [key, e] : files_) r.push_back(e.path);
    return r;
}

namespace {

// dir/stem.dxa を探す。無ければ拡張子の大文字小文字を無視して探す
std::string find_dxa(const fs::path& dir, const std::string& stem) {
    std::error_code ec;
    fs::path exact = dir / (stem + ".dxa");
    if (fs::is_regular_file(exact, ec)) return exact.string();
    std::string want = normalize_dxa_path(stem + ".dxa");
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (normalize_dxa_path(it->path().filename().string()) == want && it->is_regular_file(ec))
            return it->path().string();
    }
    return {};
}

const DxaArchive* cached_archive(const std::string& path) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::unique_ptr<DxaArchive>> cache;
    std::lock_guard<std::mutex> lock(mu);
    if (auto it = cache.find(path); it != cache.end()) return it->second.get();
    if (cache.size() >= 64) cache.clear();  // 溜まりすぎたら作り直す
    auto a = std::make_unique<DxaArchive>();
    if (!a->open(path)) a.reset();  // 開けなかったことも覚えておく
    return cache.emplace(path, std::move(a)).first->second.get();
}

}  // namespace

bool read_file_or_dxa(const std::string& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    fs::path p = fs::path(path);
    if (fs::is_regular_file(p, ec)) return read_whole_file(path, out);

    // 深い方から順に、途中のディレクトリを .dxa に置き換えて探す
    std::vector<fs::path> parts(p.begin(), p.end());
    for (size_t i = parts.size(); i-- > 1;) {
        fs::path dir;
        for (size_t k = 0; k + 1 < i; ++k) dir /= parts[k];
        std::string stem = parts[i - 1].string();
        if (stem.empty() || stem == "/" || stem == "." || stem == "..") continue;
        if (!dir.empty() && !fs::is_directory(dir, ec)) continue;
        std::string arc = find_dxa(dir.empty() ? fs::path(".") : dir, stem);
        if (arc.empty()) continue;
        std::string rel;
        for (size_t k = i; k < parts.size(); ++k) rel += (rel.empty() ? "" : "/") + parts[k].string();
        const DxaArchive* a = cached_archive(arc);
        if (a && a->read(rel, out)) return true;
    }
    return false;
}

}  // namespace rd
