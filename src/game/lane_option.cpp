#include "game/lane_option.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

namespace bmj {

namespace {

constexpr int kFirstKey = 1;

// 鍵盤ごとの並び替えの表（MIRROR / RANDOM）。table[元のレーン] = 新しいレーン。last_key は譜面の鍵盤数（5 か 7）
std::array<int, kLaneCount> lane_table(int option, int last_key, Lr2Random& rng) {
    std::array<int, kLaneCount> t{};
    std::iota(t.begin(), t.end(), 0);
    if (option == kMirror) {
        for (int k = kFirstKey; k <= last_key; ++k) t[k] = kFirstKey + last_key - k;
    } else if (option == kRandom) {
        // 前から順に、残りの中から1つを選んで入れ替える
        for (int k = kFirstKey; k < last_key; ++k) std::swap(t[k], t[k + rng.get_rand(last_key - k)]);
    }
    return t;
}

// ノーツ1つずつ鍵盤を選び直す（S-RANDOM / SCATTER / CONVERGE）。
// 同じ時刻にすでに置いた鍵盤は避け（全部埋まっていたらあきらめて重ねる）、LN の途中や同じ時刻のノーツがある鍵盤には置かない。
// SCATTER は、直前の時刻に使った鍵盤も「埋まっている」とみなす（同じ鍵盤の連打が来ない）。
// CONVERGE は、まず皿に置けるか試す
void shuffle_each(std::vector<Note>& notes, int option, int last_key, Lr2Random& rng) {
    struct Last {
        bool any = false;
        int64_t time = 0, end = -1;  // 最後に置いたノーツの時刻と、LN なら終点
    };
    std::array<Last, kLaneCount> last{};
    std::array<bool, kLaneCount> taken{}, recent{};
    int64_t now = INT64_MIN;
    for (Note& n : notes) {
        if (n.kind == NoteKind::Invisible) continue;
        if (n.time_us > now) {
            now = n.time_us;
            for (int l = 0; l < kLaneCount; ++l) {
                taken[l] = option == kScatter && recent[l];
                if (!last[l].any || last[l].end < n.time_us) recent[l] = false;
            }
        }
        if (n.lane >= kFirstKey && n.lane <= last_key) {
            bool converge = option == kConverge;
            int lane = n.lane;
            // 置ける鍵盤が無いときに止まらないよう、試す回数に上限を付ける
            for (int tries = 0; tries < 1000; ++tries) {
                lane = kFirstKey + rng.get_rand(last_key - kFirstKey);
                if (converge) lane = 0, converge = false;
                const Last& p = last[lane];
                if (p.any && (n.time_us <= p.end || n.time_us == p.time)) continue;
                if (!taken[lane]) break;
                bool full = true;
                for (int l = kFirstKey; l <= last_key; ++l) full = full && taken[l];
                if (full) break;
            }
            taken[lane] = true;
            n.lane = static_cast<uint8_t>(lane);
        }
        recent[n.lane] = true;
        last[n.lane] = {true, n.time_us, n.kind == NoteKind::Long ? n.end_us : -1};
    }
}

// 並べ替えで重なったノーツを捨てる（同じ時刻は先のものを残す。ただし LN の始点は普通のノーツを置き換える。LN の途中に始まるものは捨てる）
void drop_overlaps(std::vector<Note>& notes) {
    std::vector<Note> out;
    out.reserve(notes.size());
    std::array<int, kLaneCount> last{};
    last.fill(-1);
    for (const Note& n : notes) {
        if (n.kind == NoteKind::Invisible) {
            out.push_back(n);
            continue;
        }
        int& li = last[n.lane];
        if (li >= 0) {
            const Note& p = out[li];
            if (p.time_us == n.time_us) {
                if (n.kind == NoteKind::Long && p.kind != NoteKind::Long) out[li] = n;
                continue;
            }
            if (p.kind == NoteKind::Long && n.time_us <= p.end_us) continue;
        }
        li = static_cast<int>(out.size());
        out.push_back(n);
    }
    notes = std::move(out);
}

}  // namespace

int key_count(const Chart& chart) {
    for (const Note& n : chart.notes)
        if ((n.lane == 6 || n.lane == 7) && n.kind != NoteKind::Mine) return 7;
    return 5;
}

void apply_random_option(Chart& chart, int option, Lr2Random& rng) {
    if (option <= kRandomOff || option > kConverge) return;
    std::vector<Note>& notes = chart.notes;
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.time_us != b.time_us ? a.time_us < b.time_us : a.lane < b.lane;
    });
    const int last_key = key_count(chart);
    std::array<int, kLaneCount> table = lane_table(option, last_key, rng);
    if (option >= kSRandom) shuffle_each(notes, option, last_key, rng);
    for (Note& n : notes)
        if (n.kind != NoteKind::Invisible) n.lane = static_cast<uint8_t>(table[n.lane]);
    drop_overlaps(notes);
}

}  // namespace bmj
