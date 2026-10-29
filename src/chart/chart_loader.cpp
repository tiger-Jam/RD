#include "chart/chart_loader.h"

#include <filesystem>
#include <fstream>

#include "chart/bmson_parser.h"
#include "chart/osu_parser.h"
#include "util/path.h"
#include "util/text.h"

namespace rd {

namespace {
std::string ext_of(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
}  // namespace

bool is_chart_path(const std::string& path) {
    std::string e = ext_of(path);
    return e == ".bms" || e == ".bme" || e == ".bml" || e == ".osu" || e == ".bmson";
}

BmsParseResult parse_chart_bytes(const std::string& path, const std::string& bytes, const BmsParseOptions& opts) {
    std::string e = ext_of(path);
    if (e == ".osu") return parse_osu_text(bytes);
    if (e == ".bmson") return parse_bmson_text(bytes);
    return parse_bms_text(to_utf8_auto(bytes), opts);
}

BmsParseResult parse_chart_file(const std::string& path, const BmsParseOptions& opts) {
    std::string e = ext_of(path);
    if (e != ".osu" && e != ".bmson") return parse_bms_file(path, opts);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        BmsParseResult r;
        r.error = "ファイルを開けない: " + path;
        return r;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BmsParseResult r = parse_chart_bytes(path, bytes, opts);
    r.chart.base_dir = parent_dir(path);
    return r;
}

}  // namespace rd
