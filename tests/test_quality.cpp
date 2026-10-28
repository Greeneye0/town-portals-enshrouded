// Lighter portals from the game's graphics settings (portal_quality.h LowFromGameSettings).
#include "../src/portal_quality.h"
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static std::string Render(const char* preset, const char* shadows, const char* gi, const char* reflections, const char* fog) {
    char text[512];
    std::snprintf(text, sizeof(text),
        "{\"graphics\": {\"render\": {\"preset\": \"%s\", \"antiAliasing\": \"Dlaa\", \"shadows\": \"%s\", "
        "\"dynamicGi\": \"%s\", \"reflections\": \"%s\", \"water\": \"Ultra\", \"fog\": \"%s\"}}}",
        preset, shadows, gi, reflections, fog);
    return text;
}

int main() {
    using portal::quality::LowFromGameSettings;
    CHECK(!LowFromGameSettings(Render("Ultra", "Ultra", "Ultra", "Ultra", "Ultra")));
    CHECK(!LowFromGameSettings(Render("High", "Low", "Low", "Low", "Low")));  // a preset decides
    CHECK(LowFromGameSettings(Render("Low", "Ultra", "Ultra", "Ultra", "Ultra")));
    CHECK(LowFromGameSettings(Render("Medium", "High", "High", "High", "High")));
    CHECK(LowFromGameSettings(Render("Custom", "Low", "Medium", "Ultra", "Ultra")));  // two lowered
    CHECK(!LowFromGameSettings(Render("Custom", "Low", "Ultra", "Ultra", "Ultra"))); // one lowered
    CHECK(LowFromGameSettings(Render("Lowest", "Low", "Performance", "Performance", "Low"))); // the game's own names
    CHECK(LowFromGameSettings(Render("Custom", "Ultra", "Performance", "Performance", "Ultra")));
    CHECK(!LowFromGameSettings("{}"));
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("portal quality: all checks passed\n");
    return 0;
}
