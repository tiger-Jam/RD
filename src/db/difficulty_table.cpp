#include "db/difficulty_table.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "util/hash.h"
#include "util/http.h"
#include "util/text.h"
#include "yyjson.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string json_str(yyjson_val* obj, const char* key) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (yyjson_is_str(v)) return yyjson_get_str(v);
    if (yyjson_is_int(v)) return std::to_string(yyjson_get_int(v));
    if (yyjson_is_real(v)) return std::to_string(yyjson_get_real(v));
    return {};
}

// BOMを落とす（UTF-8のBOM付きJSONを出す表がある）
std::string strip_bom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

void lower_hex(std::string& s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

// コース 1 件。charts（譜面のオブジェクト）が無ければ md5 の文字列の並びを読む
bool read_course(yyjson_val* v, const std::vector<std::string>& default_constraints, TableCourse& c) {
    if (!yyjson_is_obj(v)) return false;
    c.name = json_str(v, "name");
    c.constraints = default_constraints;
    if (yyjson_val* con = yyjson_obj_get(v, "constraint"); yyjson_is_arr(con)) {
        c.constraints.clear();
        size_t i, n;
        yyjson_val* s;
        yyjson_arr_foreach(con, i, n, s) if (yyjson_is_str(s)) c.constraints.push_back(yyjson_get_str(s));
    }
    size_t i, n;
    yyjson_val* e;
    if (yyjson_val* charts = yyjson_obj_get(v, "charts"); yyjson_is_arr(charts)) {
        yyjson_arr_foreach(charts, i, n, e) {
            if (!yyjson_is_obj(e)) continue;
            TableChart t;
            t.md5 = json_str(e, "md5"), t.sha256 = json_str(e, "sha256");
            t.title = json_str(e, "title"), t.artist = json_str(e, "artist");
            lower_hex(t.md5), lower_hex(t.sha256);
            c.charts.push_back(std::move(t));
        }
    } else if (yyjson_val* md5 = yyjson_obj_get(v, "md5"); yyjson_is_arr(md5)) {
        yyjson_arr_foreach(md5, i, n, e) {
            if (!yyjson_is_str(e)) continue;
            TableChart t;
            t.md5 = yyjson_get_str(e);
            lower_hex(t.md5);
            c.charts.push_back(std::move(t));
        }
    }
    return !c.charts.empty();
}

// ヘッダの course（コースの並び、または並びの並び）。無ければ古い形の grade（制約は段位・MIRROR 可・LR2 のゲージ）
void read_courses(yyjson_val* header, std::vector<TableCourse>& out) {
    out.clear();
    auto read_list = [&](yyjson_val* arr, const std::vector<std::string>& def) {
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(arr, i, n, v) {
            if (yyjson_is_arr(v)) {
                size_t j, m;
                yyjson_val* w;
                yyjson_arr_foreach(v, j, m, w) {
                    TableCourse c;
                    c.group = static_cast<int>(i);
                    if (read_course(w, def, c)) out.push_back(std::move(c));
                }
            } else {
                TableCourse c;
                if (read_course(v, def, c)) out.push_back(std::move(c));
            }
        }
    };
    if (yyjson_val* c = yyjson_obj_get(header, "course"); yyjson_is_arr(c)) read_list(c, {});
    else if (yyjson_val* g = yyjson_obj_get(header, "grade"); yyjson_is_arr(g)) read_list(g, {"grade_mirror", "gauge_lr2"});
}

bool looks_like_json(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        return c == '{' || c == '[';
    }
    return false;
}

std::string cache_base(const std::string& dir, const std::string& url) {
    return (fs::path(dir) / sha256_hex(url).substr(0, 16)).string();
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.find("://") != std::string::npos) return rel;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return rel;
    size_t host_end = base.find('/', scheme + 3);
    std::string origin = host_end == std::string::npos ? base : base.substr(0, host_end);
    if (rel.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + rel;
    if (!rel.empty() && rel[0] == '/') return origin + rel;
    std::string dir = host_end == std::string::npos ? base + "/" : base.substr(0, base.rfind('/') + 1);
    // クエリは落とす
    size_t q = dir.find('?');
    if (q != std::string::npos) dir = dir.substr(0, dir.rfind('/', q) + 1);
    std::string r = rel;
    while (r.rfind("./", 0) == 0) r.erase(0, 2);
    while (r.rfind("../", 0) == 0) {
        r.erase(0, 3);
        if (dir.size() > origin.size() + 1) {
            dir.pop_back();
            dir = dir.substr(0, dir.rfind('/') + 1);
        }
    }
    return dir + r;
}

