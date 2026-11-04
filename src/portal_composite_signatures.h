// The in-frame renderer's native functions and layouts are checked by portal_natives.h.
inline bool CompositeNativeMatches(const uint8_t*) {
    return natives::RenderReady();
}
