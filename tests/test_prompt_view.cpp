// The portal prompt claims E only when the player is turned to the portal (portal_prompt_view.h).
#include "../src/portal_prompt_view.h"
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
    using portal::promptview::Facing;
    // Portal 2.5 m north of the player (centre 0,2.5; right vector +x).
    CHECK(Facing(0, 0, 0, 1, 0, 2.5f, 1, 0));            // looking straight at it
    CHECK(Facing(0, 0, 0.6f, 0.8f, 0, 2.5f, 1, 0));       // ~37 degrees off
    CHECK(!Facing(0, 0, 1, 0, 0, 2.5f, 1, 0));            // looking east, at a chest beside it
    CHECK(!Facing(0, 0, 0, -1, 0, 2.5f, 1, 0));           // back turned
    CHECK(Facing(0, 0, 0.8f, 0.6f, 0, 2.5f, 1, 0));       // ~53 degrees off, but at its right edge
    CHECK(Facing(0, 0, 0, -1, 0, 0.6f, 1, 0));            // right at its face: always
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("portal prompt view: all checks passed\n");
    return 0;
}
