#include "util/log.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// windows.h のあとに読む
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "util/data_dir.h"
#include "util/version.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string g_name;
FILE* g_file = nullptr;  // ログのファイル
int g_console = -1;      // もとの標準出力（コンソールやパイプ。無ければ -1）
int g_pipe_read = -1;
std::thread g_reader;

#if defined(_WIN32)
int pid_now() { return static_cast<int>(GetCurrentProcessId()); }
#else
int pid_now() { return static_cast<int>(getpid()); }
#endif

std::string stamp(const char* fmt) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

// 行の頭に付ける時刻（時:分:秒.ミリ秒）
std::string line_time() {
    const auto now = std::chrono::system_clock::now();
    const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "[%s.%03d] ", stamp("%H:%M:%S").c_str(), ms);
    return buf;
}

// パイプから読んで、コンソールにはそのまま、ファイルには行ごとに時刻を付けて書く
void read_loop() {
    char buf[4096];
    bool line_start = true;
    for (;;) {
#if defined(_WIN32)
        const int n = _read(g_pipe_read, buf, sizeof(buf));
#else
        const int n = static_cast<int>(read(g_pipe_read, buf, sizeof(buf)));
#endif
        if (n <= 0) break;
        if (g_console >= 0) {
#if defined(_WIN32)
            (void)_write(g_console, buf, n);
#else
            (void)!write(g_console, buf, n);
#endif
        }
        for (int i = 0; i < n; ++i) {
            if (line_start) std::fputs(line_time().c_str(), g_file), line_start = false;
            std::fputc(buf[i], g_file);
            if (buf[i] == '\n') line_start = true;
        }
        std::fflush(g_file);
    }
}

// 終わるときに、書いたものを全部ファイルへ流してから閉じる
void finish_log() {
    std::fflush(stdout);
    std::fflush(stderr);
    // 標準出力と標準エラーをもとに戻すと、パイプの書く側が閉じて、読む側が終わる
#if defined(_WIN32)
    const int null_fd = _open("NUL", _O_WRONLY);
    if (g_console >= 0) _dup2(g_console, 1), _dup2(g_console, 2);
    else if (null_fd >= 0) _dup2(null_fd, 1), _dup2(null_fd, 2);
    if (null_fd >= 0) _close(null_fd);
#else
    const int null_fd = open("/dev/null", O_WRONLY);
    if (g_console >= 0) dup2(g_console, 1), dup2(g_console, 2);
    else if (null_fd >= 0) dup2(null_fd, 1), dup2(null_fd, 2);
    if (null_fd >= 0) close(null_fd);
#endif
    if (g_reader.joinable()) g_reader.join();
    if (g_file) std::fclose(g_file), g_file = nullptr;
}

#if defined(_WIN32)
// 落ちたとき：読む側が書き切るのを少し待ち、ダンプを残す
LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    std::fflush(stdout);
    std::fprintf(stderr, "落ちた（例外 0x%08lX）\n", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul);
    std::fflush(stderr);
    Sleep(300);
    const fs::path dmp = fs::u8path(log_dir()) / fs::u8path(fs::u8path(g_name).stem().string() + ".dmp");
    HANDLE f = CreateFileW(dmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION ex{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &ex, nullptr, nullptr);
        CloseHandle(f);
    }
    if (g_file) std::fflush(g_file);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void on_signal(int sig) {
    char msg[64];
    const int n = std::snprintf(msg, sizeof(msg), "落ちた（シグナル %d）\n", sig);
    (void)!write(2, msg, static_cast<size_t>(n));
    usleep(300 * 1000);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

// 古いログを消す（新しいほうから 60 個まで残す）
void trim_old_logs() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const char* sub : {"", "Sent"})
        for (const auto& e : fs::directory_iterator(fs::u8path(log_dir()) / sub, ec))
            if (e.is_regular_file(ec)) files.push_back(e.path());
    if (files.size() <= 60) return;
    // ファイル名の頭が日時なので、名前の順が古い順
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    for (size_t i = 0; i + 60 < files.size(); ++i) fs::remove(files[i], ec);
}

}  // namespace

bool output_is_redirected() {
#if defined(_WIN32)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return false;
    const DWORD t = GetFileType(out);
    return t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK;
#else
    return !isatty(1);
#endif
}

std::string log_dir() { return data_dir() + "Log/"; }

const std::string& current_log_name() { return g_name; }

bool log_owner_alive(const std::string& file_name) {
    // <日時>_<名前>_<プロセス番号>.txt
    const std::string stem = fs::u8path(file_name).stem().string();
    const size_t us = stem.rfind('_');
    if (us == std::string::npos) return false;
    const int pid = std::atoi(stem.c_str() + us + 1);
    if (pid <= 0) return false;
    if (file_name == g_name || pid == pid_now()) return true;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(pid, 0) == 0;
#endif
}

void start_log(const char* who) {
    if (g_file) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(log_dir()), ec);
    fs::create_directories(fs::u8path(log_dir()) / "Sent", ec);
    trim_old_logs();
    g_name = stamp("%Y%m%d_%H%M%S") + "_" + who + "_" + std::to_string(pid_now()) + ".txt";
#if defined(_WIN32)
    g_file = _wfopen((fs::u8path(log_dir()) / fs::u8path(g_name)).wstring().c_str(), L"wb");
#else
    g_file = std::fopen((log_dir() + g_name).c_str(), "wb");
#endif
    if (!g_file) return;
    std::fflush(stdout);
    std::fflush(stderr);
    int fds[2];
#if defined(_WIN32)
    // ダブルクリックで起動すると標準出力が無い。先に NUL に向けて番号 1・2 を作っておく
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool has_console = out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
    if (has_console && _fileno(stdout) >= 0) g_console = _dup(_fileno(stdout));
    FILE* dummy = nullptr;
    if (_fileno(stdout) < 0) freopen_s(&dummy, "NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen_s(&dummy, "NUL", "w", stderr);
    // 子のプロセスには渡さない
    if (_pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT) != 0) return;
    _dup2(fds[1], 1);
    _dup2(fds[1], 2);
    _close(fds[1]);
    SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
    SetUnhandledExceptionFilter(on_crash);
#else
    g_console = dup(1);
    if (pipe(fds) != 0) return;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);
    fcntl(g_console, F_SETFD, FD_CLOEXEC);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) signal(sig, on_signal);
#endif
    // ログのパイプへは行ごとに流す（Windows の CRT には行ごとの区切りが無いので、小さくまとめて、落ちたときにも流す）
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    // 止まったときにも直前の行が残るように、1 秒ごとに流す
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::fflush(stdout);
        }
    }).detach();
    g_pipe_read = fds[0];
    g_reader = std::thread(read_loop);
    std::atexit(finish_log);
    std::printf("BMjoy %s（%s）を起動 %s\n", kBmjVersion, who, stamp("%Y-%m-%d %H:%M:%S").c_str());
}

}  // namespace bmj
