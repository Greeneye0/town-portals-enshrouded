// The easter egg (portal_easter.h): its hidden line, the 2% roll and the gag's timeline.
#include "../src/portal_protocol.h"
#include "../src/portal_easter.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
    using namespace portal;
    char line[96]{};
    const int n = FormatEasterEgg(line, sizeof(line), {1762530000123ull, 76561197993547076ull, 2});
    EasterEggMsg m{};
    CHECK(n > 0 && ParseEasterEgg({line, size_t(n)}, m));
    CHECK(m.portalId == 1762530000123ull && m.ownerAccount == 76561197993547076ull && m.place == 2);
    CHECK(!ParseEasterEgg("/tpx1 0 1 2", m));   // no portal
    CHECK(!ParseEasterEgg("/tpx1 5 1 99", m));  // no such place
    CHECK(!std::strcmp(easter::Name(text::Lang::de, 3), "Bruchtal"));
    CHECK(!ParseEasterEgg("/tpx1 5 1 2 7", m)); // extra field
    CHECK(!ParseEasterEgg("/tpo2 1 2 3", m));

    int hits = 0, counts[easter::kVariants]{};
    for (uint64_t i = 0; i < 200000; ++i) {
        const int r = easter::Roll(i * 7919 + 13, 1762530000000ull + i * 16, false);
        if (r >= 0) { ++hits; ++counts[r]; }
    }
    const double percent = hits * 100.0 / 200000;
    CHECK(percent > 1.8 && percent < 2.2);
    for (int c : counts) CHECK(c > hits / (easter::kVariants * 2));
    CHECK(easter::Roll(1, 2, true) >= 0);
    CHECK(easter::Roll(1, 2, true, 0) == -1); // no picture installed: never
    for (uint64_t i = 0; i < 1000; ++i) CHECK(easter::Roll(i, i * 3, true, 0b01010) % 2 == 1); // only places 1 and 3

    easter::Start(42, 1, 1000);
    easter::Start(42, 2, 3000); // a repeat of the same portal keeps the first
    int variant = -1;
    uint64_t t = 0;
    CHECK(easter::Playing(42, 4000, variant, t) && variant == 1 && t == 3000);
    CHECK(!std::strcmp(easter::LabelFor(42, 4000, text::Lang::de, "Echt"), "Sturmwind"));
    CHECK(!std::strcmp(easter::LabelFor(42, 1000 + easter::kEndMs, text::Lang::de, "Echt"), "Echt"));
    CHECK(!easter::Playing(42, 1000 + easter::kEndMs, variant, t));
    CHECK(!easter::Playing(7, 4000, variant, t));
    for (int v = 0; v < easter::kVariants; ++v)
        for (int l = 0; l < int(text::Lang::count); ++l) CHECK(*easter::Name(text::Lang(l), v));

    std::printf(failures ? "easter egg: %d failures\n" : "easter egg: all checks passed (roll %.2f%%)\n",
                failures ? double(failures) : percent);
    return failures ? 1 : 0;
}
