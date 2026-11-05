#include <cstdio>
#include "../src/portal_host_identity.h"

using portal::travel::HostIdentity;
using portal::travel::PlayerSpot;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

// The host (entity 2) stands at 1000,1000, the friend (entity 1) far away.
const PlayerSpot kApart[]{{1, 3000.0f, 3000.0f}, {2, 1000.0f, 1000.0f}};

void Observe(HostIdentity& h, uint64_t from, uint64_t to, const PlayerSpot* spots, size_t count) {
    for (uint64_t t = from; t <= to; t += 100) h.Observe(1000.0f, 1000.0f, spots, count, t);
}

} // namespace

int main() {
    {
        // After a reload the friend's character ticks first: the old rule stayed on the friend.
        HostIdentity h;
        h.OnTick(1, true, 2);
        Check(h.Current() == 1, "friend ticking first is the first guess");
        Observe(h, 0, 900, kApart, 2);
        Check(h.Current() == 1 && !h.Confirmed(), "the guess stands until the hold is over");
        Check(h.Observe(1000.0f, 1000.0f, kApart, 2, 1000), "the position check reports the change");
        Check(h.Current() == 2 && h.Confirmed(), "the position check moves to the host");
    }
    {
        // A stall of the host's character after it is known: the friend ticks alone for a moment.
        HostIdentity h;
        h.OnTick(2, true, 2);
        Observe(h, 0, 1000, kApart, 2);
        Check(h.Current() == 2 && h.Confirmed(), "the host is confirmed");
        h.OnTick(1, true, 2);
        Check(h.Current() == 2, "a stall can't move a confirmed host onto the friend");
        Check(!h.MayBeOwn(1, true), "the host's request can't run on the friend's lone tick");
        Check(h.MayBeOwn(2, false), "the host's request runs on the host's tick");
    }
    {
        // Before any confirmation the old guess still lets a lone tick take a request.
        HostIdentity h;
        Check(h.MayBeOwn(1, true), "unconfirmed, a lone tick may take the request");
        Check(!h.MayBeOwn(1, false), "unconfirmed, a shared tick that isn't the guess may not");
    }
    {
        // Hint only fills an empty answer.
        HostIdentity h;
        h.OnTick(1, false, 2);
        Check(h.Current() == 0, "another player's shared tick sets nothing");
        h.OnTick(2, false, 2);
        Check(h.Current() == 2, "the game's own id fills an empty answer");
        h.OnTick(1, false, 2);
        Check(h.Current() == 2, "a shared tick leaves a guess alone");
    }
    {
        // Side by side nothing can be told: the current answer stays.
        const PlayerSpot close[]{{1, 1001.0f, 1000.0f}, {2, 1000.0f, 1000.0f}};
        HostIdentity h;
        h.OnTick(1, true, 2);
        Observe(h, 0, 5000, close, 2);
        Check(h.Current() == 1 && !h.Confirmed(), "side by side gives no evidence");
        // They separate: it settles only after a full hold.
        h.Observe(1000.0f, 1000.0f, kApart, 2, 5100);
        Check(h.Current() == 1, "the hold starts over when they separate");
        h.Observe(1000.0f, 1000.0f, kApart, 2, 6100);
        Check(h.Current() == 2, "and then settles on the host");
    }
    {
        // A wrong answer can still be corrected after a confirmation (a changed entity id).
        const PlayerSpot moved[]{{3, 1000.0f, 1000.0f}, {1, 3000.0f, 3000.0f}};
        HostIdentity h;
        Observe(h, 0, 1000, kApart, 2);
        Observe(h, 1100, 2100, moved, 2);
        Check(h.Current() == 3, "a confirmed answer follows the position to a new id");
    }
    {
        // The evidence must be continuous: a friend passing through doesn't get it.
        const PlayerSpot none[]{{1, 3000.0f, 3000.0f}, {2, 2000.0f, 2000.0f}};
        HostIdentity h;
        h.OnTick(1, true, 2);
        h.Observe(1000.0f, 1000.0f, kApart, 2, 0);
        h.Observe(1000.0f, 1000.0f, none, 2, 500);
        h.Observe(1000.0f, 1000.0f, kApart, 2, 600);
        h.Observe(1000.0f, 1000.0f, kApart, 2, 1500);
        Check(h.Current() == 1, "a gap in the evidence restarts the hold");
    }
    {
        // Nobody near the UI's position (a loading screen, a stale position): nothing changes.
        HostIdentity h;
        h.OnTick(1, true, 2);
        for (uint64_t t = 0; t <= 5000; t += 100) h.Observe(500.0f, 500.0f, kApart, 2, t);
        Check(h.Current() == 1 && !h.Confirmed(), "no player near the position gives no answer");
    }
    {
        // Alone in the world, the one entity near the position is confirmed.
        const PlayerSpot solo[]{{1, 1000.5f, 1000.0f}};
        HostIdentity h;
        h.OnTick(1, true, 1);
        Observe(h, 0, 1000, solo, 1);
        Check(h.Current() == 1 && h.Confirmed(), "alone, the player is confirmed");
    }
    {
        // Leaving the world starts over.
        HostIdentity h;
        Observe(h, 0, 1000, kApart, 2);
        h.Reset();
        Check(h.Current() == 0 && !h.Confirmed(), "reset forgets the answer");
        h.OnTick(1, true, 1);
        Check(h.Current() == 1, "and the next world guesses again");
    }
    {
        // Empty slots are skipped, and the nearest-first order doesn't matter.
        const PlayerSpot gaps[]{{0, 1000.0f, 1000.0f}, {2, 1000.0f, 1000.0f}, {1, 3000.0f, 3000.0f}};
        HostIdentity h;
        Observe(h, 0, 1000, gaps, 3);
        Check(h.Current() == 2, "empty slots are skipped");
    }
    if (g_failed) std::printf("%d failed\n", g_failed);
    else std::printf("Host identity tests passed\n");
    return g_failed ? 1 : 0;
}
