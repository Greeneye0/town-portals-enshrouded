// The game's language from its word for "Legendary" (portal_text.h Detect): Russian and
// Ukrainian in every form the game may use (a Russian game was read as Ukrainian, reported
// by aerospace on Nexus).
#include "../src/portal_text.h"
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static bool Is(const char* word, portal::text::Lang lang) {
    portal::text::Lang found = portal::text::Lang::count;
    return portal::text::Detect(word, found) && found == lang;
}

int main() {
    using portal::text::Lang;
    CHECK(Is("Legendary", Lang::en));
    CHECK(Is("Legendär", Lang::de));
    for (const char* ru : {"Легендарный", "Легендарное", "Легендарная", "Легендарные", "ЛЕГЕНДАРНОЕ"}) CHECK(Is(ru, Lang::ru));
    for (const char* uk : {"Легендарний", "Легендарна", "Легендарне", "Легендарні"}) CHECK(Is(uk, Lang::uk));
    CHECK(Is("传说", Lang::zhHans));
    Lang kept = Lang::de;
    CHECK(!portal::text::Detect("", kept) && kept == Lang::de);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("language detection: all checks passed\n");
    return 0;
}
