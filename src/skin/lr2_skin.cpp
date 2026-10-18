#include "skin/lr2_skin.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#include "stb_image.h"
#include "util/dxa.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

constexpr int kMaxImages = 100;
constexpr int kSpecialImages = 120;  // 100=STAGEFILE, 101=BACKBMP, 102=BANNER, 110=black, 111=white

// LR2の SplitCSV。str はカンマ区切りの各欄、val はそれを数値にしたもの（"!N" は -N）
struct Csv {
    std::vector<std::string> str;
    int val(size_t i) const {
        if (i >= str.size()) return 0;
        const std::string& s = str[i];
        size_t p = 0;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        if (p < s.size() && s[p] == '!') return -std::atoi(s.c_str() + p + 1);
        return std::atoi(s.c_str() + p);
    }
    std::string s(size_t i) const { return i < str.size() ? str[i] : std::string(); }
};

Csv split_csv(const std::string& line) {
    Csv c;
    std::string cur;
    for (char ch : line) {
        if (ch == ',') {
            c.str.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    c.str.push_back(cur);
    return c;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// '*' だけを扱う単純なワイルドカード（大文字小文字は区別しない）
bool wildcard_match(const std::string& pat, const std::string& name) {
    std::string p = lower(pat), n = lower(name);
    size_t star = p.find('*');
    if (star == std::string::npos) return p == n;
    std::string pre = p.substr(0, star), post = p.substr(star + 1);
    return n.size() >= pre.size() + post.size() && n.compare(0, pre.size(), pre) == 0 &&
           n.compare(n.size() - post.size(), post.size(), post) == 0;
}

// 1つのフォルダの中で、名前を大文字小文字を区別せずに探す
std::string find_in_dir(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return name;
    std::string want = lower(name);
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (lower(n) == want) return n;
    }
    return {};
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

std::vector<std::string> split_path(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur != ".") parts.push_back(cur);
    return parts;
}

std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> lines_of(const std::string& bytes) {
    std::vector<std::string> out;
    std::istringstream in(to_utf8_auto(bytes));
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

// DxLibの読み込みと同じく、透過色は「アルファの無い画像」にだけ適用する
bool decode_image(const std::vector<uint8_t>& bytes, SkinImage& img, const uint8_t trans[3], bool use_trans) {
    int w, h, comp;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) return false;
    img.w = w;
    img.h = h;
    img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    if (use_trans && comp != 4 && comp != 2) {
        for (size_t i = 0; i < img.rgba.size(); i += 4) {
            if (img.rgba[i] == trans[0] && img.rgba[i + 1] == trans[1] && img.rgba[i + 2] == trans[2]) img.rgba[i + 3] = 0;
        }
    }
    return true;
}

bool load_image_file(const std::string& path, SkinImage& img, const uint8_t trans[3], bool use_trans) {
    std::vector<uint8_t> bytes;
    if (!read_file_or_dxa(path, bytes)) return false;
    img.path = path;
    return decode_image(bytes, img, trans, use_trans);
}

bool is_movie(const std::string& p) {
    std::string l = lower(p);
    auto ends = [&](const char* e) { return l.size() >= 3 && l.compare(l.size() - 3, 3, e) == 0; };
    return ends("mpg") || ends("avi") || ends("wma") || ends("ogv");
}

// LR2: ReadSRC
SkinSrc read_src(const Csv& c, const Lr2Skin& sk) {
    SkinSrc s;
    s.valid = true;
    s.n = c.val(1);
    s.gr = c.val(2);
    s.x = std::max(0, c.val(3));
    s.y = std::max(0, c.val(4));
    s.w = c.val(5);
    s.h = c.val(6);
    if (s.gr >= 0 && s.gr < static_cast<int>(sk.images.size())) {
        const SkinImage& img = sk.images[s.gr];
        if (s.w < 0) s.w = img.w;
        if (s.h < 0) s.h = img.h;
    }
    s.div_x = std::max(1, c.val(7));
    s.div_y = std::max(1, c.val(8));
    s.cycle = c.val(9);
    s.timer = c.val(10);
    for (int i = 0; i < 5; ++i) s.op[i] = c.val(11 + i);
    return s;
}

// LR2: ReadDST。番号・ループ・タイマー・オプションは最初の行だけから読む
void read_dst(SkinDst& d, const Csv& c, int order) {
    if (d.keys.empty()) {
        d.n = c.val(1);
        d.loop = c.val(16);
        d.timer = c.val(17);
        for (int i = 0; i < 5; ++i) d.op[i] = c.val(18 + i);
    }
    SkinDstKey k;
    k.time = c.val(2);
    k.x = static_cast<float>(c.val(3));
    k.y = static_cast<float>(c.val(4));
    k.w = static_cast<float>(c.val(5));
    k.h = static_cast<float>(c.val(6));
    k.acc = c.val(7);
    k.a = c.val(8);
    k.r = c.val(9);
    k.g = c.val(10);
    k.b = c.val(11);
    int blend = c.val(12);
    switch (blend) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 9: case 10: case 11: k.blend = blend; break;
    default: k.blend = 1; break;
    }
    k.filter = c.val(13);
    k.angle = static_cast<float>(c.val(14));
    k.center = c.val(15);
    k.sort = static_cast<float>(order);
    d.keys.push_back(k);
}

struct Loader {
    std::string root;
    Lr2Skin& sk;
    const SkinHeader& header;
    uint8_t trans[3] = {0, 255, 0};
    int custom_index = 0;  // #CUSTOMOPTION と #CUSTOMFILE で共有する番号（LR2: customfile_count）
    // #CUSTOMFILE の検索パターンと、選ばれたファイル名
    std::vector<std::pair<std::string, std::string>> custom_files;
    int image_count = 0;

    std::string apply_custom_file(std::string path) {
        for (auto& [pattern, chosen] : custom_files) {
            if (chosen.empty() || chosen == "RANDOM" || chosen == "ERROR") continue;
            if (lower(path).compare(0, pattern.size(), lower(pattern)) == 0) {
                size_t star = path.find('*');
                if (star != std::string::npos) path.replace(star, 1, chosen);
                break;
            }
        }
        return path;
    }

    int read(const std::string& path, int skin_num) {
        std::string bytes = read_text_file(path);
        if (bytes.empty()) {
            sk.warnings.push_back("スキンが読めない: " + path);
            return 0;
        }
        int t_skin = skin_num;
        int if_count = 0;
        int if_switch[100] = {};
        for (std::string line : lines_of(bytes)) {
            if (line.size() <= 6 || line[0] != '#') continue;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            line = std::string(trim(line));

            // #IF / #ELSEIF / #ELSE / #ENDIF（LR2: skinload:888）。欄の値がすべて真なら成立。負の値は常に偽
            auto cond = [&](const Csv& c) {
                for (int i = 1; i < 10; ++i) {
                    int v = c.val(i);
                    if (v < 0 || v > 999 || !sk.op[v]) return false;
                }
                return true;
            };
            if (line.rfind("#IF", 0) == 0) {
                if (if_count != 99) {
                    if_switch[if_count + 1] = cond(split_csv(line)) ? 1 : 2;
                    if_count++;
                }
            } else if (line.rfind("#ELSEIF", 0) == 0 && if_switch[if_count] != 3) {
                if (if_count) {
                    if (if_switch[if_count] == 1) if_switch[if_count] = 3;
                    else if_switch[if_count] = cond(split_csv(line)) ? 1 : 2;
                }
            } else if (line.rfind("#ELSE", 0) == 0 && if_switch[if_count] != 3) {
                if (if_count) if_switch[if_count] = if_switch[if_count] == 1 ? 3 : 1;
            } else if (line.rfind("#ENDIF", 0) == 0) {
                if (if_count) {
                    if_switch[if_count] = 0;
                    if_count--;
                }
            }
            if (if_count > 0 && if_switch[if_count] > 1) continue;

            Csv c = split_csv(line);
            std::string cmd = to_upper_ascii(c.s(0));
            command(cmd, c, t_skin, path);
            t_skin++;
        }
        return t_skin;
    }

    void command(const std::string& cmd, const Csv& c, int& t_skin, const std::string& path) {
        auto src_into = [&](SkinSrc& s) { s = read_src(c, sk); };
        auto lane_idx = [&](int max) {
            int i = c.val(1);
            return (i >= 0 && i <= max) ? i : -1;
        };
        if (cmd == "#IMAGE") {
            if (image_count >= kMaxImages) return;
            std::string p = c.s(1);
            if (p == "CONTINUE") {
                image_count++;
                return;
            }
            SkinImage& img = sk.images[image_count++];
            img = SkinImage{};
            if (is_movie(p)) return;  // 動画は未対応
            std::string resolved = resolve_lr2_path(root, apply_custom_file(p));
            if (resolved.empty() || !load_image_file(resolved, img, trans, true))
                sk.warnings.push_back("画像が読めない: " + p);
        } else if (cmd == "#TRANSCOLOR" || cmd == "#TRANSCLOLR" || cmd == "#TRANSCLOLOR") {
            trans[0] = static_cast<uint8_t>(c.val(1));
            trans[1] = static_cast<uint8_t>(c.val(2));
            trans[2] = static_cast<uint8_t>(c.val(3));
        } else if (cmd == "#FONT") {
            if (sk.ttf_fonts.size() < 10) sk.ttf_fonts.push_back({c.val(1), c.val(2), c.val(3)});
        } else if (cmd == "#LR2FONT") {
            if (sk.image_fonts.size() >= 10) return;
            sk.image_fonts.emplace_back();
            if (c.s(1) != "CONTINUE") load_image_font(apply_custom_file(c.s(1)), sk.image_fonts.back());
        } else if (cmd == "#INCLUDE") {
            std::string p = resolve_lr2_path(root, apply_custom_file(c.s(1)));
            if (t_skin == 0) t_skin = 1;
            if (p.empty()) sk.warnings.push_back("#INCLUDE が見つからない: " + c.s(1));
            else t_skin += read(p, t_skin);
        } else if (cmd == "#CUSTOMOPTION") {
            custom_index++;
        } else if (cmd == "#CUSTOMFILE") {
            std::string chosen;
            if (custom_index < static_cast<int>(header.customs.size())) {
                const SkinCustomItem& it = header.customs[custom_index];
                if (it.selected >= 0 && it.selected < static_cast<int>(it.labels.size())) chosen = it.labels[it.selected];
            }
            if (chosen.empty()) chosen = c.s(3);
            if (chosen == "RANDOM") {
                std::string r = resolve_lr2_path(root, c.s(2));
                if (!r.empty()) chosen = fs::path(r).stem().string();
            }
            custom_files.push_back({c.s(2), chosen});
            custom_index++;
        } else if (cmd == "#SETOPTION") {
            int v = c.val(1);
            if (v >= 0 && v < 1000) sk.op[v] = c.val(2) != 0;
        } else if (cmd == "#STARTINPUT") {
            sk.startinput = c.val(1);
        } else if (cmd == "#LOADSTART") {
            sk.loadstart = c.val(1);
        } else if (cmd == "#LOADEND") {
            sk.loadend = c.val(1);
        } else if (cmd == "#PLAYSTART") {
            sk.playstart = c.val(1);
        } else if (cmd == "#FADEOUT") {
            sk.fadeout = c.val(1);
        } else if (cmd == "#CLOSE" || cmd == "#SKIP") {
            sk.close = c.val(1);
        } else if (cmd == "#SCRATCHSIDE") {
            sk.scratch_side[0] = c.val(1);
            sk.scratch_side[1] = c.val(2);
        } else if (cmd == "#SRC_IMAGE") {
            sk.image_objs.emplace_back();
            src_into(sk.image_objs.back().src);
        } else if (cmd == "#DST_IMAGE") {
            if (!sk.image_objs.empty()) read_dst(sk.image_objs.back().dst, c, t_skin);
        } else if (cmd == "#SRC_NUMBER") {
            sk.numbers.emplace_back();
            src_into(sk.numbers.back().src);
        } else if (cmd == "#DST_NUMBER") {
            if (!sk.numbers.empty()) read_dst(sk.numbers.back().dst, c, t_skin);
        } else if (cmd == "#SRC_BARGRAPH") {
            sk.bargraphs.emplace_back();
            src_into(sk.bargraphs.back().src);
        } else if (cmd == "#DST_BARGRAPH") {
            if (!sk.bargraphs.empty()) read_dst(sk.bargraphs.back().dst, c, t_skin);
            t_skin++;
        } else if (cmd == "#SRC_SLIDER") {
            sk.sliders.emplace_back();
            src_into(sk.sliders.back().src);
        } else if (cmd == "#DST_SLIDER") {
            if (!sk.sliders.empty()) read_dst(sk.sliders.back().dst, c, t_skin);
        } else if (cmd == "#SRC_BGA") {
            sk.bgas.emplace_back();
            src_into(sk.bgas.back().src);
        } else if (cmd == "#DST_BGA") {
            if (!sk.bgas.empty()) read_dst(sk.bgas.back().dst, c, t_skin);
            t_skin++;
        } else if (cmd == "#SRC_TEXT") {
            SkinText t;
            t.src.n = c.val(1);
            t.src.font = c.val(2);
            t.src.st = c.val(3);
            t.src.align = c.val(4);
            for (int i = 0; i < 5; ++i) t.src.op[i] = c.val(5 + i);
            sk.texts.push_back(t);
        } else if (cmd == "#DST_TEXT") {
            if (!sk.texts.empty()) read_dst(sk.texts.back().dst, c, t_skin);
        } else if (cmd == "#SRC_NOTE" || cmd == "#SRC_MINE" || cmd == "#SRC_LN_START" || cmd == "#SRC_LN_END" ||
                   cmd == "#SRC_LN_BODY" || cmd == "#SRC_AUTO_NOTE" || cmd == "#SRC_AUTO_MINE" ||
                   cmd == "#SRC_AUTO_LN_START" || cmd == "#SRC_AUTO_LN_END" || cmd == "#SRC_AUTO_LN_BODY") {
            int i = lane_idx(19);
            if (i < 0) return;
            std::array<SkinSrc, 20>* arr = cmd == "#SRC_NOTE"           ? &sk.note
                                           : cmd == "#SRC_MINE"         ? &sk.mine
                                           : cmd == "#SRC_LN_START"     ? &sk.ln_start
                                           : cmd == "#SRC_LN_END"       ? &sk.ln_end
                                           : cmd == "#SRC_LN_BODY"      ? &sk.ln_body
                                           : cmd == "#SRC_AUTO_NOTE"    ? &sk.auto_note
                                           : cmd == "#SRC_AUTO_MINE"    ? &sk.auto_mine
                                           : cmd == "#SRC_AUTO_LN_START" ? &sk.auto_ln_start
                                           : cmd == "#SRC_AUTO_LN_END"  ? &sk.auto_ln_end
                                                                        : &sk.auto_ln_body;
            src_into((*arr)[i]);
        } else if (cmd == "#DST_NOTE") {
            int i = lane_idx(19);
            if (i < 0) return;
            read_dst(sk.note_dst[i], c, t_skin);
            t_skin += 2;
        } else if (cmd == "#SRC_NOWJUDGE_1P" || cmd == "#SRC_NOWJUDGE_2P") {
            int p = cmd.back() == 'P' && cmd[cmd.size() - 2] == '2' ? 1 : 0;
            int i = lane_idx(5);
            if (i < 0) return;
            src_into(sk.nowjudge[p][i].src);
            sk.nowjudge[p][i].src.timer = 46 + p;
        } else if (cmd == "#DST_NOWJUDGE_1P" || cmd == "#DST_NOWJUDGE_2P") {
            int p = cmd[cmd.size() - 2] == '2' ? 1 : 0;
            int i = lane_idx(5);
            if (i < 0) return;
            read_dst(sk.nowjudge[p][i].dst, c, t_skin);
            sk.nowjudge[p][i].dst.timer = 46 + p;
        } else if (cmd == "#SRC_NOWCOMBO_1P" || cmd == "#SRC_NOWCOMBO_2P") {
            int p = cmd[cmd.size() - 2] == '2' ? 1 : 0;
            int i = lane_idx(5);
            if (i < 0) return;
            src_into(sk.nowcombo[p][i].src);
            sk.nowcombo[p][i].src.timer = 46 + p;
        } else if (cmd == "#DST_NOWCOMBO_1P" || cmd == "#DST_NOWCOMBO_2P") {
            int p = cmd[cmd.size() - 2] == '2' ? 1 : 0;
            int i = lane_idx(5);
            if (i < 0) return;
            read_dst(sk.nowcombo[p][i].dst, c, t_skin);
        } else if (cmd == "#SRC_GROOVEGAUGE" || cmd == "#SRC_LINE" || cmd == "#SRC_JUDGELINE") {
            int i = lane_idx(1);
            if (i < 0) return;
            auto& arr = cmd == "#SRC_GROOVEGAUGE" ? sk.gauge : cmd == "#SRC_LINE" ? sk.line : sk.judgeline;
            src_into(arr[i].src);
        } else if (cmd == "#DST_GROOVEGAUGE" || cmd == "#DST_LINE" || cmd == "#DST_JUDGELINE") {
            int i = lane_idx(1);
            if (i < 0) return;
            auto& arr = cmd == "#DST_GROOVEGAUGE" ? sk.gauge : cmd == "#DST_LINE" ? sk.line : sk.judgeline;
            read_dst(arr[i].dst, c, t_skin);
        }
        (void)path;
    }

    // LR2: ReadImageFont（skinload:443）
    void load_image_font(const std::string& lr2path, ImageFont& f) {
        std::string resolved = resolve_lr2_path(root, lr2path);
        if (resolved.empty()) {
            // .dxa の中にあるときは、フォルダとしては見つからない。パスをそのまま組み立てて dxa を探させる
            std::vector<std::string> parts = split_path(lr2path);
            fs::path p = root;
            for (auto& part : parts) p /= part;
            resolved = p.string();
        }
        std::vector<uint8_t> bytes;
        if (!read_file_or_dxa(resolved, bytes)) {
            sk.warnings.push_back("画像フォントが読めない: " + lr2path);
            return;
        }
        std::string dir = fs::path(resolved).parent_path().string();
        std::map<int, std::string> image_names;
        for (const std::string& raw : lines_of(std::string(bytes.begin(), bytes.end()))) {
            std::string line(trim(raw));
            if (line.size() < 2 || line[0] != '#') continue;
            Csv c = split_csv(line);
            switch (line[1]) {
            case 'R': {
                int id = c.val(1);
                char32_t ch = 0;
                char sjis[3] = {0, 0, 0};
                if (id >= 0 && id <= 255) {
                    sjis[0] = static_cast<char>(id);
                } else if (id >= 256 && id <= 8126) {
                    int i = id + 32832;
                    sjis[0] = static_cast<char>((i >> 8) & 0xFF);
                    sjis[1] = static_cast<char>(i & 0xFF);
                } else if (id >= 8127 && id <= 15306) {
                    int i = id + 49281;
                    sjis[0] = static_cast<char>((i >> 8) & 0xFF);
                    sjis[1] = static_cast<char>(i & 0xFF);
                }
                std::string u8 = id <= 127 ? std::string(1, sjis[0]) : cp932_to_utf8(sjis);
                ch = decode_utf8_first(u8);
                ImageFontChar fc;
                fc.image = c.val(2);
                fc.x = c.val(3);
                fc.y = c.val(4);
                fc.w = c.val(5);
                fc.h = c.val(6);
                f.chars[ch] = fc;
                break;
            }
            case 'M': f.kerning = c.val(1); break;
            case 'S': f.size = c.val(1); break;
            case 'T': image_names[c.val(1)] = std::string(trim(c.s(2))); break;
            default: break;
            }
        }
        int max_img = image_names.empty() ? -1 : image_names.rbegin()->first;
        f.images.assign(static_cast<size_t>(max_img + 1), SkinImage{});
        uint8_t no_trans[3] = {0, 0, 0};
        for (auto& [idx, name] : image_names) {
            if (idx < 0) continue;
            std::string p = (fs::path(dir) / name).string();
            if (!load_image_file(p, f.images[idx], no_trans, false))
                sk.warnings.push_back("画像フォントの画像が読めない: " + p);
        }
        f.loaded = true;
    }

    static char32_t decode_utf8_first(const std::string& s) {
        if (s.empty()) return 0;
        unsigned char c = static_cast<unsigned char>(s[0]);
        if (c < 0x80) return c;
        int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        char32_t v = c & (0x3F >> n);
        for (int i = 1; i <= n && i < static_cast<int>(s.size()); ++i) v = (v << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        return v;
    }
};

}  // namespace

std::string resolve_lr2_path(const std::string& lr2_root, const std::string& path, bool allow_wildcard) {
    std::vector<std::string> parts = split_path(path);
    if (parts.empty()) return {};
    fs::path cur = lr2_root;
    std::error_code ec;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& part = parts[i];
        bool last = i + 1 == parts.size();
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        if (last && allow_wildcard && part.find('*') != std::string::npos) {
            std::vector<std::string> hits;
            for (auto& e : fs::directory_iterator(cur, ec)) {
                std::string n = e.path().filename().string();
                if (e.is_regular_file(ec) && wildcard_match(part, n)) hits.push_back(n);
            }
            if (hits.empty()) return {};
            std::sort(hits.begin(), hits.end());
            std::uniform_int_distribution<size_t> pick(0, hits.size() - 1);
            return (cur / hits[pick(rng())]).string();
        }
        std::string found = find_in_dir(cur, part);
        if (found.empty()) return {};
        cur /= found;
    }
    return cur.string();
}

