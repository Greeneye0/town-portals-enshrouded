// The game's Interact key from its user_settings.json (portal_bindings.h InteractKeysFromSettings).
#include "../src/portal_bindings.h"
#include <cstdio>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static std::string Settings(const char* gameplay) {
    std::string s = "{\n\t\"version\": 13,\n\t\"input\": {\n\t\t\"keyboardMouse\": {\n\t\t\t\"selectedPresetId\": \"PresetA\",\n"
                     "\t\t\t\"bindings\": {\n\t\t\t\t\"customBasePresetId\": \"PresetA\",\n\t\t\t\t\"gameplay\": [\n";
    s += gameplay;
    s += "\n\t\t\t\t],\n\t\t\t\t\"ui\": [\n\t\t\t\t]\n\t\t\t}\n\t\t},\n\t\t\"gamepad\": {\"bindings\": {\"gameplay\": [{\"id\": \"ContextualAction\", "
         "\"action\": {\"primary\": {\"operation\": {\"type\": \"GamepadButton\", \"id\": \"FaceUp\"}}}}]}}\n\t},\n\t\"gameplay\": {\"textScale\": \"1\"}\n}";
    return s;
}

static std::string Binding(const char* action, const char* type1, const char* id1, const char* type2 = nullptr, const char* id2 = nullptr) {
    char text[1024];
    std::snprintf(text, sizeof(text),
        "{\"id\": \"%s\", \"action\": {\"primary\": {\"operation\": {\"type\": \"%s\", \"id\": \"%s\", \"axis\": \"Positive\", \"gesture\": \"None\"}, \"modifiers\": []}, "
        "\"secondary\": {\"operation\": {\"type\": \"%s\", \"id\": \"%s\", \"axis\": \"Positive\", \"gesture\": \"None\"}, \"modifiers\": []}, "
        "\"modality\": \"Press\", \"toggle\": false}}",
        action, type1, id1, type2 ? type2 : "None", id2 ? id2 : "");
    return text;
}

int main() {
    using namespace portal::bindings;
    // No custom bindings (the game's defaults): E.
    Keys k = InteractKeysFromSettings(Settings(""));
    CHECK(k.count == 1 && k.vk[0] == 'E' && !k.custom);
    // Unreadable or empty file: E.
    CHECK(InteractKeysFromSettings("").vk[0] == 'E');
    CHECK(InteractKeysFromSettings("{ not json").vk[0] == 'E');
    // Interact moved to F; another action's binding is ignored.
    k = InteractKeysFromSettings(Settings((Binding("Jump", "Key", "Space") + ",\n" + Binding("ContextualAction", "Key", "F")).c_str()));
    CHECK(k.custom && k.count == 1 && k.vk[0] == 'F');
    // Primary and secondary.
    k = InteractKeysFromSettings(Settings(Binding("ContextualAction", "Key", "Number5", "MouseButton", "Button4").c_str()));
    CHECK(k.count == 2 && k.vk[0] == '5' && k.vk[1] == VK_XBUTTON1);
    // The gamepad's own binding of the same action does not count for the keyboard.
    k = InteractKeysFromSettings(Settings(Binding("Jump", "Key", "Space").c_str()));
    CHECK(!k.custom && k.vk[0] == 'E');
    // Key names.
    CHECK(GameKeyToVk("Key", "F12") == VK_F12);
    CHECK(GameKeyToVk("Key", "KeyPad_7") == VK_NUMPAD7);
    CHECK(GameKeyToVk("Key", "Space") == VK_SPACE);
    CHECK(GameKeyToVk("Key", "Semicolon") == VK_OEM_1);
    CHECK(GameKeyToVk("MouseButton", "Middle") == VK_MBUTTON);
    CHECK(GameKeyToVk("Key", "Unknown") == 0);
    // Bound to something that is not a key: no keyboard key (the gamepad still works).
    k = InteractKeysFromSettings(Settings(Binding("ContextualAction", "MouseWheel", "Up").c_str()));
    CHECK(k.custom && k.count == 0);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("interact bindings: all checks passed\n");
    return 0;
}
