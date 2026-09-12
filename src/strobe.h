// Time-multiplexes the keys Synthesia asks to light so the keyboard never has more than
// MaxLit on at once.
//
// Blocks pattern: the requested keys are sorted by pitch and split into adjacent blocks of
// up to MaxLit, one block shown per turn. Over the limit this is the brightest overflow,
// since it keeps every lamp busy and cycles in the fewest turns. Under the limit nothing
// rotates and the keys simply stay lit.
//
// Pure logic: keys are opaque ints and output goes through the sendLight callback, so the
// self-check at the bottom runs without a keyboard.
#pragma once
#include <algorithm>
#include <functional>
#include <vector>

class Strobe {
public:
    explicit Strobe(std::function<void(int, bool)> sendLight, int maxLit = 4)
        : send_(std::move(sendLight)), maxLit_(maxLit) {}

    // Synthesia wants this key lit, or no longer lit.
    void Light(int key, bool on) {
        auto it = std::find(lit_.begin(), lit_.end(), key);
        if (on) {
            if (it == lit_.end()) lit_.push_back(key);
        } else if (it != lit_.end()) {
            lit_.erase(it);
        }
        Refresh(false);
    }

    void Clear() {
        lit_.clear();
        Refresh(false);
    }

    // True while any key is being driven, so the caller knows to keep the light display alive.
    bool HasLit() const { return !lit_.empty(); }

    // True only when more keys are wanted than the keyboard can show, which is the only time
    // anything actually flashes.
    bool Strobing() const { return maxLit_ > 0 && (int)lit_.size() > maxLit_; }

    // Turns in a full cycle. Every key is lit exactly once per cycle.
    int Groups() const {
        int n = (int)lit_.size();
        if (maxLit_ <= 0 || n <= maxLit_) return 1;
        return (n + maxLit_ - 1) / maxLit_;
    }

    // Show the current turn, or the next one when advance is set.
    void Refresh(bool advance) {
        std::vector<int> keys(lit_.begin(), lit_.end());
        std::sort(keys.begin(), keys.end());                   // low -> high

        std::vector<int> want;
        if (!keys.empty() && maxLit_ > 0) {
            if (!Strobing()) {
                want = keys;                                   // they all fit: nothing to rotate
            } else {
                int groups = Groups();
                if (advance) ++group_;
                group_ %= groups;
                int n = (int)keys.size(), size = n / groups, extra = n % groups;
                // the first `extra` blocks take one more, so sizes never differ by more than one
                int start = group_ * size + (group_ < extra ? group_ : extra);
                int count = size + (group_ < extra ? 1 : 0);
                want.assign(keys.begin() + start, keys.begin() + start + count);
            }
        }

        // Offs first, so the keyboard never momentarily sees more than MaxLit on.
        for (size_t i = 0; i < shown_.size();) {
            if (std::find(want.begin(), want.end(), shown_[i]) == want.end()) {
                send_(shown_[i], false);
                shown_.erase(shown_.begin() + i);
            } else {
                ++i;
            }
        }
        for (int k : want) {
            if (std::find(shown_.begin(), shown_.end(), k) == shown_.end()) {
                send_(k, true);
                shown_.push_back(k);
            }
        }
    }

    // The one runnable check: returns false if the logic breaks.
    static bool SelfTest();

private:
    std::function<void(int, bool)> send_;
    int maxLit_;
    std::vector<int> lit_;      // keys Synthesia wants on, in arrival order
    std::vector<int> shown_;    // keys the keyboard currently has on
    int group_ = 0;             // which turn of the cycle is showing
};