// 属性の値の文字参照（&amp; など）を戻す。header.json の URL のクエリに & があると、HTML には &amp; で書かれる
static std::string unescape_attr(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        size_t semi = v[i] == '&' ? v.find(';', i) : std::string::npos;
        if (semi == std::string::npos || semi - i > 10) {
            out += v[i];
            continue;
        }
        const std::string ent = v.substr(i + 1, semi - i - 1);
        long code = -1;
        if (ent == "amp") code = '&';
        else if (ent == "lt") code = '<';
        else if (ent == "gt") code = '>';
        else if (ent == "quot") code = '"';
        else if (ent == "apos") code = '\'';
        else if (ent.size() > 1 && ent[0] == '#') code = std::strtol(ent.c_str() + 1 + (ent[1] == 'x' || ent[1] == 'X'), nullptr,
                                                                     ent[1] == 'x' || ent[1] == 'X' ? 16 : 10);
        if (code <= 0 || code > 0x7f) {  // 知らない参照と ASCII でない文字はそのまま
            out += v[i];
            continue;
        }
        out += static_cast<char>(code);
        i = semi;
    }
    return out;
}

std::string find_bmstable_meta(const std::string& html) {
    std::string low = html;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = 0;
    while ((pos = low.find("<meta", pos)) != std::string::npos) {
        size_t end = low.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = low.substr(pos, end - pos);
        if (tag.find("bmstable") != std::string::npos) {
            size_t c = tag.find("content");
            if (c != std::string::npos) {
                size_t q = tag.find_first_of("\"'", c);
                if (q != std::string::npos) {
                    char quote = tag[q];
                    size_t q2 = tag.find(quote, q + 1);
                    if (q2 != std::string::npos) return unescape_attr(html.substr(pos + q + 1, q2 - q - 1));  // 元の大文字小文字で返す
                }
            }
        }
        pos = end;
    }
    return {};
}

bool parse_table(const std::string& header_json, const std::string& data_json, DifficultyTable& out, std::string& error) {
    std::string h = strip_bom(header_json), d = strip_bom(data_json);
    yyjson_doc* hd = yyjson_read(h.data(), h.size(), 0);
    yyjson_doc* dd = yyjson_read(d.data(), d.size(), 0);
    bool ok = false;
    if (!hd || !yyjson_is_obj(yyjson_doc_get_root(hd))) {
        error = "header.json を読めない";
    } else if (!dd || !yyjson_is_arr(yyjson_doc_get_root(dd))) {
        error = "data.json を読めない";
    } else {
        yyjson_val* hr = yyjson_doc_get_root(hd);
        out.name = json_str(hr, "name");
        out.symbol = json_str(hr, "symbol");
        out.charts.clear();
        out.levels.clear();
        read_courses(hr, out.courses);
        yyjson_val* order = yyjson_obj_get(hr, "level_order");
        if (yyjson_is_arr(order)) {
            size_t i, n;
            yyjson_val* v;
            yyjson_arr_foreach(order, i, n, v) {
                if (yyjson_is_str(v)) out.levels.push_back(yyjson_get_str(v));
                else if (yyjson_is_num(v)) out.levels.push_back(std::to_string(static_cast<int>(yyjson_get_num(v))));
            }
        }
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(yyjson_doc_get_root(dd), i, n, v) {
            if (!yyjson_is_obj(v)) continue;
            TableChart c;
            c.md5 = json_str(v, "md5");
            c.sha256 = json_str(v, "sha256");
            c.level = json_str(v, "level");
            c.title = json_str(v, "title");
            c.artist = json_str(v, "artist");
            c.url = json_str(v, "url");
            c.url_diff = json_str(v, "url_diff");
            for (char& ch : c.md5) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (c.md5.empty() && c.sha256.empty()) continue;
            out.charts.push_back(std::move(c));
        }
        // level_order が無ければ、出てきたレベルを数値順（数値でなければ出てきた順）に並べる
        if (out.levels.empty()) {
            std::vector<std::string> seen;
            for (const TableChart& c : out.charts)
                if (std::find(seen.begin(), seen.end(), c.level) == seen.end()) seen.push_back(c.level);
            std::stable_sort(seen.begin(), seen.end(), [](const std::string& a, const std::string& b) {
                char* ea;
                char* eb;
                double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
                bool na = *ea == 0 && !a.empty(), nb = *eb == 0 && !b.empty();
                if (na && nb) return x < y;
                return na && !nb;
            });
            out.levels = seen;
        }
        ok = true;
    }
    yyjson_doc_free(hd);
    yyjson_doc_free(dd);
    return ok;
}

