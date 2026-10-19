#include "game/replay.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bmj {

// 書き方：1 行目が "BMJREPLAY <版>"、続いて "キー = 値" のヘッダ、"EVENTS" の後に「時刻(us) レーン 押した(1)/離した(0)」
bool save_replay(const std::string& path, const Replay& r) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    // 途中で落ちても前のリプレイが壊れないよう、別名に書いてから置き換える
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << "BMJREPLAY " << Replay::kVersion << "\n";
        f << "sha256 = " << r.sha256 << "\n";
        f << "seed = " << r.seed << "\n";
        f << "random_version = " << r.random_version << "\n";
        f << "judge_timing = " << r.judge_timing << "\n";
        f << "played_at = " << r.played_at << "\n";
        f << "random = " << r.random << "\n";
        f << "gauge = " << r.gauge << "\n";
        f << "ln_mode = " << r.ln_mode << "\n";
        f << "hs_fix = " << r.hs_fix << "\n";
        f << "effect = " << r.effect << "\n";
        f << "assist = " << (r.assist ? 1 : 0) << "\n";
        f << "gas = " << (r.gas ? 1 : 0) << "\n";
        f << "hi_speed = " << r.hi_speed << "\n";
        f << "lane_cover = " << r.lane_cover << "\n";
        f << "lane_cover_v = " << r.lane_cover_v << "\n";
        if (!r.mods.empty()) f << "mods = " << r.mods << "\n";
        if (!r.custom_gauge.empty()) f << "custom_gauge = " << r.custom_gauge << "\n";
        f << "EVENTS\n";
        for (const ReplayEvent& e : r.events) f << e.time_us << ' ' << static_cast<int>(e.lane) << ' ' << (e.down ? 1 : 0) << '\n';
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

bool load_replay(const std::string& path, Replay& r) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line.rfind("BMJREPLAY ", 0) != 0) return false;
    r = Replay{};
    bool events = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (events) {
            std::istringstream in(line);
            long long t;
            int lane, down;
            if (in >> t >> lane >> down) r.events.push_back({t, static_cast<uint8_t>(lane), down != 0});
            continue;
        }
        if (line == "EVENTS") {
            events = true;
            continue;
        }
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 3);
        int iv = std::atoi(v.c_str());
        if (k == "sha256") r.sha256 = v;
        else if (k == "seed") r.seed = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "random") r.random = iv;
        else if (k == "random_version") r.random_version = iv;
        else if (k == "judge_timing") r.judge_timing = iv;
        else if (k == "played_at") r.played_at = std::atoll(v.c_str());
        else if (k == "gauge") r.gauge = iv;
        else if (k == "ln_mode") r.ln_mode = iv;
        else if (k == "hs_fix") r.hs_fix = iv;
        else if (k == "effect") r.effect = iv;
        else if (k == "assist") r.assist = iv != 0;
        else if (k == "gas") r.gas = iv != 0;
        else if (k == "hi_speed") r.hi_speed = iv;
        else if (k == "lane_cover") r.lane_cover = iv;
        else if (k == "lane_cover_v") r.lane_cover_v = iv;
        else if (k == "mods") r.mods = v;
        else if (k == "custom_gauge") r.custom_gauge = v;
    }
    return events;
}

}  // namespace bmj
