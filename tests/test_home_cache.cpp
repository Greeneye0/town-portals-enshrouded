// Home snapshot cache file: R11G11B10 colour + half depth round trip (depth exact, colour
// within the small floats' precision), and rejection of files for another account or
// altar, truncated or damaged files.
#include "../src/portal_snapshot_cache.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace portal;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static float HalfValue(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e ? std::ldexp(1.f + m / 1024.f, e - 15) : std::ldexp(m / 1024.f, -14);
    return (h & 0x8000) ? -v : v;
}

int main() {
    // The conversion itself: exact for values the small floats hold, clamped at the top,
    // negative colour to 0.
    CHECK(homecache::HalfToSmall(0x3c00, 4) == 0x3c0 && homecache::SmallToHalf(0x3c0, 4) == 0x3c00); // 1.0
    CHECK(homecache::HalfToSmall(0x7bff, 4) == 0x7bf && homecache::HalfToSmall(0x7bff, 5) == 0x3df);  // largest
    CHECK(homecache::HalfToSmall(0xbc00, 4) == 0);                                                    // -1.0
    CHECK(homecache::HalfToSmall(0x3c08, 4) == 0x3c1);                                                // rounds up

    sharing::Image image{};
    image.meta.x = 3715.f; image.meta.y = 845.f; image.meta.z = 1418.f;
    image.meta.nx = 0.6f; image.meta.nz = 0.8f;
    image.meta.cx = 3716.f; image.meta.cy = 846.6f; image.meta.cz = 1420.f;
    image.meta.exposure = 0.25f; image.meta.clip[8] = 1; image.meta.codec = 1;
    image.pixels.resize(sharing::kRawBytes);
    uint32_t seed = 12345;
    for (size_t i = 0; i < image.pixels.size(); i += 8) {
        seed = seed * 1664525u + 1013904223u;
        // A smooth gradient with some noise, like a picture; depth grows across the image.
        const size_t p = i / 8, x = p % sharing::kWidth, y = p / sharing::kWidth;
        const uint16_t px[4] = {uint16_t(0x3000 + (x >> 2) + (seed & 7)), uint16_t(0x3400 + (y >> 2)),
                                uint16_t(0x2c00 + ((x + y) >> 3)), uint16_t(0x4000 + (y << 2) + (x >> 6))};
        std::memcpy(&image.pixels[i], px, 8);
    }
    const homecache::Key key{76561198000000001ull, {3715ll << 32, 845ll << 32, 1418ll << 32}};
    CHECK(homecache::Valid(key));
    CHECK(!homecache::Valid(homecache::Key{}));
    const auto file = homecache::EncodeFile(key, image);
    CHECK(!file.empty() && file.size() < homecache::kPackedBytes);

    sharing::Image decoded;
    CHECK(homecache::DecodeFile(key, file, decoded));
    CHECK(decoded.pixels.size() == image.pixels.size());
    CHECK(decoded.meta.x == image.meta.x && decoded.meta.nz == image.meta.nz && decoded.meta.exposure == image.meta.exposure);
    double worst = 0; bool depthExact = true;
    for (size_t i = 0; i + 8 <= decoded.pixels.size() && !decoded.pixels.empty(); i += 8) {
        uint16_t a[4], b[4]; std::memcpy(a, &image.pixels[i], 8); std::memcpy(b, &decoded.pixels[i], 8);
        for (int c = 0; c < 3; ++c) worst = std::max(worst, double(std::fabs(HalfValue(a[c]) - HalfValue(b[c])) / HalfValue(a[c])));
        depthExact = depthExact && a[3] == b[3];
    }
    CHECK(depthExact);
    CHECK(worst <= 1.0 / 64); // 5 mantissa bits for blue: half a step at most

    auto other = key; other.account += 1;
    CHECK(!homecache::DecodeFile(other, file, decoded));
    other = key; other.altar[1] += 1;
    CHECK(!homecache::DecodeFile(other, file, decoded));
    CHECK(homecache::AltarHash(other) != homecache::AltarHash(key));
    auto truncated = file; truncated.pop_back();
    CHECK(!homecache::DecodeFile(key, truncated, decoded));
    auto damaged = file; damaged[file.size() / 2] ^= 0x5a; // in the data (the code-length table may have unused entries)
    CHECK(!homecache::DecodeFile(key, damaged, decoded));
    auto version1 = file; version1[4] = 1;
    CHECK(!homecache::DecodeFile(key, version1, decoded));

    wchar_t name[96]{};
    homecache::FileName(key, name, 96);
    CHECK(std::wcsncmp(name, L"home-76561198000000001-", 23) == 0 && std::wcslen(name) == 23 + 16 + 4);

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("home snapshot cache: all checks passed (test picture %.1f MB of %.1f MB packed; colour error %.4f)\n",
                file.size() / 1048576.0, homecache::kPackedBytes / 1048576.0, worst);
    return 0;
}
