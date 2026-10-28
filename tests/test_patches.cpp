#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include "../src/portal_protocol.h"
#include "../src/portal_native_signatures.h"
#include "../src/portal_travel_signatures.h"
#include "../src/portal_snapshot_native.h"
#include "../src/portal_snapshot_hide.h"
#include "../src/portal_cast_hooks.h"

namespace {

uint8_t liveInput[128]{}, originalInput[128]{};
void ReadOriginalInput(const void* address, void* out, size_t size) {
    std::memcpy(out, originalInput + (static_cast<const uint8_t*>(address) - liveInput), size);
}

bool CheckChainedInputValidation() {
    using namespace portal::casting::hooks;
    const uint8_t original[]{0x4c,0x89,0x6c,0x24,0x28,0x48,0x89,0x44,0x24,0x20,
        0xe8,0,0,0,0,0x48,0x8b,0x9c,0x24,0x98,0x01,0x00,0x00};
    constexpr size_t call=32, target=64;
    std::memcpy(originalInput+call-inputCallOffset,original,sizeof(original));
    int32_t rel=int32_t(target-call-5);
    std::memcpy(originalInput+call+1,&rel,4);
    std::memcpy(liveInput,originalInput,sizeof(liveInput));
    for(size_t chainedTarget:{target,size_t(80),size_t(96)}) {
        // No hook, another mod already loaded, then a later loader generation.
        rel=int32_t(chainedTarget-call-5);
        std::memcpy(liveInput+call+1,&rel,4);
        if(!CheckInputCall(ReadOriginalInput,liveInput+call,liveInput+target) ||
           !CheckCall(ReadOriginalInput,liveInput+call,liveInput+target))return false;
    }
    // Canonical validation must still reject a changed native ABI or call site.
    for(size_t byte:{call-inputCallOffset,call,call+1,call+5}) {
        originalInput[byte]^=1;
        if(CheckInputCall(ReadOriginalInput,liveInput+call,liveInput+target))return false;
        originalInput[byte]^=1;
    }
    return !CheckInputCall(nullptr,liveInput+call,liveInput+target) &&
           !CheckCall(nullptr,liveInput+call,liveInput+target);
}

std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            bytes.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        bytes.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
        p += 2;
    }
    return bytes;
}

const uint8_t* FindUniqueInBuffer(const uint8_t* data, size_t size, const char* pattern) {
    auto pat = ParsePattern(pattern);
    if (pat.empty() || pat[0] < 0 || size < pat.size()) return nullptr;
    const uint8_t first = static_cast<uint8_t>(pat[0]);
    const size_t n = pat.size();
    const uint8_t* found = nullptr;
    for (const uint8_t* p = data; p + n <= data + size; ++p) {
        p = static_cast<const uint8_t*>(std::memchr(p, first, (data + size) - p - n + 1));
        if (!p) break;
        size_t k = 1;
        while (k < n && (pat[k] < 0 || p[k] == pat[k])) ++k;
        if (k == n) {
            if (found) return nullptr;
            found = p;
        }
    }
    return found;
}

} // namespace

