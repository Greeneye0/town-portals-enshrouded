#pragma once
// Greeneye Loader mod API (see GreeneyeLoader/ and common/README.md). Shared by the loader and
// every mod; plain C data only crosses between them.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cwchar>

constexpr uint32_t kGreeneyeApiVersion = 2;

enum GreeneyeSide : int { kGreeneyeClient = 1, kGreeneyeServer = 2 };

struct GreeneyeHost {
    // API 1
    uint32_t size;
    uint32_t apiVersion;
    int side;
    const wchar_t* modDir;
    const wchar_t* gameDir;
    void (*log)(const char* line);

    // API 2
    uint32_t generation;
    bool hotReload;
    void* token;
    bool (*enter)(void* token);
    void (*leave)();
    uint8_t* (*find)(const char* pattern, void* module);
    void (*read)(const void* p, void* out, size_t n);
    void* (*hook)(uint8_t* target, void* detour, size_t len, const uint8_t* prologue, size_t prologueLen);
    void* (*redirectCall)(uint8_t* call, void* detour);
    void (*poll)();
    void* (*keep)(const char* key, size_t size);
};

using GreeneyeModStartFn = bool (*)(const GreeneyeHost* host);
#define GREENEYE_MOD_START extern "C" __declspec(dllexport) bool GreeneyeModStart(const GreeneyeHost* host)

struct GreeneyeHot {
    uint32_t size;
    bool (*prepare)();
    bool (*ready)();
    void (*resume)();
};
using GreeneyeModHotFn = const GreeneyeHot* (*)();
#define GREENEYE_MOD_HOT extern "C" __declspec(dllexport) const GreeneyeHot* GreeneyeModHot()

namespace greeneye {

inline bool ClaimProcess(const wchar_t* mod)
{
    wchar_t name[96];
    swprintf_s(name, L"Local\\Greeneye-%s-%lu", mod, GetCurrentProcessId());
    HANDLE claim = CreateMutexW(nullptr, FALSE, name);
    if (!claim)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(claim);
        return false;
    }
    return true;
}

inline bool HostOk(const GreeneyeHost* host)
{
    return host && host->apiVersion >= 1 && host->size >= offsetof(GreeneyeHost, generation) && host->modDir;
}

inline bool HostHasHooks(const GreeneyeHost* host)
{
    return HostOk(host) && host->apiVersion >= 2 && host->size >= sizeof(GreeneyeHost) && host->hook && host->enter;
}

inline const GreeneyeHost*& Host()
{
    static const GreeneyeHost* host = nullptr;
    return host;
}

inline void UseHost(const GreeneyeHost* host)
{
    Host() = HostHasHooks(host) ? host : nullptr;
}

inline bool ClaimProcess(const wchar_t* mod, const GreeneyeHost* host)
{
    if (HostHasHooks(host) && host->generation > 1)
        return true;
    return ClaimProcess(mod);
}

class Lease {
public:
    Lease() : host_(Host()), active_(!host_ || host_->enter(host_->token)) {}
    ~Lease()
    {
        if (host_)
            host_->leave();
    }
    explicit operator bool() const { return active_; }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

private:
    const GreeneyeHost* host_;
    bool active_;
};

} // namespace greeneye