// ---------------------------------------------------------------------------------------
// Self-check. Header-only so the test target is a single translation unit.
// ---------------------------------------------------------------------------------------
inline bool Strobe::SelfTest() {
    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            fprintf(stderr, "strobe selftest FAILED: %s\n", what);
        }
    };

    // Runs n keys numbered 0..n-1 and returns what is lit on each of `turns` turns.
    auto frames = [](int n, int turns, int maxLit) {
        std::vector<int> on;
        Strobe s([&on](int k, bool o) {
            auto it = std::find(on.begin(), on.end(), k);
            if (o) { if (it == on.end()) on.push_back(k); }
            else if (it != on.end()) on.erase(it);
        }, maxLit);
        for (int i = 0; i < n; i++) s.Light(i, true);
        std::vector<std::vector<int>> out;
        for (int i = 0; i < turns; i++) {
            std::vector<int> f = on;
            std::sort(f.begin(), f.end());
            out.push_back(f);
            s.Refresh(true);
        }
        return out;
    };

    // Under the limit nothing rotates.
    for (auto& f : frames(3, 3, 4))
        check(f == std::vector<int>({0, 1, 2}), "under the limit the keys just stay lit");
    for (auto& f : frames(4, 3, 4))
        check(f == std::vector<int>({0, 1, 2, 3}), "exactly at the limit still does not flash");

    // Over the limit: 10 keys over 4 lamps splits 4/3/3.
    auto over = frames(10, 3, 4);
    check(over.size() == 3, "10 keys over 4 lamps is three turns");
    check(over[0].size() == 4 && over[1].size() == 3 && over[2].size() == 3,
          "10 keys split 4/3/3, never over MaxLit");
    std::vector<int> all;
    for (auto& f : over) all.insert(all.end(), f.begin(), f.end());
    std::sort(all.begin(), all.end());
    check(all == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}),
          "every key gets exactly one turn per cycle");
    for (auto& f : over)
        check(!f.empty() && f.back() - f.front() == (int)f.size() - 1,
              "each block is keys next to each other, not a scattered set");

    // The cycle repeats.
    auto twice = frames(10, 6, 4);
    check(twice[0] == twice[3] && twice[1] == twice[4] && twice[2] == twice[5],
          "the cycle repeats every Groups turns");

    // The invariant the keyboard cares about, across a full run including removals.
    {
        std::vector<std::pair<int, bool>> sent;
        Strobe s([&sent](int k, bool o) { sent.push_back({k, o}); }, 4);
        for (int n = 60; n < 69; n++) s.Light(n, true);
        for (int i = 0; i < 12; i++) s.Refresh(true);
        for (int n = 60; n < 69; n++) s.Light(n, false);
        check(!s.HasLit(), "no keys left requested at the end");

        std::vector<int> on;
        for (auto& [k, o] : sent) {
            auto it = std::find(on.begin(), on.end(), k);
            if (o) { if (it == on.end()) on.push_back(k); }
            else if (it != on.end()) on.erase(it);
            check((int)on.size() <= 4, "the keyboard never sees more than four on");
        }
        check(on.empty(), "everything is turned off at the end");
    }

    // A key removed mid-cycle is turned off and stops taking a turn.
    {
        std::vector<int> on;
        Strobe s([&on](int k, bool o) {
            auto it = std::find(on.begin(), on.end(), k);
            if (o) { if (it == on.end()) on.push_back(k); }
            else if (it != on.end()) on.erase(it);
        }, 4);
        for (int i = 0; i < 8; i++) s.Light(i, true);
        s.Light(0, false);
        for (int i = 0; i < 8; i++) {
            check(std::find(on.begin(), on.end(), 0) == on.end(), "a released key never lights again");
            s.Refresh(true);
        }
    }

    // Shrinking below the limit stops the rotation and lights them all.
    {
        std::vector<int> on;
        Strobe s([&on](int k, bool o) {
            auto it = std::find(on.begin(), on.end(), k);
            if (o) { if (it == on.end()) on.push_back(k); }
            else if (it != on.end()) on.erase(it);
        }, 4);
        for (int i = 0; i < 9; i++) s.Light(i, true);
        check(s.Strobing(), "nine keys over four lamps is a strobe");
        for (int i = 0; i < 6; i++) s.Light(i, false);
        check(!s.Strobing(), "three keys left is no longer a strobe");
        std::sort(on.begin(), on.end());
        check(on == std::vector<int>({6, 7, 8}), "and the three that remain are simply lit");
    }

    return ok;
}
