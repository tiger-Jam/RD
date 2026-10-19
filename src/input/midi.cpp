#include "input/midi.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "input/input.h"

#if defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <map>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <thread>
#endif

namespace bmj {

void MidiDecoder::set(int source, bool down, std::vector<std::pair<int, bool>>& out) {
    if (down_[source] == down) return;
    down_[source] = down;
    out.push_back({source, down});
}

void MidiDecoder::feed(uint8_t status, uint8_t data1, uint8_t data2, std::vector<std::pair<int, bool>>& out) {
    if (status < 0x80 || status >= 0xf0) return;  // システムメッセージ（クロックなど）は使わない
    data1 &= 0x7f;
    data2 &= 0x7f;
    switch (status >> 4) {
    case 0x8: set(data1, false, out); break;
    case 0x9: set(data1, data2 != 0, out); break;
    case 0xb:
        // ダンパーペダル。ちょうど 64 のときは前のまま
        if (data1 != 64) break;
        if (data2 < 64) set(kMidiPedal, false, out);
        else if (data2 > 64) set(kMidiPedal, true, out);
        break;
    case 0xe:
        // ピッチベンドは上位 7 ビット（data2）だけを見る
        set(kMidiBendMinus, data2 < 64, out);
        set(kMidiBendPlus, data2 > 64, out);
        break;
    default: break;
    }
}

void MidiDecoder::release_all(std::vector<std::pair<int, bool>>& out) {
    for (int s = 0; s < kMidiSourceCount; ++s) set(s, false, out);
}

#if defined(__APPLE__) || defined(_WIN32)
namespace {

void send(const std::vector<std::pair<int, bool>>& changes, uint64_t ts) {
    for (auto [source, down] : changes) push_midi_event(source, down, ts);
}

}  // namespace
#endif

#if defined(__APPLE__)

struct MidiInput::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    mutable std::mutex mu;
    MidiDecoder decoder;
    std::map<SInt32, std::string> connected;  // 機器の固有番号 → 名前
    mach_timebase_info_data_t timebase{};

    // CoreMIDI の時刻（mach_absolute_time）を SDL_GetTicksNS() 基準に直す
    uint64_t to_ticks(MIDITimeStamp t) const {
        uint64_t now = SDL_GetTicksNS();
        uint64_t host = mach_absolute_time();
        if (t == 0 || t >= host) return now;
        uint64_t ago = (host - t) * timebase.numer / timebase.denom;
        return ago < now ? now - ago : 0;
    }

    // つながっている入力機器を全部開く。抜かれた機器があれば、押したままの入力を離す
    void connect_all() {
        std::lock_guard<std::mutex> lock(mu);
        std::map<SInt32, std::string> now;
        ItemCount n = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < n; ++i) {
            MIDIEndpointRef src = MIDIGetSource(i);
            if (!src) continue;
            SInt32 uid = 0, offline = 0;
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &uid);
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyOffline, &offline);
            if (offline) continue;
            auto it = connected.find(uid);
            if (it != connected.end()) {
                now[uid] = it->second;
                continue;
            }
            std::string name = "MIDI";
            CFStringRef cf = nullptr;
            if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &cf) == noErr && cf) {
                char buf[256];
                if (CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8)) name = buf;
                CFRelease(cf);
            }
            if (MIDIPortConnectSource(port, src, nullptr) == noErr) {
                std::printf("MIDI: %s を開いた\n", name.c_str());
                now[uid] = name;
            }
        }
        if (now.size() < connected.size() ||
            std::any_of(connected.begin(), connected.end(), [&](const auto& c) { return !now.count(c.first); })) {
            std::vector<std::pair<int, bool>> changes;
            decoder.release_all(changes);
            send(changes, SDL_GetTicksNS());
        }
        connected = std::move(now);
    }

    void receive(const MIDIEventList* list) {
        std::vector<std::pair<int, bool>> changes;
        std::lock_guard<std::mutex> lock(mu);
        const MIDIEventPacket* p = &list->packet[0];
        for (UInt32 k = 0; k < list->numPackets; ++k) {
            uint64_t ts = to_ticks(p->timeStamp);
            // UMP（32 ビットの語）。種類 2 が MIDI 1.0 のチャンネルメッセージ
            for (UInt32 i = 0; i < p->wordCount;) {
                uint32_t w = p->words[i];
                uint32_t mt = w >> 28;
                static const uint8_t kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
                if (mt == 2) {
                    changes.clear();
                    decoder.feed(static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w), changes);
                    send(changes, ts);
                }
                i += kWords[mt];
            }
            p = MIDIEventPacketNext(p);
        }
    }
};

