#include "mod/script.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>

// Lua は C++ として組んでいる（CMakeLists.txt）ので、extern "C" の lua.hpp ではなく素のヘッダを読む
#include "lauxlib.h"
#include "lua.h"
#include "util/dev.h"
#include "lualib.h"

namespace fs = std::filesystem;

namespace rd {

namespace {

// 1 回の呼び出しで動かしてよい命令の数（止まらないスクリプトで本体が固まらないように）。
// フォルダを作るときは譜面の数だけ回るので多めにする
constexpr long long kStepsPerCall = 5'000'000;
constexpr long long kStepsFolders = 2'000'000'000;
constexpr long long kHookEvery = 10'000;
constexpr size_t kMemoryLimit = 256u << 20;  // 1 つの MOD が使ってよいメモリ
// フォルダを作るときは全譜面の表を渡すので、譜面 1 つにつきこれだけ足す（傾向と難易度表のレベルを付けた表は 1 つ 1.5 KB ほど。
// 16 万譜面で 256 MB を超えて、譜面の傾向の MOD が作れなかった）
constexpr size_t kMemoryPerChart = 4u << 10;

const char* kJudgeNames[6] = {"EMPTY_POOR", "POOR", "BAD", "GOOD", "GREAT", "PGREAT"};

// パスを UTF-8 の文字列にする
std::string utf8_of(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

}  // namespace

bool mod_file_name_ok(const std::string& name) {
    if (name.empty() || name.size() > 200) return false;
    fs::path p(name);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p)
        if (part == ".." || part == ".") return false;
    for (char c : name)
        if (c == ':' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

// MOD 1 つぶんの Lua の環境
class ModScript {
public:
    ModScript(const ModManifest& m, std::string data_dir)
        : id_(m.id), name_(m.name), dir_(m.dir), touches_(m.touches), data_dir_(std::move(data_dir)) {}
    ~ModScript() {
        if (L_) lua_close(L_);
    }

    bool load(const std::string& script, std::string& err) {
        L_ = lua_newstate(&ModScript::alloc, this);
        if (!L_) return err = id_ + ": Lua を始められない", false;
        *static_cast<ModScript**>(lua_getextraspace(L_)) = this;
        open_libs();
        std::ifstream f(fs::path(dir_) / script, std::ios::binary);
        if (!f) return err = id_ + ": スクリプトが無い: " + script, false;
        std::string code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // 文字のスクリプトだけ（バイトコードは読まない）
        if (luaL_loadbufferx(L_, code.data(), code.size(), ("@" + script).c_str(), "t") != LUA_OK) {
            err = id_ + ": " + lua_tostring(L_, -1);
            return false;
        }
        if (!call(0, kStepsPerCall)) return err = error_, false;
        return true;
    }

    bool alive() const { return L_ && error_.empty(); }
    bool touches(ModTouch t) const { return (touches_ & static_cast<uint32_t>(t)) != 0; }
    const std::string& error() const { return error_; }
    const std::string& id() const { return id_; }

    // 窓口の関数 name があるか
    bool has(const char* name) {
        if (!alive()) return false;
        bool f = lua_getglobal(L_, name) == LUA_TFUNCTION;
        lua_pop(L_, 1);
        return f;
    }
    // 窓口の関数 name を、push_arg が積んだ 1 つの引数で呼ぶ。read_result があれば戻り値を 1 つ読む。
    // 引数を作るところから守られた呼び出しの中で行う（メモリが足りないときも本体を落とさない）
    bool run(const char* name, const std::function<void(lua_State*)>& push_arg, long long steps,
             const std::function<void(lua_State*)>& read_result = {}) {
        if (!has(name)) return false;
        struct Ctx {
            const char* name;
            const std::function<void(lua_State*)>* push_arg;
            const std::function<void(lua_State*)>* read_result;
        } ctx{name, &push_arg, &read_result};
        lua_pushcfunction(L_, [](lua_State* L) -> int {
            auto* c = static_cast<Ctx*>(lua_touserdata(L, 1));
            lua_getglobal(L, c->name);
            (*c->push_arg)(L);
            lua_call(L, 1, c->read_result && *c->read_result ? 1 : 0);
            if (c->read_result && *c->read_result) (*c->read_result)(L);
            return 0;
        });
        lua_pushlightuserdata(L_, &ctx);
        return call(1, steps);
    }

    // 積んである関数と nargs 個の引数を呼ぶ。失敗したらこの MOD を止める
    bool call(int nargs, long long steps, int nresults = 0) {
        steps_left_ = steps;
        lua_sethook(L_, &ModScript::hook, LUA_MASKCOUNT, static_cast<int>(kHookEvery));
        int base = lua_gettop(L_) - nargs;
        lua_pushcfunction(L_, &ModScript::traceback);
        lua_insert(L_, base);
        int rc = lua_pcall(L_, nargs, nresults, base);
        lua_remove(L_, base);
        lua_sethook(L_, nullptr, 0, 0);
        if (rc != LUA_OK) {
            const char* m = lua_tostring(L_, -1);
            error_ = id_ + ": " + (m ? m : "スクリプトが失敗した");
            lua_pop(L_, 1);
            return false;
        }
        return true;
    }

    std::vector<ModDrawCmd>* draw_sink = nullptr;
    // スクリプトが読んだファイルと、今の時刻を見たか（MOD のフォルダの計算を残すとき、作り直しが要るかを決める）
    std::vector<std::string> read_paths;
    bool read_time = false;
    size_t memory_limit = kMemoryLimit;

private:
    static ModScript* self(lua_State* L) { return *static_cast<ModScript**>(lua_getextraspace(L)); }

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
        auto* s = static_cast<ModScript*>(ud);
        size_t old = ptr ? osize : 0;
        if (nsize == 0) {
            s->memory_ -= old;
            std::free(ptr);
            return nullptr;
        }
        if (nsize > old && s->memory_ + (nsize - old) > s->memory_limit) return nullptr;  // Lua は「メモリが足りない」で止まる
        void* p = std::realloc(ptr, nsize);
        if (p) s->memory_ = s->memory_ - old + nsize;
        return p;
    }

    static void hook(lua_State* L, lua_Debug*) {
        ModScript* s = self(L);
        s->steps_left_ -= kHookEvery;
        if (s->steps_left_ < 0) luaL_error(L, "時間がかかりすぎた（1 回の呼び出しで動かせる量を超えた）");
    }

    static int traceback(lua_State* L) {
        const char* m = lua_tostring(L, 1);
        luaL_traceback(L, L, m ? m : "スクリプトが失敗した", 1);
        return 1;
    }

    void open_libs() {
        static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                        {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                        {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine}};
        for (const luaL_Reg& l : libs) {
            luaL_requiref(L_, l.name, l.func, 1);
            lua_pop(L_, 1);
        }
        // math.random の種。Lua の既定は時刻（秒）とアドレスなので、同じ秒に作り直した環境では同じ並びになる
        {
            std::random_device rd;
            lua_getglobal(L_, LUA_MATHLIBNAME);
            lua_getfield(L_, -1, "randomseed");
            lua_pushinteger(L_, static_cast<lua_Integer>(rd()));
            lua_pushinteger(L_, static_cast<lua_Integer>(rd()));
            lua_call(L_, 2, 0);
            lua_pop(L_, 1);
        }
        // ファイルを読んで動かす関数と、バイトコードを読める load は外す
        for (const char* name : {"dofile", "loadfile", "load", "require", "collectgarbage"}) {
            lua_pushnil(L_);
            lua_setglobal(L_, name);
        }
        lua_pushcfunction(L_, &ModScript::l_log);
        lua_setglobal(L_, "print");

        lua_newtable(L_);
        static const luaL_Reg fns[] = {{"log", &ModScript::l_log},
                                       {"write_file", &ModScript::l_write_file},
                                       {"append_file", &ModScript::l_append_file},
                                       {"read_file", &ModScript::l_read_file},
                                       {"read_mod_file", &ModScript::l_read_mod_file},
                                       {"time", &ModScript::l_time},
                                       {"date", &ModScript::l_date},
                                       {"draw_rect", &ModScript::l_draw_rect},
                                       {"draw_text", &ModScript::l_draw_text},
                                       {nullptr, nullptr}};
        luaL_setfuncs(L_, fns, 0);
        lua_newtable(L_);
        for (int i = 0; i < 6; ++i) {
            lua_pushinteger(L_, i);
            lua_setfield(L_, -2, kJudgeNames[i]);
        }
        lua_setfield(L_, -2, "JUDGE");
        lua_pushstring(L_, id_.c_str());
        lua_setfield(L_, -2, "id");
        lua_setglobal(L_, "rd");
    }

    // rd.log(...)：黒い画面に出す
    static int l_log(lua_State* L) {
        int n = lua_gettop(L);
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        for (int i = 1; i <= n; ++i) {
            if (i > 1) luaL_addchar(&b, ' ');
            luaL_tolstring(L, i, nullptr);
            luaL_addvalue(&b);
        }
        luaL_pushresult(&b);
        std::printf("MOD %s: %s\n", self(L)->id_.c_str(), lua_tostring(L, -1));
        return 0;
    }

    // 書けたら true。書けなければ nil と理由（スクリプトは止めない）
    static int write_impl(lua_State* L, bool append) {
        size_t nlen = 0, tlen = 0;
        const char* n = luaL_checklstring(L, 1, &nlen);
        const char* t = luaL_checklstring(L, 2, &tlen);
        ModScript* s = self(L);
        bool ok = false;
        const char* why = "名前が使えない";
        {
            std::string name(n, nlen);
            if (mod_file_name_ok(name)) {
                fs::path p = fs::path(s->data_dir_) / fs::u8path(name);
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
                if (f) f.write(t, static_cast<std::streamsize>(tlen));
                ok = static_cast<bool>(f);
                why = "書けない";
            }
        }
        if (ok) return lua_pushboolean(L, 1), 1;
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    static int l_write_file(lua_State* L) { return write_impl(L, false); }
    static int l_append_file(lua_State* L) { return write_impl(L, true); }

    static int read_impl(lua_State* L, const std::string& root) {
        size_t nlen = 0;
        const char* n = luaL_checklstring(L, 1, &nlen);
        bool ok = false;
        {
            std::string name(n, nlen), text;
            if (mod_file_name_ok(name)) {
                self(L)->read_paths.push_back(utf8_of(fs::path(root) / fs::u8path(name)));
                std::ifstream f(fs::path(root) / fs::u8path(name), std::ios::binary);
                if (f) {
                    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    lua_pushlstring(L, text.data(), text.size());
                    ok = true;
                }
            }
        }
        if (!ok) lua_pushnil(L);
        return 1;
    }
    // rd.read_file(name)：自分が書いたファイル。rd.read_mod_file(name)：MOD のフォルダのファイル
    static int l_read_file(lua_State* L) { return read_impl(L, self(L)->data_dir_); }
    static int l_read_mod_file(lua_State* L) { return read_impl(L, self(L)->dir_); }

    static int l_time(lua_State* L) {
        self(L)->read_time = true;
        lua_pushinteger(L, static_cast<lua_Integer>(std::time(nullptr)));
        return 1;
    }
    // rd.date(書式, 時刻)：strftime と同じ書式（既定 "%Y-%m-%d %H:%M:%S"、時刻の既定は今）
    static int l_date(lua_State* L) {
        const char* fmt = luaL_optstring(L, 1, "%Y-%m-%d %H:%M:%S");
        if (lua_isnoneornil(L, 2)) self(L)->read_time = true;
        std::time_t t = static_cast<std::time_t>(luaL_optinteger(L, 2, static_cast<lua_Integer>(std::time(nullptr))));
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[256];
        size_t n = std::strlen(fmt) < 100 ? std::strftime(buf, sizeof(buf), fmt, &tm) : 0;
        lua_pushlstring(L, buf, n);
        return 1;
    }

    // rd.draw_rect(x, y, w, h, 色 0xRRGGBB, 不透明度 0〜255)
    static int l_draw_rect(lua_State* L) {
        ModDrawCmd c;
        c.kind = ModDrawCmd::Kind::Rect;
        c.x = static_cast<float>(luaL_checknumber(L, 1)), c.y = static_cast<float>(luaL_checknumber(L, 2));
        c.w = static_cast<float>(luaL_checknumber(L, 3)), c.h = static_cast<float>(luaL_checknumber(L, 4));
        c.rgb = static_cast<uint32_t>(luaL_optinteger(L, 5, 0xffffff)) & 0xffffff;
        c.alpha = static_cast<uint8_t>(std::clamp<lua_Integer>(luaL_optinteger(L, 6, 255), 0, 255));
        ModScript* s = self(L);
        if (s->draw_sink && s->draw_sink->size() < 4096) s->draw_sink->push_back(c);
        return 0;
    }
    // rd.draw_text(x, y, 文字, 大きさ, 色, 不透明度, 揃え 0 左 1 中央 2 右)
    static int l_draw_text(lua_State* L) {
        lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
        size_t len = 0;
        const char* t = luaL_checklstring(L, 3, &len);
        lua_Integer size = luaL_optinteger(L, 4, 16), rgb = luaL_optinteger(L, 5, 0xffffff), a = luaL_optinteger(L, 6, 255),
                    align = luaL_optinteger(L, 7, 0);
        ModScript* s = self(L);
        if (s->draw_sink && s->draw_sink->size() < 4096) {
            ModDrawCmd c;
            c.kind = ModDrawCmd::Kind::Text;
            c.x = static_cast<float>(x), c.y = static_cast<float>(y);
            c.text.assign(t, std::min<size_t>(len, 256));
            c.size = static_cast<int>(std::clamp<lua_Integer>(size, 6, 128));
            c.rgb = static_cast<uint32_t>(rgb) & 0xffffff;
            c.alpha = static_cast<uint8_t>(std::clamp<lua_Integer>(a, 0, 255));
            c.align = static_cast<int>(std::clamp<lua_Integer>(align, 0, 2));
            s->draw_sink->push_back(std::move(c));
        }
        return 0;
    }

    lua_State* L_ = nullptr;
    std::string id_, name_, dir_;
    uint32_t touches_ = 0;
    std::string data_dir_;
    std::string error_;
    size_t memory_ = 0;
    long long steps_left_ = 0;
};

namespace {

void set_str(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void set_int(lua_State* L, const char* k, long long v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, -2, k);
}
void set_num(lua_State* L, const char* k, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void set_bool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

// 譜面 1 つぶんの値を、MOD に渡す順に v へ出す。Lua に積むとき（LuaPush）と、MOD のフォルダの計算を残すための
// 指紋を取るとき（Fingerprint）で同じものを使い、渡す値と指紋がずれないようにする。
//   v.table(配列の数, 名前の数) で表を始め、v.str / v.integer / v.num / v.boolean で名前つきの値を足し、
//   v.field(名前) で表を親の名前つきの値に、v.item(番号) で親の配列の要素にする
template <class V>
void visit_chart(V& v, const ChartRecord& c, const std::unordered_map<std::string, ScoreRecord>& scores, const ModChartExtra& extra) {
    v.table(0, 30);
    v.str("sha256", c.sha256), v.str("md5", c.md5), v.str("path", c.path), v.str("folder", c.folder);
    v.str("title", c.title), v.str("subtitle", c.subtitle), v.str("artist", c.artist);
    v.str("genre", c.genre);
    v.integer("playlevel", c.playlevel), v.integer("difficulty", c.difficulty), v.integer("notes", c.notes);
    v.integer("ln", c.ln), v.integer("rank", c.rank), v.integer("total", c.total), v.integer("keys", c.keys);
    v.num("bpm_min", c.bpm_min), v.num("bpm_max", c.bpm_max), v.num("bpm_main", c.bpm_main);
    v.integer("length_ms", c.length_ms), v.boolean("scratch", c.scratch), v.integer("added", c.added);
    auto it = scores.find(c.sha256);
    if (it != scores.end()) {
        v.integer("clear", it->second.clear), v.integer("exscore", it->second.exscore);
        v.integer("bp", it->second.bp), v.integer("playcount", it->second.playcount);
        v.integer("last_played", it->second.last_played);
    } else {
        v.integer("clear", 0);
    }
    if (extra.attrs)
        if (auto a = extra.attrs->find(c.sha256); a != extra.attrs->end()) {
            const ChartAttr& x = a->second;
            v.table(0, 20);
            v.integer("code", x.attr), v.integer("code2", x.attr2), v.integer("sub", x.sub), v.integer("mask", x.mask);
            v.integer("practice_low", x.practice_low), v.num("conf", x.conf), v.num("grid_bpm", x.grid_bpm);
            v.num("avg_chord", x.avg_chord), v.num("micro_rate", x.micro_rate), v.num("subgrid_rate", x.subgrid_rate);
            v.num("avg_density", x.avg_density), v.num("d10", x.d10), v.num("randa_33", x.randa_33);
            v.num("randa_42", x.randa_42), v.num("randa_51", x.randa_51), v.num("stair_rate", x.stair_rate);
            v.num("full_rate", x.full_rate), v.num("chart_seconds", x.chart_seconds);
            v.field("attr");
        }
    if (extra.tables)
        if (auto t = extra.tables->find(c.md5); t != extra.tables->end()) {
            v.table(static_cast<int>(t->second.size()), 0);
            long long k = 0;
            for (const ModTableLevel& tl : t->second) {
                v.table(0, 5);
                v.str("table", tl.table), v.str("symbol", tl.symbol), v.str("level", tl.level);
                v.integer("table_order", tl.table_order), v.integer("level_order", tl.level_order);
                v.item(++k);
            }
            v.field("tables");
        }
}

struct LuaPush {
    lua_State* L;
    void table(int narr, int nrec) { lua_createtable(L, narr, nrec); }
    void str(const char* k, const std::string& x) { set_str(L, k, x); }
    void integer(const char* k, long long x) { set_int(L, k, x); }
    void num(const char* k, double x) { set_num(L, k, x); }
    void boolean(const char* k, bool x) { set_bool(L, k, x); }
    void field(const char* k) { lua_setfield(L, -2, k); }
    void item(long long i) { lua_rawseti(L, -2, static_cast<lua_Integer>(i)); }
};

// 渡す値の指紋（FNV-1a 64 ビット）。値の区切りも混ぜる（"ab"+"c" と "a"+"bc" を分ける）
struct Fingerprint {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    void tag(char t, const char* k) {
        bytes(&t, 1);
        bytes(k, std::strlen(k) + 1);
    }
    void table(int, int) { tag('{', ""); }
    void str(const char* k, const std::string& x) {
        tag('s', k);
        const uint64_t n = x.size();
        bytes(&n, sizeof(n));
        bytes(x.data(), x.size());
    }
    void integer(const char* k, long long x) { tag('i', k), bytes(&x, sizeof(x)); }
    void num(const char* k, double x) { tag('n', k), bytes(&x, sizeof(x)); }
    void boolean(const char* k, bool x) { tag('b', k), bytes(&x, 1); }
    void field(const char* k) { tag('}', k); }
    void item(long long i) { tag(']', ""), bytes(&i, sizeof(i)); }
};

// 譜面 1 つの表：{ sha256, md5, title, …, clear, exscore, bp, playcount, last_played }（記録が無ければ clear = 0）。
// extra があれば、傾向（attr = { code, code2, sub, mask, … }）と難易度表のレベル（tables = { { table, symbol, level, … }, … }）
void push_chart(lua_State* L, const ChartRecord& c, const std::unordered_map<std::string, ScoreRecord>& scores,
                const ModChartExtra& extra = {}) {
    LuaPush v{L};
    visit_chart(v, c, scores, extra);
}

std::unique_ptr<ModScript> open_script(const ModManifest& m, const std::string& data_root, std::string& err) {
    auto s = std::make_unique<ModScript>(m, (fs::path(data_root) / fs::u8path(m.id)).string());
    if (!s->load(m.script, err)) return nullptr;
    return s;
}

// フォルダの表 { title = "…", charts = { sha256 か { sha256 = …, label = … }, … }, keep_order = 真偽, folders = { … } } を読む（深さは 6 まで）
void read_folder(lua_State* L, int idx, ModFolder& out, int depth) {
    idx = lua_absindex(L, idx);
    if (lua_getfield(L, idx, "title") == LUA_TSTRING) out.title = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "keep_order");
    out.keep_order = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "charts") == LUA_TTABLE) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            const int t = lua_rawgeti(L, -1, i);
            if (t == LUA_TSTRING) {
                out.charts.push_back(lua_tostring(L, -1));
                out.labels.emplace_back();
            } else if (t == LUA_TTABLE) {
                if (lua_getfield(L, -1, "sha256") == LUA_TSTRING) {
                    out.charts.push_back(lua_tostring(L, -1));
                    lua_pop(L, 1);
                    out.labels.push_back(lua_getfield(L, -1, "label") == LUA_TSTRING ? lua_tostring(L, -1) : "");
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (depth < 6 && lua_getfield(L, idx, "folders") == LUA_TTABLE) {
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TTABLE) {
                ModFolder f;
                read_folder(L, -1, f, depth + 1);
                if (!f.title.empty()) out.children.push_back(std::move(f));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

}  // namespace

ModScripts::ModScripts() = default;
ModScripts::~ModScripts() = default;

void ModScripts::load(const std::vector<const ModManifest*>& mods, const std::string& data_root) {
    scripts_.clear();
    for (const ModManifest* m : mods) {
        if (m->script.empty()) continue;
        std::string err;
        if (auto s = open_script(*m, data_root, err)) scripts_.push_back(std::move(s));
        else errors_.push_back(err);
    }
}

void ModScripts::play_start(const ModPlayInfo& info) {
    for (auto& s : scripts_) {
        if (!s->has("on_play_start")) continue;
        bool ok = s->run("on_play_start", [&](lua_State* L) {
            lua_newtable(L);
            set_str(L, "sha256", info.sha256), set_str(L, "md5", info.md5), set_str(L, "path", info.path);
            set_str(L, "title", info.title), set_str(L, "subtitle", info.subtitle), set_str(L, "artist", info.artist);
            set_str(L, "genre", info.genre), set_str(L, "gauge", info.gauge);
            set_int(L, "playlevel", info.playlevel), set_int(L, "notes", info.notes), set_int(L, "total", info.total);
            set_int(L, "rank", info.rank), set_int(L, "keys", info.keys), set_int(L, "length_ms", info.length_ms);
            set_num(L, "bpm_min", info.bpm_min), set_num(L, "bpm_max", info.bpm_max);
            set_int(L, "random", info.random);
            set_bool(L, "autoplay", info.autoplay), set_bool(L, "replay", info.replay), set_bool(L, "course", info.course);
            set_int(L, "view_w", info.view_w), set_int(L, "view_h", info.view_h);
            // windows = { pgreat = { 早, 遅 }, great = …, good = …, bad = … }
            lua_newtable(L);
            const std::pair<const char*, const int*> w[] = {{"pgreat", info.pgreat}, {"great", info.great}, {"good", info.good}, {"bad", info.bad}};
            for (const auto& [k, v] : w) {
                lua_createtable(L, 2, 0);
                lua_pushinteger(L, v[0]), lua_rawseti(L, -2, 1);
                lua_pushinteger(L, v[1]), lua_rawseti(L, -2, 2);
                lua_setfield(L, -2, k);
            }
            lua_setfield(L, -2, "windows");
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

void ModScripts::judge(const ModJudgeInfo& e) {
    for (auto& s : scripts_) {
        if (!s->has("on_judge")) continue;
        bool ok = s->run("on_judge", [&](lua_State* L) {
            lua_newtable(L);
            set_num(L, "time_ms", e.time_ms);
            set_int(L, "lane", e.lane);
            set_int(L, "judge", e.judge);
            set_str(L, "name", e.mine ? "MINE" : kJudgeNames[std::clamp(e.judge, 0, 5)]);
            if (e.has_offset) set_int(L, "offset_ms", e.offset_ms);
            set_bool(L, "fast", e.fast), set_bool(L, "mine", e.mine), set_bool(L, "ln_end", e.ln_end);
            set_int(L, "combo", e.combo), set_int(L, "exscore", e.exscore);
            set_num(L, "hp", e.hp);
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

void ModScripts::play_end(const ModPlayResult& r) {
    for (auto& s : scripts_) {
        if (!s->has("on_play_end")) continue;
        bool ok = s->run("on_play_end", [&](lua_State* L) {
            lua_newtable(L);
            set_int(L, "clear", r.clear), set_int(L, "exscore", r.exscore), set_int(L, "bp", r.bp);
            set_int(L, "max_combo", r.max_combo), set_int(L, "total_notes", r.total_notes);
            set_int(L, "fast", r.fast), set_int(L, "slow", r.slow);
            set_bool(L, "quit_early", r.quit_early);
            lua_newtable(L);
            for (int i = 0; i < 6; ++i) set_int(L, kJudgeNames[i], r.judge[i]);
            lua_setfield(L, -2, "judges");
            lua_newtable(L);
            for (const auto& [k, v] : r.gauges) set_num(L, k.c_str(), v);
            lua_setfield(L, -2, "gauges");
        }, kStepsPerCall);
        if (!ok) errors_.push_back(s->error());
    }
}

const std::vector<ModDrawCmd>& ModScripts::draw(const ModDrawState& st) {
    draw_.clear();
    for (auto& s : scripts_) {
        if (!s->touches(ModTouch::Visual) || !s->has("on_draw")) continue;
        s->draw_sink = &draw_;
        bool ok = s->run("on_draw", [&](lua_State* L) {
            lua_newtable(L);
            set_num(L, "now_ms", st.now_ms), set_num(L, "song_ms", st.song_ms);
            set_int(L, "combo", st.combo), set_int(L, "exscore", st.exscore);
            set_num(L, "hp", st.hp);
            set_int(L, "view_w", st.view_w), set_int(L, "view_h", st.view_h);
        }, kStepsPerCall);
        s->draw_sink = nullptr;
        if (!ok) errors_.push_back(s->error());
    }
    return draw_;
}

std::vector<std::string> ModScripts::take_errors() {
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

namespace {

// MOD のフォルダの計算を残したファイル（<cache_dir>/<MOD の id>.cache）。中身は次の順：
//   目印・渡した値の指紋・時刻を見たならその時（1 時間の区切り）・スクリプトが読んだファイル（場所・大きさ・更新日時）・フォルダ
// 指紋と、読んだファイルと、時刻（見たときだけ）が同じなら、計算せずにこのフォルダを使う
constexpr char kModCacheMagic[] = "RDMODFOLDERS1";

struct FileStamp {
    std::string path;
    int64_t size = -1;  // 無いファイルは -1
    int64_t mtime = 0;
};

FileStamp stamp_of(const std::string& path) {
    FileStamp f;
    f.path = path;
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const auto size = fs::file_size(p, ec);
    if (ec) return f;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return f;
    f.size = static_cast<int64_t>(size);
    f.mtime = static_cast<int64_t>(t.time_since_epoch().count());
    return f;
}

int64_t current_hour() { return static_cast<int64_t>(std::time(nullptr)) / 3600; }

struct CacheWriter {
    std::string buf;
    void u64(uint64_t x) { buf.append(reinterpret_cast<const char*>(&x), sizeof(x)); }
    void str(const std::string& x) { u64(x.size()), buf.append(x); }
    void folder(const ModFolder& f) {
        str(f.title), u64(f.keep_order), u64(f.charts.size());
        for (size_t i = 0; i < f.charts.size(); ++i) str(f.charts[i]), str(i < f.labels.size() ? f.labels[i] : "");
        u64(f.children.size());
        for (const ModFolder& c : f.children) folder(c);
    }
};

struct CacheReader {
    const std::string& buf;
    size_t pos = 0;
    bool ok = true;
    uint64_t u64() {
        uint64_t x = 0;
        if (pos + sizeof(x) > buf.size()) return ok = false, 0;
        std::memcpy(&x, buf.data() + pos, sizeof(x));
        pos += sizeof(x);
        return x;
    }
    std::string str() {
        const uint64_t n = u64();
        if (!ok || n > buf.size() - pos) return ok = false, std::string();
        std::string x = buf.substr(pos, n);
        pos += n;
        return x;
    }
    bool folder(ModFolder& f, int depth) {
        if (depth > 6) return ok = false;
        f.title = str(), f.keep_order = u64() != 0;
        const uint64_t n = u64();
        for (uint64_t i = 0; ok && i < n; ++i) f.charts.push_back(str()), f.labels.push_back(str());
        const uint64_t nc = u64();
        for (uint64_t i = 0; ok && i < nc; ++i) {
            f.children.emplace_back();
            folder(f.children.back(), depth + 1);
        }
        return ok;
    }
};

// 残した計算が今も使えるなら、そのフォルダを out に足して true
bool read_mod_cache(const std::string& path, uint64_t key, std::vector<ModFolder>& out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    const std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CacheReader r{buf};
    // 開発用：RD_TIMING があれば、使えなかったわけを出す
    auto miss = [&](const std::string& why) {
        if (dev_env("RD_TIMING")) std::printf("      MOD のフォルダの計算を作り直す：%s\n", why.c_str());
        return false;
    };
    if (r.str() != kModCacheMagic) return miss("形が違う");
    if (r.u64() != key) return miss("渡す値が変わった");
    const auto hour = static_cast<int64_t>(r.u64());
    if (hour >= 0 && hour != current_hour()) return miss("時刻を見る MOD で、前に作ってから 1 時間の区切りを越えた");
    for (uint64_t i = 0, n = r.u64(); r.ok && i < n; ++i) {
        FileStamp was;
        was.path = r.str(), was.size = static_cast<int64_t>(r.u64()), was.mtime = static_cast<int64_t>(r.u64());
        const FileStamp now = stamp_of(was.path);
        if (now.size != was.size || now.mtime != was.mtime) return miss("読んだファイルが変わった：" + was.path);
    }
    std::vector<ModFolder> got(r.ok ? r.u64() : 0);
    for (ModFolder& x : got)
        if (!r.folder(x, 0)) return false;
    if (!r.ok) return false;
    for (ModFolder& x : got) out.push_back(std::move(x));
    return true;
}

void write_mod_cache(const std::string& path, uint64_t key, bool read_time, const std::vector<FileStamp>& deps,
                     const std::vector<ModFolder>& folders) {
    CacheWriter w;
    w.str(kModCacheMagic), w.u64(key), w.u64(static_cast<uint64_t>(read_time ? current_hour() : -1));
    w.u64(deps.size());
    for (const FileStamp& d : deps) w.str(d.path), w.u64(static_cast<uint64_t>(d.size)), w.u64(static_cast<uint64_t>(d.mtime));
    w.u64(folders.size());
    for (const ModFolder& x : folders) w.folder(x);
    // 書きかけを読まれないように、別の名前で書いてから置き換える（裏の 2 本が同時に書いても混ざらない）
    std::error_code ec;
    const fs::path dst = fs::u8path(path);
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary);
        if (!o.write(w.buf.data(), static_cast<std::streamsize>(w.buf.size()))) return;
    }
    fs::rename(tmp, dst, ec);
    if (dev_env("RD_TIMING")) std::printf("      MOD のフォルダの計算を残した：%s %zu B%s\n", path.c_str(), w.buf.size(), ec ? "（置けない）" : "");
    if (ec) fs::remove(tmp, ec);
}

}  // namespace

std::vector<ModFolder> build_mod_folders(const std::vector<const ModManifest*>& mods, const std::vector<ChartRecord>& charts,
                                         const std::unordered_map<std::string, ScoreRecord>& scores,
                                         const std::string& data_root, std::vector<std::string>* errors, const ModChartExtra& extra,
                                         const std::string& cache_dir) {
    std::vector<ModFolder> out;
    // 渡す値の指紋は、傾向を渡す MOD と渡さない MOD の 2 通りだけなので、それぞれ 1 回だけ取る
    uint64_t prints[2] = {};
    bool printed[2] = {};
    for (const ModManifest* m : mods) {
        if (m->script.empty() || !(m->touches & static_cast<uint32_t>(ModTouch::Control))) continue;
        const ModChartExtra& ex = m->chart_attr ? extra : ModChartExtra{};
        // 開発用：RD_TIMING があれば、MOD ごとに譜面を渡すまでと、MOD の処理の時間を出す
        auto t0 = std::chrono::steady_clock::now(), t1 = t0;
        // 渡す値が前と同じなら、残した計算を使う
        uint64_t key = 0;
        const std::string cache = cache_dir.empty() ? "" : cache_dir + "/" + m->id + ".cache";
        if (!cache.empty()) {
            const int v = m->chart_attr ? 1 : 0;
            if (!printed[v]) {
                Fingerprint fp;
                fp.integer("charts", static_cast<long long>(charts.size()));
                for (const ChartRecord& c : charts) visit_chart(fp, c, scores, ex);
                prints[v] = fp.h, printed[v] = true;
            }
            Fingerprint fp;
            fp.integer("values", static_cast<long long>(prints[v])), fp.str("mod", m->id);
            key = fp.h;
            if (read_mod_cache(cache, key, out)) {
                if (dev_env("RD_TIMING"))
                    std::printf("      MOD %s：残した計算を使った %.0f ms\n", m->id.c_str(),
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                continue;
            }
        }
        std::string err;
        auto s = open_script(*m, data_root, err);
        if (!s) {
            if (errors) errors->push_back(err);
            continue;
        }
        std::vector<ModFolder> got;
        bool ok = true;
        if (s->has("build_folders")) {
            s->memory_limit = kMemoryLimit + charts.size() * kMemoryPerChart;
            t1 = std::chrono::steady_clock::now();
            ok = s->run("build_folders", [&](lua_State* L) {
                lua_createtable(L, static_cast<int>(charts.size()), 0);
                lua_Integer i = 0;
                for (const ChartRecord& c : charts) {
                    push_chart(L, c, scores, ex);
                    lua_rawseti(L, -2, ++i);
                }
                t1 = std::chrono::steady_clock::now();
            }, kStepsFolders, [&](lua_State* L) {
                // 戻り値はフォルダの配列
                if (!lua_istable(L, -1)) return;
                lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
                for (lua_Integer k = 1; k <= n; ++k) {
                    if (lua_rawgeti(L, -1, k) == LUA_TTABLE) {
                        ModFolder f;
                        read_folder(L, -1, f, 0);
                        if (!f.title.empty()) got.push_back(std::move(f));
                    }
                    lua_pop(L, 1);
                }
            });
            if (dev_env("RD_TIMING"))
                std::printf("      MOD %s：譜面を渡す %.0f ms、MOD の処理 %.0f ms\n", m->id.c_str(),
                            std::chrono::duration<double, std::milli>(t1 - t0).count(),
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
        }
        if (!ok) {
            if (errors) errors->push_back(s->error());
            continue;
        }
        if (!cache.empty()) {
            // スクリプトそのものと、スクリプトが読んだファイル（設定・モデルなど）が変わったら作り直す
            std::vector<FileStamp> deps{stamp_of(utf8_of(fs::path(m->dir) / m->script))};
            for (const std::string& p : s->read_paths)
                if (std::none_of(deps.begin(), deps.end(), [&](const FileStamp& d) { return d.path == p; })) deps.push_back(stamp_of(p));
            write_mod_cache(cache, key, s->read_time, deps, got);
        }
        for (ModFolder& f : got) out.push_back(std::move(f));
    }
    return out;
}


std::vector<ModBar> mod_folder_bars(const std::vector<const ModManifest*>& mods, const std::string& folder, int charts,
                                    const std::string& data_root, std::vector<std::string>* errors) {
    std::vector<ModBar> out;
    for (const ModManifest* m : mods) {
        if (m->script.empty() || !(m->touches & static_cast<uint32_t>(ModTouch::Control))) continue;
        std::string err;
        auto s = open_script(*m, data_root, err);
        if (!s) {
            if (errors) errors->push_back(err);
            continue;
        }
        if (!s->has("folder_bars")) continue;
        bool ok = s->run("folder_bars", [&](lua_State* L) {
            // { title = フォルダの名前, charts = 中の譜面の数 }
            lua_createtable(L, 0, 2);
            set_str(L, "title", folder), set_int(L, "charts", charts);
        }, kStepsPerCall, [&](lua_State* L) {
            // 戻り値はバーの配列 { { key = "…", title = "…", roulette = true }, … }
            if (!lua_istable(L, -1)) return;
            lua_Integer n = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(L, -1)), 8);
            for (lua_Integer k = 1; k <= n; ++k) {
                if (lua_rawgeti(L, -1, k) == LUA_TTABLE) {
                    ModBar b;
                    b.mod_id = m->id;
                    if (lua_getfield(L, -1, "key") == LUA_TSTRING) b.key = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    if (lua_getfield(L, -1, "title") == LUA_TSTRING) b.title = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "roulette");
                    b.roulette = lua_toboolean(L, -1);
                    lua_pop(L, 1);
                    if (!b.title.empty()) out.push_back(std::move(b));
                }
                lua_pop(L, 1);
            }
        });
        if (!ok && errors) errors->push_back(s->error());
    }
    return out;
}

std::string mod_pick_chart(const ModManifest& m, const ModBar& bar, const std::string& folder,
                           const std::vector<const ChartRecord*>& charts,
                           const std::unordered_map<std::string, ScoreRecord>& scores, const std::string& data_root,
                           std::string* error, const std::string& shown) {
    std::string err, picked;
    auto s = open_script(m, data_root, err);
    if (!s) {
        if (error) *error = err;
        return {};
    }
    bool ok = s->run("pick_chart", [&](lua_State* L) {
        // { key = バーの key, folder = フォルダの名前, charts = { 譜面, … }, shown = ルーレットに出ていた譜面の sha256 }
        //（譜面は今の一覧に見えているもの。shown はルーレットのバーのときだけ）
        lua_createtable(L, 0, 4);
        set_str(L, "key", bar.key), set_str(L, "folder", folder);
        if (!shown.empty()) set_str(L, "shown", shown);
        lua_createtable(L, static_cast<int>(charts.size()), 0);
        lua_Integer i = 0;
        for (const ChartRecord* c : charts) {
            push_chart(L, *c, scores);
            lua_rawseti(L, -2, ++i);
        }
        lua_setfield(L, -2, "charts");
    }, kStepsFolders, [&](lua_State* L) {
        // 戻り値は遊ぶ譜面の sha256（nil なら何もしない）
        if (lua_type(L, -1) == LUA_TSTRING) picked = lua_tostring(L, -1);
    });
    if (!ok && error) *error = s->error().empty() ? m.id + ": pick_chart が無い" : s->error();
    return picked;
}

}  // namespace rd
