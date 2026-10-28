#pragma once
#include "portal_types.h"
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace portal {

// Hidden multiplayer protocol channel (type 255 is relayed by all Enshrouded servers and ignored by chat UI).
inline constexpr uint8_t kChatType = 255;
inline constexpr std::string_view kHello = "/tph1";
inline constexpr std::string_view kHelloReply = "/tps1 0 hello";

struct TeleportRequestMsg {
    uint64_t nonce{};
    FixedPos target{};
};

struct PortalSyncMsg {
    uint64_t portalId{};
    uint64_t ownerAccount{};
    FixedPos fieldPos{};
    int32_t fieldYaw{};
    FixedPos basePos{};
    int32_t baseYaw{};
    FixedPos altarPos{};
    uint32_t visitedBase{};
    uint32_t ownerPlayerId{}; // /tpo2 only; 0 from older clients
};

struct PortalCloseMsg {
    uint64_t portalId{};
    uint64_t ownerAccount{};
};

namespace detail {
template <class T>
inline bool NextInt(std::string_view& text, T& out, bool last) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto r = std::from_chars(begin, end, out);
    if (r.ec != std::errc{}) return false;
    if (last) {
        while (r.ptr < end && *r.ptr == ' ') ++r.ptr;
        return r.ptr == end;
    }
    if (r.ptr == end || *r.ptr != ' ') return false;
    text.remove_prefix(static_cast<size_t>(r.ptr - begin) + 1);
    return true;
}
} // namespace detail

// Format: "/tpg1 <nonce> <rawX> <rawY> <rawZ>"
inline int FormatTeleportRequest(char* buf, size_t cap, uint64_t nonce, const FixedPos& target) {
    return sprintf_s(buf, cap, "/tpg1 %llu %lld %lld %lld",
                     static_cast<unsigned long long>(nonce),
                     static_cast<long long>(target.x),
                     static_cast<long long>(target.y),
                     static_cast<long long>(target.z));
}

inline bool ParseTeleportRequest(std::string_view text, TeleportRequestMsg& out) {
    if (!text.starts_with("/tpg1 ") || text.size() > 140) return false;
    text.remove_prefix(6);
    TeleportRequestMsg m{};
    if (!detail::NextInt(text, m.nonce, false) ||
        !detail::NextInt(text, m.target.x, false) ||
        !detail::NextInt(text, m.target.y, false) ||
        !detail::NextInt(text, m.target.z, true)) {
        return false;
    }
    constexpr int64_t kMaxCoord = 100000LL * 4294967296LL;
    if (!m.nonce || m.target.x <= 0 || m.target.z <= 0 || m.target.x > kMaxCoord || m.target.z > kMaxCoord) {
        return false;
    }
    out = m;
    return true;
}

// Format: "/tpo2 <portalId> <account> <fx> <fy> <fz> <fyaw> <bx> <by> <bz> <byaw> <ax> <ay> <az> <visited> <ownerPlayerId>"
// /tpo1 is the same without the owner's player id (older clients send it; the portal then
// keeps the default colour). Older clients cannot read /tpo2.
inline int FormatPortalSync(char* buf, size_t cap, const PortalInstance& p) {
    return sprintf_s(buf, cap,
                     "/tpo2 %llu %llu %lld %lld %lld %d %lld %lld %lld %d %lld %lld %lld %u %u",
                     static_cast<unsigned long long>(p.portalId),
                     static_cast<unsigned long long>(p.ownerAccount),
                     static_cast<long long>(p.fieldEnd.pos.x),
                     static_cast<long long>(p.fieldEnd.pos.y),
                     static_cast<long long>(p.fieldEnd.pos.z),
                     static_cast<int>(p.fieldEnd.yawMillirad),
                     static_cast<long long>(p.baseEnd.pos.x),
                     static_cast<long long>(p.baseEnd.pos.y),
                     static_cast<long long>(p.baseEnd.pos.z),
                     static_cast<int>(p.baseEnd.yawMillirad),
                     static_cast<long long>(p.altarPos.x),
                     static_cast<long long>(p.altarPos.y),
                     static_cast<long long>(p.altarPos.z),
                     p.ownerVisitedBase ? 1u : 0u,
                     p.ownerPlayerId);
}

inline bool ParsePortalSync(std::string_view text, PortalSyncMsg& out) {
    bool withOwner = text.starts_with("/tpo2 ");
    if ((!withOwner && !text.starts_with("/tpo1 ")) || text.size() > 240) return false;
    text.remove_prefix(6);
    PortalSyncMsg m{};
    if (!detail::NextInt(text, m.portalId, false) ||
        !detail::NextInt(text, m.ownerAccount, false) ||
        !detail::NextInt(text, m.fieldPos.x, false) ||
        !detail::NextInt(text, m.fieldPos.y, false) ||
        !detail::NextInt(text, m.fieldPos.z, false) ||
        !detail::NextInt(text, m.fieldYaw, false) ||
        !detail::NextInt(text, m.basePos.x, false) ||
        !detail::NextInt(text, m.basePos.y, false) ||
        !detail::NextInt(text, m.basePos.z, false) ||
        !detail::NextInt(text, m.baseYaw, false) ||
        !detail::NextInt(text, m.altarPos.x, false) ||
        !detail::NextInt(text, m.altarPos.y, false) ||
        !detail::NextInt(text, m.altarPos.z, false) ||
        !detail::NextInt(text, m.visitedBase, !withOwner) ||
        (withOwner && !detail::NextInt(text, m.ownerPlayerId, true))) {
        return false;
    }
    if (!m.portalId) return false;
    out = m;
    return true;
}

// Format: "/tpc1 <portalId> <account>"
inline int FormatPortalClose(char* buf, size_t cap, uint64_t portalId, uint64_t ownerAccount) {
    return sprintf_s(buf, cap, "/tpc1 %llu %llu",
                     static_cast<unsigned long long>(portalId),
                     static_cast<unsigned long long>(ownerAccount));
}

inline bool ParsePortalClose(std::string_view text, PortalCloseMsg& out) {
    if (!text.starts_with("/tpc1 ") || text.size() > 96) return false;
    text.remove_prefix(6);
    PortalCloseMsg m{};
    if (!detail::NextInt(text, m.portalId, false) ||
        !detail::NextInt(text, m.ownerAccount, true)) {
        return false;
    }
    if (!m.portalId) return false;
    out = m;
    return true;
}

} // namespace portal
