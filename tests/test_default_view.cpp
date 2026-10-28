// Default picture camera (portal_default_view.h): carried to any endpoint, a point at the same
// place relative to that endpoint projects exactly as relative to the reference one, the
// picture's view is the reference's narrowed, and the result passes the snapshot checks.
#include "../src/portal_default_view.h"
#include <cmath>
#include <cstdio>

using namespace portal;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void Project(const double* clip, const double* p, double out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = clip[row * 4] * p[0] + clip[row * 4 + 1] * p[1] + clip[row * 4 + 2] * p[2] + clip[row * 4 + 3];
}
// A point at (right, up, out) metres from an endpoint.
static void At(double x, double y, double z, double nx, double nz, double right, double up, double out, double p[3]) {
    p[0] = x + right * nz + out * nx; p[1] = y + up; p[2] = z - right * nx + out * nz;
}

int main() {
    const auto& e = defaultview::kRefEnd;
    const auto& n = defaultview::kRefNormal;
    sharing::Metadata self{};
    defaultview::Place(e[0], e[1], e[2], n[0], n[1], self);
    for (int k = 0; k < 4; ++k) {
        CHECK(std::fabs(self.clip[k] - defaultview::kRefClip[k] * defaultview::kNarrow) < 1e-6 * (1 + std::fabs(self.clip[k])));
        CHECK(std::fabs(self.clip[4 + k] - defaultview::kRefClip[4 + k] * defaultview::kNarrow) < 1e-6 * (1 + std::fabs(self.clip[4 + k])));
        CHECK(std::fabs(self.clip[8 + k] - defaultview::kRefClip[8 + k]) < 1e-6 * (1 + std::fabs(self.clip[8 + k])));
    }
    CHECK(std::fabs(self.cx - defaultview::kRefCamera[0]) < 1e-3 && std::fabs(self.cz - defaultview::kRefCamera[2]) < 1e-3);

    // Somewhere else, facing another way.
    const double angle = 2.1, nx = std::sin(angle), nz = std::cos(angle), x = 6100.25, y = 912.5, z = 2733.75;
    sharing::Metadata moved{};
    defaultview::Place(x, y, z, nx, nz, moved);
    CHECK(sharing::ValidMetadata([&] { auto m = moved; m.exposure = 1; return m; }()));
    const double samples[][3] = {{0, 1.6, 30}, {-12, 4, 80}, {25, -3, 400}, {3, 10, 7}};
    for (const auto& s : samples) {
        double p1[3], p2[3], c1[3], c2[3];
        At(e[0], e[1], e[2], n[0], n[1], s[0], s[1], s[2], p1);
        At(x, y, z, nx, nz, s[0], s[1], s[2], p2);
        Project(self.clip, p1, c1);
        Project(moved.clip, p2, c2);
        for (int i = 0; i < 3; ++i) CHECK(std::fabs(c1[i] - c2[i]) < 1e-6 * (1 + std::fabs(c1[i])));
    }
    // Straight out in front is near the middle of the picture, and in front of the camera.
    double ahead[3], c[3];
    At(x, y, z, nx, nz, 0, 1.62, 200, ahead);
    Project(moved.clip, ahead, c);
    CHECK(c[2] > 150 && std::fabs(c[0] / c[2]) < 0.05 && std::fabs(c[1] / c[2]) < 0.5);
    // The camera keeps its place relative to the endpoint.
    const double back = (moved.cx - x) * nx + (moved.cz - z) * nz;
    const double refBack = (defaultview::kRefCamera[0] - e[0]) * n[0] + (defaultview::kRefCamera[2] - e[2]) * n[1];
    CHECK(std::fabs(back - refBack) < 1e-3 && std::fabs((moved.cy - y) - (defaultview::kRefCamera[1] - e[1])) < 1e-3);

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("default picture camera: all checks passed (ahead lands at %.3f, %.3f)\n", c[0] / c[2], c[1] / c[2]);
    return 0;
}