bool read_skin_header(const std::string& lr2_root, const std::string& skin_path, SkinHeader& out) {
    std::string bytes = read_text_file(skin_path);
    if (bytes.empty()) return false;
    out = SkinHeader{};
    for (std::string line : lines_of(bytes)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("#ENDOFHEADER", 0) == 0) break;
        if (line.empty() || line[0] != '#') continue;
        Csv c = split_csv(line);
        std::string cmd = to_upper_ascii(c.s(0));
        if (cmd == "#INFORMATION") {
            out.type = c.val(1);
            out.title = c.s(2);
            out.maker = c.s(3);
            // OpenLR2の拡張：6番目と7番目の欄に幅と高さ（LR2_skinmanage.cpp:246）
            int w = c.val(6), h = c.val(7);
            if (w && h) {
                out.width = std::max(640, w);
                out.height = std::max(480, h);
                out.has_resolution = true;
            }
        } else if (cmd == "#RESOLUTION") {
            static const int sizes[4][2] = {{640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}};
            int v = c.val(1);
            if (v >= 0 && v <= 3) out.width = sizes[v][0], out.height = sizes[v][1];
            else out.width = v, out.height = c.val(2);
            out.has_resolution = true;
        } else if (cmd == "#CUSTOMOPTION") {
            SkinCustomItem it;
            it.title = c.s(1);
            it.op_start = c.val(2);
            for (size_t i = 3; i < c.str.size(); ++i) {
                std::string l(trim(c.str[i]));
                if (l.empty()) break;
                it.labels.push_back(l);
            }
            out.customs.push_back(it);
        } else if (cmd == "#CUSTOMFILE") {
            SkinCustomItem it;
            it.is_file = true;
            it.title = c.s(1);
            it.pattern = c.s(2);
            it.default_value = std::string(trim(c.s(3)));
            // 候補のファイル名を集める（.txt は除く）
            std::vector<std::string> parts = split_path(it.pattern);
            if (!parts.empty()) {
                std::string pat = parts.back();
                parts.pop_back();
                std::string dir_rel;
                for (auto& p : parts) dir_rel += p + "/";
                std::string dir = resolve_lr2_path(lr2_root, dir_rel, false);
                std::error_code ec;
                if (!dir.empty()) {
                    for (auto& e : fs::directory_iterator(dir, ec)) {
                        std::string n = e.path().filename().string();
                        if (wildcard_match(pat, n) && lower(e.path().extension().string()) != ".txt")
                            it.labels.push_back(e.path().stem().string());
                    }
                }
                std::sort(it.labels.begin(), it.labels.end());
            }
            it.labels.push_back("RANDOM");
            for (size_t i = 0; i < it.labels.size(); ++i)
                if (lower(it.labels[i]) == lower(it.default_value)) it.selected = static_cast<int>(i);
            out.customs.push_back(it);
        }
    }
    return true;
}

