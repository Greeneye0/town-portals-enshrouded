#pragma once
// Which player in a world this machine hosts is its own. Every player's tick passes through the
// host, and tick timing alone can't tell them apart: a friend who ticks first after a reload, or
// through a stall of the host's own character, looks like the only player (juli5556, 2026-11-01:
// the host's portals stopped working, with the host's trips going to the friend's character).
// So timing is only the first guess, and where the players stand decides: the UI knows where this
// machine's player is, and the world knows where each entity is.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace portal::travel {

struct PlayerSpot {
    uint32_t entity = 0;
    float wx = 0.0f;
    float wz = 0.0f;
};

class HostIdentity {
public:
    static constexpr float kMatchMeters = 3.0f;   // the UI's position and the world's agree this closely
    static constexpr float kClearMeters = 2.0f;   // the next player must be this much further away
    static constexpr uint64_t kHoldMs = 1000;     // one answer for this long, so a passer-by can't take it

    uint32_t Current() const { return current_; }
    bool Confirmed() const { return confirmed_; }

    void Reset() { *this = HostIdentity{}; }

    // A player's tick: alone means no other player ticked lately. Only a guess until confirmed.
    void OnTick(uint32_t id, bool alone, uint32_t localHint) {
        if (confirmed_) return;
        if (alone) current_ = id;
        else if (!current_ && id == localHint) current_ = id;
    }

    // Can a request from this machine's player run on this entity's tick?
    bool MayBeOwn(uint32_t id, bool alone) const {
        return id == current_ || (alone && !confirmed_);
    }

    // The UI's position against the players the world knows. True when the answer changed.
    bool Observe(float wx, float wz, const PlayerSpot* spots, size_t count, uint64_t now) {
        const uint32_t match = Match(wx, wz, spots, count);
        if (!match) {
            candidate_ = 0; // two players side by side, or no position: no evidence either way
            return false;
        }
        if (match != candidate_) {
            candidate_ = match;
            since_ = now;
        }
        if (now - since_ < kHoldMs) return false;
        const bool changed = current_ != match;
        current_ = match;
        confirmed_ = true;
        return changed;
    }

private:
    static uint32_t Match(float wx, float wz, const PlayerSpot* spots, size_t count) {
        uint32_t best = 0;
        float bestDistance = 0.0f, nextDistance = 0.0f;
        bool haveNext = false;
        for (size_t i = 0; i < count; ++i) {
            if (!spots[i].entity) continue;
            const float distance = std::hypot(spots[i].wx - wx, spots[i].wz - wz);
            if (!best || distance < bestDistance) {
                if (best) {
                    nextDistance = bestDistance;
                    haveNext = true;
                }
                best = spots[i].entity;
                bestDistance = distance;
            } else if (!haveNext || distance < nextDistance) {
                nextDistance = distance;
                haveNext = true;
            }
        }
        if (!best || bestDistance > kMatchMeters) return 0;
        if (haveNext && nextDistance < bestDistance + kClearMeters) return 0;
        return best;
    }

    uint32_t current_ = 0;
    uint32_t candidate_ = 0;
    uint64_t since_ = 0;
    bool confirmed_ = false;
};

} // namespace portal::travel