namespace {

// URLから header.json と data.json の中身を取ってくる（URLはHTMLでもheader.jsonでもよい）
bool fetch_raw(const std::string& url, std::string& header, std::string& data, std::string& error) {
    std::string body;
    if (!http_get(url, body, error)) return false;
    std::string header_url = url;
    if (!looks_like_json(body)) {
        std::string meta = find_bmstable_meta(body);
        if (meta.empty()) return error = "bmstable のメタタグが無い（" + url + "）", false;
        header_url = resolve_url(url, meta);
        if (!http_get(header_url, body, error)) return false;
    }
    header = strip_bom(body);
    yyjson_doc* hd = yyjson_read(header.data(), header.size(), 0);
    std::string data_url = hd && yyjson_is_obj(yyjson_doc_get_root(hd)) ? json_str(yyjson_doc_get_root(hd), "data_url") : "";
    yyjson_doc_free(hd);
    if (data_url.empty()) return error = "header.json に data_url が無い", false;
    return http_get(resolve_url(header_url, data_url), data, error);
}

int64_t now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

bool is_today(int64_t t) {
    if (t <= 0) return false;
    auto day = [](int64_t s) {
        std::time_t tt = static_cast<std::time_t>(s);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        return (tm.tm_year + 1900) * 1000 + tm.tm_yday;
    };
    return day(t) == day(now_sec());
}

bool fetch_table(const std::string& url, DifficultyTable& out, std::string& error) {
    std::string header, data;
    if (!fetch_raw(url, header, data, error)) return false;
    out.url = url;
    if (!parse_table(header, data, out, error)) return false;
    out.fetched = now_sec();
    out.from_cache = false;
    return true;
}

bool load_cached_table(const std::string& cache_dir, const std::string& url, DifficultyTable& out) {
    std::string base = cache_base(cache_dir, url);
    std::string header = read_file(base + ".header.json"), data = read_file(base + ".data.json");
    if (header.empty() || data.empty()) return false;
    std::string err;
    out.url = url;
    if (!parse_table(header, data, out, err)) return false;
    out.from_cache = true;
    std::ifstream meta(base + ".meta");
    std::string line;
    if (std::getline(meta, line)) out.fetched = std::atoll(line.c_str());
    return true;
}

DifficultyTable update_table(const std::string& cache_dir, const std::string& url, bool force) {
    DifficultyTable cached;
    bool have_cache = load_cached_table(cache_dir, url, cached);
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    const std::string base = cache_base(cache_dir, url);
    // 表の更新はその日の最初の 1 回だけ確かめる（起動のたびに取りに行くと待たされる）。
    // 取れなかったときも確かめた時刻を残し、その日のうちは取りに行かない（応答の無いサイトを毎回待たない）
    int64_t checked = 0;
    std::string last_error;
    {
        std::ifstream in(base + ".checked");
        std::string line;
        if (std::getline(in, line)) checked = std::atoll(line.c_str());
        std::getline(in, last_error);
    }
    if (!force && ((have_cache && is_today(cached.fetched)) || is_today(checked))) {
        if (have_cache) {
            if (!last_error.empty() && !is_today(cached.fetched)) cached.warning = "今日は更新できなかったので、手元の版を使う：" + last_error;
            return cached;
        }
        DifficultyTable none;
        none.url = url;
        none.warning = "今日は取得できなかった：" + last_error;
        return none;
    }
    auto mark_checked = [&](const std::string& error) { std::ofstream(base + ".checked") << now_sec() << "\n" << error << "\n"; };
    std::string header, data, error;
    DifficultyTable fresh;
    fresh.url = url;
    if (!fetch_raw(url, header, data, error) || !parse_table(header, data, fresh, error)) {
        mark_checked(error);
        // 取れなかった・壊れていたときは手元の版を使い続ける
        if (have_cache) {
            cached.warning = "更新できなかったので、手元の版を使う：" + error;
            return cached;
        }
        fresh.warning = "取得できない：" + error;
        return fresh;
    }
    mark_checked("");
    if (have_cache && fresh.charts.size() * 2 < cached.charts.size()) {
        // 譜面が半分未満に減ったら、置き換えずに手元の版を使う（仕様書9章）
        cached.warning = "取得した表の譜面が " + std::to_string(cached.charts.size()) + " → " +
                         std::to_string(fresh.charts.size()) + " に減ったので、置き換えなかった";
        return cached;
    }
    fresh.fetched = now_sec();
    // 書き終えてから差し替える（選曲画面が読んでいる最中に、書きかけのファイルを読ませない）
    auto write_whole = [](const std::string& path, const std::string& body) {
        const std::string tmp = path + ".tmp";
        { std::ofstream(tmp, std::ios::binary) << body; }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    };
    write_whole(base + ".header.json", header);
    write_whole(base + ".data.json", data);
    std::ofstream(base + ".meta") << fresh.fetched << "\n" << url << "\n";
    return fresh;
}

// コースの組の名前。組の中のコースの名前に共通する頭（大文字小文字は区別しない）から付ける
// （「GENOCIDE 2018 発狂初段」…「GENOCIDE 2018 overjoy」→「GENOCIDE 2018」）。語の途中で切れたら空白まで戻し、
// 終わりの「発狂」「第」は外す。共通の頭が無い組は、課題曲で見分けられるものはその名前、ほかは表の名前
std::string course_group_label(const std::vector<const TableCourse*>& list, const std::string& table_name) {
    // 年の表記が無い発狂BMS段位認定（2012 年版。発狂初段の 1 曲目 Papyrus で見分ける）
    static const std::map<std::string, std::string> known = {{"c07125de4ed7fbe7cb066cc41e50e51e", "発狂BMS段位認定 2012"}};
    if (!list.empty() && !list.front()->charts.empty())
        if (auto it = known.find(list.front()->charts.front().md5); it != known.end()) return it->second;
    if (list.size() < 2) return table_name;
    auto lower = [](unsigned char c) { return static_cast<char>(c < 0x80 ? std::tolower(c) : c); };
    std::string p = list.front()->name;
    for (const TableCourse* tc : list) {
        size_t n = 0;
        while (n < p.size() && n < tc->name.size() && lower(p[n]) == lower(tc->name[n])) ++n;
        p.resize(n);
    }
    // 文字（UTF-8）の途中で切れていたら、その文字の頭まで戻す
    if (!p.empty()) {
        size_t start = p.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(p[start]) & 0xC0) == 0x80) --start;
        unsigned char lead = static_cast<unsigned char>(p[start]);
        size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
        if (p.size() - start < len) p.resize(start);
    }
    const std::string& first = list.front()->name;
    if (p.size() < first.size() && p.size() > 0 && std::isalnum(static_cast<unsigned char>(first[p.size()])) &&
        std::isalnum(static_cast<unsigned char>(p.back()))) {
        size_t sp = p.rfind(' ');
        p = sp == std::string::npos ? std::string() : p.substr(0, sp);
    }
    // 終わりの段位の言葉（「発狂初段」の「発狂」、「第1期」の「第」）は外す
    for (const std::string& tail : {std::string("発狂"), std::string("第")})
        if (p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0) p.resize(p.size() - tail.size());
    while (!p.empty() && p.back() == ' ') p.pop_back();
    return p.size() >= 3 ? p : table_name;
}