bool load_lr2_skin(const std::string& lr2_root, const std::string& skin_path, const SkinHeader& header,
                   const std::array<bool, 1000>& options, Lr2Skin& out, std::string& error) {
    out = Lr2Skin{};
    out.header = header;
    out.width = header.width;
    out.height = header.height;
    out.images.assign(kSpecialImages, SkinImage{});
    out.op = options;
    out.op[0] = true;
    out.op[999] = false;
    // カスタマイズの選択（LR2: LoadScene で customize_value の 900-999 を立てる）
    for (const SkinCustomItem& it : header.customs) {
        if (it.is_file) continue;
        int v = it.op_start + it.selected;
        if (v >= 0 && v < 1000) out.op[v] = true;
    }
    // black.bmp / white.bmp（LR2files/Config）
    uint8_t no_trans[3] = {0, 0, 0};
    for (auto [idx, name] : {std::pair{110, "black.bmp"}, std::pair{111, "white.bmp"}}) {
        std::string p = resolve_lr2_path(lr2_root, std::string("LR2files/Config/") + name);
        if (!p.empty()) load_image_file(p, out.images[idx], no_trans, false);
        if (out.images[idx].rgba.empty()) {
            out.images[idx].w = out.images[idx].h = 1;
            uint8_t v = idx == 110 ? 0 : 255;
            out.images[idx].rgba = {v, v, v, 255};
        }
    }
    Loader ld{lr2_root, out, header, {0, 255, 0}, 0, {}, 0};
    if (ld.read(skin_path, 0) == 0) {
        error = "スキンを読めない: " + skin_path;
        return false;
    }
    return true;
}