int main() {
    if(!CheckChainedInputValidation()) {
        std::puts("FAIL: movement hook canonical validation with existing redirects");return 1;
    }
    std::puts("PASS: movement hook accepts chained redirects and rejects changed native calls");
    // 1. Verify protocol formatting & parsing round-trip
    portal::PortalInstance p{};
    p.active = true;
    p.ownerVisitedBase = true;
    p.portalId = 123456789ULL;
    p.ownerAccount = 76561198000000001ULL;
    p.fieldEnd.wx = 2450.5f;
    p.fieldEnd.wy = 310.25f;
    p.fieldEnd.wz = 4120.75f;
    p.fieldEnd.SyncFixedFromFloat();
    p.fieldEnd.SetNormal(0.6f, 0.8f);
    p.baseEnd.wx = 3100.0f;
    p.baseEnd.wy = 285.5f;
    p.baseEnd.wz = 2900.0f;
    p.baseEnd.SyncFixedFromFloat();
    p.baseEnd.SetNormal(-0.6f, -0.8f);
    p.altarPos = portal::ToFixed(3102.0f, 285.5f, 2902.5f);

    p.ownerPlayerId = 4242u;
    char buf[240]{};
    int len = portal::FormatPortalSync(buf, sizeof(buf), p);
    if (len <= 0) {
        std::printf("FAIL: FormatPortalSync failed\n");
        return 1;
    }
    portal::PortalSyncMsg syncOut{};
    if (!portal::ParsePortalSync({buf, static_cast<size_t>(len)}, syncOut) ||
        syncOut.portalId != p.portalId ||
        syncOut.ownerAccount != p.ownerAccount ||
        syncOut.visitedBase != 1u ||
        syncOut.fieldPos.x != p.fieldEnd.pos.x ||
        syncOut.basePos.z != p.baseEnd.pos.z ||
        syncOut.ownerPlayerId != p.ownerPlayerId) {
        std::printf("FAIL: ParsePortalSync mismatch\n");
        return 1;
    }
    // Older clients' /tpo1 (no owner player id) still parses.
    {
        char old[240]{};
        std::snprintf(old, sizeof(old), "/tpo1 %s", std::strchr(buf, ' ') + 1);
        *std::strrchr(old, ' ') = 0;
        portal::PortalSyncMsg oldOut{};
        if (!portal::ParsePortalSync(old, oldOut) || oldOut.ownerPlayerId != 0 || oldOut.visitedBase != 1u) {
            std::printf("FAIL: /tpo1 compatibility\n");
            return 1;
        }
    }

    char tpBuf[140]{};
    int tpLen = portal::FormatTeleportRequest(tpBuf, sizeof(tpBuf), 987654ULL, p.fieldEnd.pos);
    portal::TeleportRequestMsg tpOut{};
    if (tpLen <= 0 || !portal::ParseTeleportRequest({tpBuf, static_cast<size_t>(tpLen)}, tpOut) ||
        tpOut.nonce != 987654ULL || tpOut.target.y != p.fieldEnd.pos.y) {
        std::printf("FAIL: ParseTeleportRequest mismatch\n");
        return 1;
    }
    std::printf("PASS: Protocol round-trip tests succeeded\n");

    // 2. Offline binary signature verification against enshrouded.exe
    const char* exePath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Enshrouded\\enshrouded.exe";
    std::ifstream f(exePath, std::ios::binary | std::ios::ate);
    if (!f) {
        std::printf("SKIP: enshrouded.exe not found at default path\n");
        return 0;
    }
    size_t fileSize = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> exe(fileSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(exe.data()), fileSize);

    struct SigSpec {
        const char* name;
        const char* pattern;
    };
    const SigSpec sigs[] = {
        {"SceneIbl", portal::sceneprobe::signatures::sceneIbl},
        {"TerrainRequest", portal::sceneprobe::signatures::terrainRequest},
        {"PlayerSystem", "48 89 4C 24 08 55 41 56 48 8D AC 24 E8 F5 FF FF 48 81 EC 18 0B 00 00 41 B8 A8 05 00 00 48 8D 55 00 4C 8B F1 E8 ?? ?? ?? ?? 41 B8 A8 05 00 00 48 8D 55 00 49 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ??"},
        {"ApplyTeleport", "40 55 56 48 8D AC 24 A8 FD FF FF 48 81 EC 58 03 00 00 41 B8 B0 01 00 00 48 8D 55 90 48 8B F1 E8 ?? ?? ?? ?? 41 B8 B0 01 00 00 48 8D 55 90 48 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 89 9C 24 70 03 00 00"},
        {"NextIterator", "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00 00 75 ?? 32 C0 48 83 C4 20 5F C3 41 0F B6 80 B0 04 00 00 49 8D 88 70 04 00 00 48 89 5C 24 38 45 33 DB 41 0F B7 98 BA 04 00 00"},
        {"EntityOf", "8B 41 08 4C 8B C2 85 C0 75 ?? 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1 41 89 00 49 8B C0 C3 CC CC CC CC CC CC 8B 05 ?? ?? ?? ?? 48 85 C9 74 ?? 8B 41 10 C3 CC"},
        {"TerrainRay", "48 8B C4 48 89 50 10 55 53 57 41 54 48 8D A8 58 FF FF FF 48 81 EC 88 01 00 00 41 0F 28 08 48 8B F9 F2 0F 10 05 ?? ?? ?? ?? 49 8B D9 0F 29 70 A8 4D 8B E0 41 0F 28 31 0F 29 78 98 0F 28 FE 44 0F 29 40 88"},
        {"DirtyTracker", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B B1 90 02 00 00 49 8B D9 0F B7 6C 24 70 4D 8B F8 4C 8B D2 4C 8B F1 44 8B A6 DC 04 00 00 48 85 D2"},
        {"UiCommandLoop", "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 98 00 00 00 48 8D 81 58 4F 03 00"},
        {"PlayHudState", "4C 8B DC 55 49 8D AB E8 FD FF FF 48 81 EC 10 03 00 00"},
        {"DispatchCommand", "4C 8B DC 55 56 57 49 8D AB 18 FF FF FF"},
        {"DrawImageThunk", "4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC"},
        {"TravelGate", portal::travel::signatures::travelGate},
        {"CreateDynamicTexture", "40 53 56 57 41 56 41 57 48 81 EC F0 00 00 00 48 83 7A 18 00 48 8B F2 48 8B F9"},
        {"LocalEquipmentHideCall", portal::snapshot::hide::equipmentCall},
        {"EquipmentHideTarget", portal::snapshot::hide::equipmentTarget},
        {"LocalPlayerInputCall", portal::casting::hooks::inputPattern},
    };

    for (const auto& s : sigs) {
        const uint8_t* match = FindUniqueInBuffer(exe.data(), exe.size(), s.pattern);
        if (!match) {
            std::printf("FAIL: Signature not unique or missing: %s\n", s.name);
            return 1;
        }
        std::printf("PASS: Signature %-16s -> file offset 0x%llx (not RVA)\n", s.name, static_cast<unsigned long long>(match - exe.data()));
    }

    // The sharing path uses fixed native ABIs only after these exact unique
    // prologues match their expected RVAs in the supported executable.
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(exe.data());
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe.data()+dos->e_lfanew);
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    {
        using namespace portal::casting::hooks;
        const auto* site=FindUniqueInBuffer(exe.data(),exe.size(),inputPattern)+inputCallOffset;
        const size_t offset=site-exe.data();
        uint32_t rva=0;
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& s=sections[i];
            if(offset>=s.PointerToRawData&&offset<size_t(s.PointerToRawData)+s.SizeOfRawData)
                rva=uint32_t(offset-s.PointerToRawData+s.VirtualAddress);
        }
        if(rva!=inputCallRva||!CallsTarget(site,rva,inputTargetRva)) {
            std::puts("FAIL: local input builder call/ABI target changed");return 1;
        }
        std::printf("PASS: local input builder call RVA 0x%x -> 0x%x\n",rva,inputTargetRva);
    }
    // Only redirect the camera-hide getter used by local_visual_equipment_hide.
    // Resolve file offsets through PE sections before checking the relative call.
    {
        using namespace portal::snapshot::hide;
        auto rvaOf=[&](const uint8_t* ptr)->uint32_t{
            const size_t offset=ptr-exe.data();
            for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& s=sections[i];
                if(offset>=s.PointerToRawData&&offset<size_t(s.PointerToRawData)+s.SizeOfRawData)
                    return uint32_t(offset-s.PointerToRawData+s.VirtualAddress);
            }
            return 0;
        };
        const auto* site=FindUniqueInBuffer(exe.data(),exe.size(),equipmentCall)+callOffset;
        const auto* target=FindUniqueInBuffer(exe.data(),exe.size(),equipmentTarget);
        int32_t rel{};std::memcpy(&rel,site+1,4);
        if(site[0]!=0xe8||rvaOf(site)!=callRva||rvaOf(target)!=targetRva||
           int64_t(rvaOf(site))+5+rel!=targetRva){
            std::puts("FAIL: local equipment hide call/ABI target changed");return 1;
        }
        std::printf("PASS: local equipment hide call RVA 0x%x -> 0x%x\n",callRva,targetRva);
    }
    for(const auto& entry:portal::sharing::native::entries){
        const auto* match=FindUniqueInBuffer(exe.data(),exe.size(),entry.signature);
        uint32_t rva=0;
        if(match){const size_t offset=match-exe.data();
            for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& s=sections[i];
                if(offset>=s.PointerToRawData&&offset<size_t(s.PointerToRawData)+s.SizeOfRawData)
                    rva=uint32_t(offset-s.PointerToRawData+s.VirtualAddress);
            }
        }
        if(rva!=entry.rva){std::printf("FAIL: %s expected RVA 0x%x, got 0x%x\n",entry.name,entry.rva,rva);return 1;}
        std::printf("PASS: %s -> RVA 0x%x\n",entry.name,rva);
    }

    // The native travel gate override redirects the player system's call to the gate.
    {
        const uint8_t* player = FindUniqueInBuffer(exe.data(), exe.size(), sigs[2].pattern);
        const uint8_t* gate = FindUniqueInBuffer(exe.data(), exe.size(), portal::travel::signatures::travelGate);
        const uint8_t* site = player + portal::travel::signatures::travelGateCall;
        int32_t rel = 0;
        std::memcpy(&rel, site + 1, 4);
        if (site[0] != 0xe8 || site + 5 + rel != gate) {
            std::printf("FAIL: PlayerSystem+0x%zx does not call the travel gate\n", portal::travel::signatures::travelGateCall);
            return 1;
        }
        std::printf("PASS: PlayerSystem+0x%zx calls the travel gate\n", portal::travel::signatures::travelGateCall);
    }

    std::printf("ALL TESTS PASSED!\n");
    return 0;
}