bool write_lr2_courses(const std::string& path, const std::vector<TableCourse>& courses) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    for (const TableCourse& c : courses) {
        bool grade = std::find(c.constraints.begin(), c.constraints.end(), "grade_mirror") != c.constraints.end();
        f << (grade ? "grade" : "expert") << '\t' << c.name << '\t';
        for (size_t i = 0; i < c.charts.size(); ++i) f << (i ? "," : "") << c.charts[i].md5;
        f << '\n';
    }
    return static_cast<bool>(f);
}

std::vector<TableCourse> read_lr2_courses(const std::string& path) {
    std::vector<TableCourse> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    std::map<std::string, int> groups;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        TableCourse c;
        c.name = line.substr(a + 1, b - a - 1);
        // LR2 の段位は、MIRROR まで選べて LR2 のゲージ（LR2 の段位認定と同じ）
        if (line.compare(0, a, "grade") == 0) c.constraints = {"grade_mirror", "gauge_lr2"};
        std::string md5s = line.substr(b + 1);
        for (size_t p = 0; p < md5s.size();) {
            size_t e = md5s.find(',', p);
            if (e == std::string::npos) e = md5s.size();
            if (e > p) c.charts.push_back(TableChart{}), c.charts.back().md5 = md5s.substr(p, e - p);
            p = e + 1;
        }
        // 組：名前の最後の語を除いたもの（空白が続けば 1 つにまとめる）
        std::string key = c.name;
        while (!key.empty() && key.back() == ' ') key.pop_back();
        size_t sp = key.rfind(' ');
        key = sp == std::string::npos ? std::string() : key.substr(0, sp);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        auto [it, fresh] = groups.try_emplace(key, static_cast<int>(groups.size()));
        c.group = it->second;
        if (!c.charts.empty()) out.push_back(std::move(c));
    }
    return out;
}

}  // namespace bmj