static double change_value(double v1, double v2, double t1, double t2, double t, int acc) {
    if (t2 == t1) return v1;
    double r = (t - t1) / (t2 - t1);
    switch (acc) {
    case 0: break;
    case 1: r = r * r * r; break;
    case 2: r = 1 - (1 - r) * (1 - r) * (1 - r); break;
    default: return v1;
    }
    return v1 + (v2 - v1) * r;
}

SkinDraw eval_dst(const SkinDst& dst, double time) {
    SkinDraw o;
    if (dst.keys.empty()) return o;
    int t = static_cast<int>(time);
    int t_start = dst.keys.front().time, t_end = dst.keys.back().time;
    int t2 = t_end;
    if (!(t_start <= t_end && t_start <= t && (0 <= dst.loop || t <= t_end))) return o;
    if (t_start == t_end || dst.loop == t_end) {
        if (t < t2) t2 = t;
    } else if (dst.loop < t_end) {
        t2 = t;
        if (t_end < t) t2 = (t - dst.loop) % (t_end - dst.loop) + dst.loop;
    } else {
        t2 = 0;
    }
    size_t sel = 0;
    for (size_t i = 0; i < dst.keys.size(); ++i)
        if (dst.keys[i].time <= t2) sel = i;
    const SkinDstKey& k = dst.keys[sel];
    o.visible = true;
    o.blend = k.blend;
    o.center = k.center;
    o.filter = k.filter;
    if (t2 != k.time && sel != dst.keys.size() - 1) {
        const SkinDstKey& n = dst.keys[sel + 1];
        auto lerp = [&](double a, double b) { return change_value(a, b, k.time, n.time, t2, k.acc); };
        o.x = static_cast<float>(lerp(k.x, n.x));
        o.y = static_cast<float>(lerp(k.y, n.y));
        o.w = static_cast<float>(lerp(k.w, n.w));
        o.h = static_cast<float>(lerp(k.h, n.h));
        o.sort = static_cast<float>(lerp(k.sort, n.sort));
        o.a = static_cast<int>(lerp(k.a, n.a));
        o.r = static_cast<int>(lerp(k.r, n.r));
        o.g = static_cast<int>(lerp(k.g, n.g));
        o.b = static_cast<int>(lerp(k.b, n.b));
        o.angle = static_cast<float>(lerp(k.angle, n.angle));
        return o;
    }
    o.x = k.x, o.y = k.y, o.w = k.w, o.h = k.h;
    o.a = k.a, o.r = k.r, o.g = k.g, o.b = k.b;
    o.sort = k.sort;
    o.angle = k.angle;
    return o;
}

int src_frame(const SkinSrc& src, double time) {
    if (time < 0.0) return 0;
    int count = src.graphcount();
    if (count <= 0 || src.cycle <= 0) return 0;
    int r = (static_cast<int>(time) % src.cycle) * count / src.cycle;
    if (r >= count || r < 0) r = 0;
    return r;
}

}  // namespace bmj
