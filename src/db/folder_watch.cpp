#include "db/folder_watch.h"

#include <atomic>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// 譜面のファイルか（曲データベースが読むもの）
bool is_chart(const fs::path& p) {
    std::string e = lower_ext(p);
    return e == ".bms" || e == ".bme" || e == ".bml" || e == ".osu" || e == ".bmson";
}

}  // namespace

#if defined(_WIN32)

struct FolderWatcher::Impl {
    struct Root {
        std::wstring path;
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE stop_event = nullptr;
        std::thread th;
    };
    std::vector<std::unique_ptr<Root>> roots;
};

FolderWatcher::FolderWatcher() : impl_(std::make_unique<Impl>()) {}
FolderWatcher::~FolderWatcher() { stop(); }

void FolderWatcher::start(const std::vector<std::string>& roots) {
    stop();
    for (const std::string& r : roots) {
        auto root = std::make_unique<Impl::Root>();
        root->path = fs::path(r).wstring();
        root->dir = CreateFileW(root->path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (root->dir == INVALID_HANDLE_VALUE) continue;
        root->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Impl::Root* rp = root.get();
        root->th = std::thread([this, rp] {
            std::vector<BYTE> buf(64 * 1024);
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            for (;;) {
                ResetEvent(ov.hEvent);
                if (!ReadDirectoryChangesW(rp->dir, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                                           FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                               FILE_NOTIFY_CHANGE_SIZE,
                                           nullptr, &ov, nullptr))
                    break;
                HANDLE waits[2] = {ov.hEvent, rp->stop_event};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0) {
                    CancelIoEx(rp->dir, &ov);
                    break;
                }
                DWORD bytes = 0;
                if (!GetOverlappedResult(rp->dir, &ov, &bytes, FALSE)) break;
                if (bytes == 0) {
                    // 変化が多すぎて知らせがあふれた：曲フォルダ全体を読み直してもらう
                    notify(fs::path(rp->path).string(), true);
                    continue;
                }
                for (const BYTE* p = buf.data();;) {
                    auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                    std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    fs::path full = fs::path(rp->path) / name;
                    std::error_code ec;
                    bool dir = info->Action != FILE_ACTION_REMOVED && info->Action != FILE_ACTION_RENAMED_OLD_NAME &&
                               fs::is_directory(full, ec);
                    notify(full.string(), dir);
                    if (!info->NextEntryOffset) break;
                    p += info->NextEntryOffset;
                }
            }
            CloseHandle(ov.hEvent);
        });
        impl_->roots.push_back(std::move(root));
    }
}

void FolderWatcher::stop() {
    for (auto& r : impl_->roots) {
        if (r->stop_event) SetEvent(r->stop_event);
        if (r->th.joinable()) r->th.join();
        if (r->dir != INVALID_HANDLE_VALUE) CloseHandle(r->dir);
        if (r->stop_event) CloseHandle(r->stop_event);
    }
    impl_->roots.clear();
}

#elif defined(__APPLE__)

struct FolderWatcher::Impl {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
};

namespace {
void fsevents_callback(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId[]) {
    auto* self = static_cast<FolderWatcher*>(info);
    char** list = static_cast<char**>(paths);
    for (size_t i = 0; i < count; ++i) self->notify(list[i], (flags[i] & kFSEventStreamEventFlagItemIsDir) != 0);
}
}  // namespace

FolderWatcher::FolderWatcher() : impl_(std::make_unique<Impl>()) {}
FolderWatcher::~FolderWatcher() { stop(); }

void FolderWatcher::start(const std::vector<std::string>& roots) {
    stop();
    if (roots.empty()) return;
    CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const std::string& r : roots) {
        CFStringRef s = CFStringCreateWithCString(nullptr, r.c_str(), kCFStringEncodingUTF8);
        if (s) CFArrayAppendValue(arr, s), CFRelease(s);
    }
    FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
    impl_->stream = FSEventStreamCreate(nullptr, &fsevents_callback, &ctx, arr, kFSEventStreamEventIdSinceNow, 0.5,
                                        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(arr);
    if (!impl_->stream) return;
    impl_->queue = dispatch_queue_create("bmjoy.folderwatch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(impl_->stream, impl_->queue);
    FSEventStreamStart(impl_->stream);
}

void FolderWatcher::stop() {
    if (impl_->stream) {
        FSEventStreamStop(impl_->stream);
        FSEventStreamInvalidate(impl_->stream);
        FSEventStreamRelease(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->queue) dispatch_release(impl_->queue), impl_->queue = nullptr;
}

#else

struct FolderWatcher::Impl {};
FolderWatcher::FolderWatcher() : impl_(std::make_unique<Impl>()) {}
FolderWatcher::~FolderWatcher() { stop(); }
void FolderWatcher::start(const std::vector<std::string>&) {}
void FolderWatcher::stop() {}

#endif

void FolderWatcher::notify(const std::string& path, bool is_dir) {
    if (path.empty()) return;
    fs::path p(path);
    // 譜面のファイルならその入っているフォルダ、フォルダならそのフォルダ（増えた・消えた曲フォルダ）。
    // それ以外のファイル（キー音や画像）は譜面の一覧に関係しないので見ない
    std::string dir;
    if (is_dir) dir = path;
    else if (is_chart(p)) dir = p.parent_path().string();
    else return;
    if (dir.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    changed_.insert(dir);
    last_ = std::chrono::steady_clock::now();
}

std::vector<std::string> FolderWatcher::take_changed(std::chrono::milliseconds quiet) {
    std::lock_guard<std::mutex> lock(mu_);
    if (changed_.empty() || std::chrono::steady_clock::now() - last_ < quiet) return {};
    std::vector<std::string> out(changed_.begin(), changed_.end());
    changed_.clear();
    return out;
}

}  // namespace bmj
