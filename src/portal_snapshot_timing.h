#pragma once
#include <cstdint>

namespace portal::snapshot {
// These waits coordinate the gameplay and render threads. In particular, a
// teleport must not evict the departure scene before its picture is recorded.
constexpr uint64_t kCoverMs = 420, kReadyMs = 400, kDepartureLimitMs = 6000;
constexpr uint64_t kCaptureLimitMs = 3000, kReadinessMaxAgeMs = 500;
constexpr uint32_t kCaptureFrames = 24;

// Login can finish terrain streaming while the spawning sequence/its trailing
// particles are still visible. Require normal gameplay and a continuous quiet
// interval before borrowing the camera. This is a bounded settling margin, not
// a measurement of the lifetime of every VFX particle.
constexpr uint64_t kLoginSettleMs = 3000, kLoginLimitMs = 12000;
struct LoginSettle {
    uint64_t since{};
    bool observing{};
    bool Frame(uint64_t now, bool ready) {
        if (!ready || (observing && now < since)) { *this = {}; return false; }
        if (!observing) { since = now; observing = true; }
        return now - since >= kLoginSettleMs;
    }
};

// Same effective actor-state calculation as native FUN_14021aaa0 (1273043).
constexpr uint64_t EffectiveActorStates(uint64_t states, uint64_t added,
                                         uint64_t removed, bool pending) {
    return pending ? (states | added) & ~removed : states;
}
constexpr bool ActorReadyForSnapshot(uint64_t states) {
    return !(states & (1ull << 12)) && (states & (1ull << 50)); // Spawning / IsReadyForGameplay
}

struct CaptureSettle {
    uint64_t since{};
    uint32_t frames{};
    bool Frame(uint64_t now, bool ready, uint64_t observedAt) {
        if (!ready || now < observedAt || now - observedAt > kReadinessMaxAgeMs) {
            since = 0; frames = 0; return false;
        }
        if (!frames++) since = now;
        return frames >= kCaptureFrames && now - since >= kReadyMs;
    }
};

enum class DepartureAction { wait, capture, cancelCapture, travel };
struct DepartureGate {
    enum class Phase { idle, preparing, capturing, restoring } phase = Phase::idle;
    uint64_t started{}, readySince{};
    bool readySeen{};
    bool Active() const { return phase != Phase::idle; }
    void Begin(uint64_t now) { *this = {}; phase = Phase::preparing; started = now; }
    DepartureAction Poll(uint64_t now, bool areaReady, bool busy, bool available) {
        if (!Active()) return DepartureAction::wait;
        if (phase == Phase::capturing || phase == Phase::restoring) {
            // Busy includes the camera cut back, not just the image copy.
            if (!busy) { phase = Phase::idle; return DepartureAction::travel; }
            if (phase == Phase::capturing && now - started >= kDepartureLimitMs) {
                phase = Phase::restoring;
                return DepartureAction::cancelCapture;
            }
            return DepartureAction::wait;
        }
        if (!areaReady) readySeen = false;
        else if (!readySeen) { readySeen = true; readySince = now; }
        if (now - started >= kDepartureLimitMs) {
            if (busy) { phase = Phase::restoring; return DepartureAction::cancelCapture; }
            phase = Phase::idle;
            return DepartureAction::travel; // keep the previous image; never capture an unready scene
        }
        if (busy || now - started < kCoverMs) return DepartureAction::wait;
        if (!available) { phase = Phase::idle; return DepartureAction::travel; }
        if (!readySeen || now - readySince < kReadyMs) return DepartureAction::wait;
        phase = Phase::capturing;
        return DepartureAction::capture;
    }
};
}
