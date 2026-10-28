// Reads the voice pack of every character save it is given (compare with tools/voice/scripts/find-voice.py).
#include "../src/portal_voicepack.h"
#include <cstdio>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
        const uint32_t id = portal::voicepack::FromSave(b.data(), b.size());
        const int voice = portal::voicepack::VoiceIndex(id);
        std::printf("%s 0x%08x %s\n", argv[i], id, voice < 0 ? "?" : portal::voicepack::kNames[voice]);
        bad += voice < 0;
    }
    return bad;
}
