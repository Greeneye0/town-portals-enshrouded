// Decodes voice line files with portal_adpcm.h and writes the PCM next to them as <file>.pcm.wav
// (tests/test_adpcm.py compares those with libsndfile's decode of the same files).
#include "../src/portal_adpcm.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
        const auto pcm = portal::adpcm::ToPcm(b.data(), b.size());
        if (pcm.empty()) { std::printf("%s: not decoded\n", argv[i]); ++bad; continue; }
        std::ofstream(std::string(argv[i]) + ".pcm.wav", std::ios::binary)
            .write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size()));
    }
    return bad;
}