void MidiInput::start() {
    if (impl_) return;
    Impl* im = new Impl;
    mach_timebase_info(&im->timebase);
    OSStatus st = MIDIClientCreateWithBlock(CFSTR("BMjoy"), &im->client, ^(const MIDINotification* n) {
        if (n->messageID == kMIDIMsgSetupChanged) im->connect_all();
    });
    if (st == noErr)
        st = MIDIInputPortCreateWithProtocol(im->client, CFSTR("BMjoy input"), kMIDIProtocol_1_0, &im->port,
                                             ^(const MIDIEventList* list, void*) { im->receive(list); });
    if (st != noErr) {
        std::printf("MIDI: 準備できない（%d）\n", static_cast<int>(st));
        if (im->client) MIDIClientDispose(im->client);
        delete im;
        return;
    }
    impl_ = im;
    im->connect_all();
}

void MidiInput::stop() {
    if (!impl_) return;
    if (impl_->port) MIDIPortDispose(impl_->port);
    if (impl_->client) MIDIClientDispose(impl_->client);
    delete impl_;
    impl_ = nullptr;
}

std::vector<std::string> MidiInput::devices() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lock(impl_->mu);
    for (auto& [uid, name] : impl_->connected) out.push_back(name);
    return out;
}

#elif defined(_WIN32)

struct MidiInput::Impl {
    std::thread thread;
    std::atomic<DWORD> thread_id{0};
    mutable std::mutex mu;
    MidiDecoder decoder;
    std::vector<HMIDIIN> handles;
    std::vector<std::string> names;
    UINT opened_count = 0;
    static constexpr UINT kMsgData = WM_APP + 1;

    // winmm の受け取り口。ここでは時刻を取ってスレッドへ送るだけにする（受け取り口で呼べる関数は限られているため）
    static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto self = reinterpret_cast<Impl*>(inst);
        WPARAM ts = 0;  // 32 ビットでは時刻が入らないので、スレッドで受け取った時刻を使う
        if constexpr (sizeof(WPARAM) >= 8) ts = static_cast<WPARAM>(SDL_GetTicksNS());
        PostThreadMessageW(self->thread_id, kMsgData, ts, static_cast<LPARAM>(p1));
    }

    void close_all() {
        for (HMIDIIN h : handles) midiInStop(h), midiInReset(h), midiInClose(h);
        handles.clear();
        std::lock_guard<std::mutex> lock(mu);
        names.clear();
        std::vector<std::pair<int, bool>> changes;
        decoder.release_all(changes);
        send(changes, SDL_GetTicksNS());
    }

    void open_all() {
        close_all();
        opened_count = midiInGetNumDevs();
        std::vector<std::string> opened;
        for (UINT i = 0; i < opened_count; ++i) {
            MIDIINCAPSW caps{};
            std::string name = "MIDI";
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                int n = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    name.assign(n - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), n, nullptr, nullptr);
                }
            }
            HMIDIIN h = nullptr;
            if (midiInOpen(&h, i, reinterpret_cast<DWORD_PTR>(&proc), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) !=
                MMSYSERR_NOERROR)
                continue;
            midiInStart(h);
            handles.push_back(h);
            opened.push_back(name);
            std::printf("MIDI: %s を開いた\n", name.c_str());
        }
        std::lock_guard<std::mutex> lock(mu);
        names = std::move(opened);
    }

    void run() {
        MSG m;
        PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // このスレッドのメッセージの列を作る
        // 入力の時刻の精度を上げるため、このスレッドの優先度を上げる
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        thread_id = GetCurrentThreadId();
        open_all();
        // winmm はつないだ・抜いたを知らせないので、2 秒ごとに機器の数を見て、変わっていたら開き直す
        UINT_PTR timer = SetTimer(nullptr, 0, 2000, nullptr);
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (m.message == kMsgData) {
                uint64_t ts = m.wParam ? static_cast<uint64_t>(m.wParam) : SDL_GetTicksNS();
                DWORD p1 = static_cast<DWORD>(m.lParam);
                std::vector<std::pair<int, bool>> changes;
                std::lock_guard<std::mutex> lock(mu);
                decoder.feed(static_cast<uint8_t>(p1), static_cast<uint8_t>(p1 >> 8), static_cast<uint8_t>(p1 >> 16), changes);
                send(changes, ts);
            } else if (m.message == WM_TIMER) {
                if (midiInGetNumDevs() != opened_count) open_all();
            }
        }
        KillTimer(nullptr, timer);
        close_all();
    }
};

void MidiInput::start() {
    if (impl_) return;
    impl_ = new Impl;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void MidiInput::stop() {
    if (!impl_) return;
    while (!impl_->thread_id) std::this_thread::yield();
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
    delete impl_;
    impl_ = nullptr;
}

std::vector<std::string> MidiInput::devices() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->names;
}

#else

struct MidiInput::Impl {};
void MidiInput::start() {}
void MidiInput::stop() {}
std::vector<std::string> MidiInput::devices() const { return {}; }

#endif

MidiInput& midi_input() {
    static MidiInput m;
    return m;
}

}  // namespace bmj
