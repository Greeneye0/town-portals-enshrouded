#include "../src/portal_snapshot_timing.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace portal::snapshot;

// A distant teleport evicts the old environment immediately. Model both the
// capture and restore frames separately so an early request cannot pass this.
bool Replay(int frameMs, int loadsAt, bool loadingDuringCapture = false) {
    DepartureGate gate; gate.Begin(100);
    CaptureSettle settle;
    bool sourceResident = true, active = false, image = false, restoring = false;
    uint64_t captureAt = 0, movedAt = 0, copiedAt = 0, restoredAt = 0;
    int submissions = 0;
    for (uint64_t now = 100; now < 12000; now += frameMs) {
        const bool areaReady = now >= uint64_t(loadsAt) &&
            !(loadingDuringCapture && captureAt && now < captureAt + 700);
        auto step = gate.Poll(now, areaReady, active || restoring, true);
        if (step == DepartureAction::capture) {
            assert(sourceResident && !active && !restoring && now >= 100 + kCoverMs);
            active = true; captureAt = now; settle = {};
        } else if (step == DepartureAction::cancelCapture) {
            active = false; restoring = true;
        } else if (step == DepartureAction::travel) {
            assert(!active && !restoring);
            if (image) assert(copiedAt && restoredAt > copiedAt && now > restoredAt);
            sourceResident = false; movedAt = now; ++submissions;
        }
        // Render after gameplay, as a separate stage. Busy cannot clear at the
        // START of the restore draw; the camera must finish rendering first.
        if (restoring) { restoring = false; restoredAt = now; }
        else if (active) {
            if (now - captureAt > kCaptureLimitMs) { active = false; restoring = true; }
            else if (settle.Frame(now, areaReady, now)) {
                assert(sourceResident);
                image = true; copiedAt = now; active = false; restoring = true;
            }
        }
    }
    assert(submissions == 1 && movedAt && !gate.Active());
    if (loadsAt > int(kDepartureLimitMs)) assert(!captureAt && !image);
    return image;
}

int main() {
    constexpr uint64_t gameplay = 1ull << 50, spawning = 1ull << 12;
    assert(!ActorReadyForSnapshot(0));
    assert(ActorReadyForSnapshot(gameplay));
    assert(!ActorReadyForSnapshot(gameplay | spawning));
    assert(!ActorReadyForSnapshot(EffectiveActorStates(gameplay, spawning, 0, true)));
    assert(ActorReadyForSnapshot(EffectiveActorStates(gameplay | spawning, 0, spawning, true)));
    assert(!ActorReadyForSnapshot(EffectiveActorStates(gameplay | spawning, 0, spawning, false)));
    assert(!ActorReadyForSnapshot(EffectiveActorStates(gameplay, 0, gameplay, true)));
    LoginSettle login;
    // World is loaded while spawning still runs: the login shot must not start.
    for (uint64_t now = 100; now < 5000; now += 100) assert(!login.Frame(now, false));
    assert(!login.Frame(5000, true));
    assert(!login.Frame(5000 + kLoginSettleMs - 1, true));
    assert(login.Frame(5000 + kLoginSettleMs, true));
    // A fresh loading/spawn transition or a reset invalidates the whole hold.
    assert(!login.Frame(9000, false));
    assert(!login.Frame(9100, true));
    assert(!login.Frame(9000, true)); // clock discontinuity
    login = {};
    assert(!login.Frame(20000, true));
    assert(login.Frame(20000 + kLoginSettleMs, true));
    for (int frameMs : {7, 16, 33, 100}) {
        assert(Replay(frameMs, 0));
        assert(Replay(frameMs, 1800));
        assert(Replay(frameMs, 0, true));
    }
    assert(!Replay(16, 9000)); // never publish an unready scene
    assert(!Replay(200, 0));  // timeout at low frame rate keeps the old picture

    CaptureSettle settle;
    for (uint64_t now = 0; now < 500; now += 16) settle.Frame(now, true, now);
    assert(!settle.Frame(500, false, 500)); // loading restarts the continuous hold
    for (uint64_t now = 516; now < 916; now += 16) assert(!settle.Frame(now, true, now));
    assert(settle.Frame(916, true, 916));
    assert(!settle.Frame(1600, true, 916)); // stale gameplay readiness is unusable
    assert(!settle.Frame(1700, true, 1800)); // reject a sample from the future

    DepartureGate gate; gate.Begin(100);
    assert(gate.Poll(100, true, false, true) == DepartureAction::wait);
    assert(gate.Poll(520, true, false, true) == DepartureAction::capture);
    assert(gate.Poll(6100, true, true, true) == DepartureAction::cancelCapture);
    for (uint64_t now : {6200, 7000, 8000})
        assert(gate.Poll(now, true, true, true) == DepartureAction::wait);
    assert(gate.Poll(8100, true, false, true) == DepartureAction::travel);
    assert(gate.Poll(8200, true, false, true) == DepartureAction::wait);
    // Failed/missing graphics capture still permits travel under the cover.
    gate.Begin(100);
    assert(gate.Poll(520, true, false, false) == DepartureAction::travel);
    gate.Begin(100); gate.Poll(100, true, false, true);
    assert(gate.Poll(520, true, false, true) == DepartureAction::capture);
    assert(gate.Poll(536, true, false, true) == DepartureAction::travel);
    // World exit/reset must discard deferred travel, even if it was ready.
    gate.Begin(100); gate = {};
    assert(gate.Poll(9000, true, false, true) == DepartureAction::wait);
    std::puts("PASS: login spawning/settle gate, distant-source capture before travel, readiness reset, timeouts, restore ordering, fallback and world exit");
}
