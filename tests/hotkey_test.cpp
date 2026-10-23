// The menu key (src/hotkey.h): menukey= parsing, the names written back, and which keys may
// open the menu. Pure, so CI runs it.

#include "../src/hotkey.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using namespace frostmod::hotkey;

static Hotkey P(const char* s, bool* ok = nullptr) {
    Hotkey k; k.vk = -1;
    const bool r = Parse(s, &k);
    if (ok) *ok = r;
    return k;
}

static bool Fmt(const Hotkey& k, const char* want) {
    char b[48]; Format(k, b, sizeof(b));
    return std::strcmp(b, want) == 0;
}

int main() {
    // The default is plain F8, so an install with no menukey= line behaves as before.
    CHECK(Default().vk == 0x77 && !Default().ctrl && !Default().shift && !Default().alt, "default F8");
    CHECK(Fmt(Default(), "F8"), "default formats as F8");

    bool ok = false;
    CHECK(P("F8", &ok) == Default() && ok, "F8");
    CHECK(P("f8", &ok) == Default() && ok, "lower case");
    CHECK(P(" F8\r\n", &ok) == Default() && ok, "whitespace and line end");
    CHECK(P("F1").vk == 0x70 && P("F12").vk == 0x7B && P("F24").vk == 0x87, "F-keys");
    P("F25", &ok); CHECK(!ok, "F25");
    P("F0", &ok);  CHECK(!ok, "F0");

    Hotkey k = P("Ctrl+F9", &ok);
    CHECK(ok && k.vk == 0x78 && k.ctrl && !k.shift && !k.alt, "Ctrl+F9");
    k = P("shift + alt + insert", &ok);
    CHECK(ok && k.vk == 0x2D && k.shift && k.alt && !k.ctrl, "shift+alt+insert");
    k = P("Control+K", &ok);
    CHECK(ok && k.vk == 'K' && k.ctrl, "Control alias");
    k = P("Alt+Num5", &ok);
    CHECK(ok && k.vk == 0x65 && k.alt, "numpad");
    k = P("VK_E2", &ok);   CHECK(ok && k.vk == 0xE2, "VK_ form");
    k = P("0x7a", &ok);    CHECK(ok && k.vk == 0x7A, "hex form");
    k = P("`", &ok);       CHECK(ok && k.vk == 0xC0, "backquote");

    P("", &ok);            CHECK(!ok, "empty");
    P("Ctrl+", &ok);       CHECK(!ok, "modifier with no key");
    P("+F8", &ok);         CHECK(!ok, "leading plus");
    P("Hyper+F8", &ok);    CHECK(!ok, "unknown modifier");
    P("Banana", &ok);      CHECK(!ok, "unknown key");
    P("F8+Ctrl", &ok);     CHECK(!ok, "key before modifier");
    P("VK_ZZ", &ok);       CHECK(!ok, "bad hex");

    // A failed parse leaves the output alone, so a bad cfg line keeps the current key.
    Hotkey keep; keep.vk = 0x79;
    CHECK(!Parse("nonsense", &keep) && keep.vk == 0x79, "failed parse leaves the key");

    // Format writes modifiers in one order; Parse(Format(k)) round-trips.
    Hotkey c; c.vk = 0x78; c.alt = true; c.ctrl = true;
    CHECK(Fmt(c, "Ctrl+Alt+F9"), "order");
    for (int vk = 1; vk < 0xFF; ++vk)
        for (int m = 0; m < 8; ++m) {
            Hotkey a; a.vk = vk; a.ctrl = (m & 1) != 0; a.shift = (m & 2) != 0; a.alt = (m & 4) != 0;
            char b[48]; Format(a, b, sizeof(b));
            Hotkey back; back.vk = -1;
            const bool r = Parse(b, &back);
            CHECK(r && back == a, "round trip vk=0x%02X mods=%d text=%s", vk, m, b);
        }

    // Pack/Unpack carries every field.
    for (int m = 0; m < 8; ++m) {
        Hotkey a; a.vk = 0x7B; a.ctrl = (m & 1) != 0; a.shift = (m & 2) != 0; a.alt = (m & 4) != 0;
        CHECK(Unpack(Pack(a)) == a, "pack mods=%d", m);
    }

    // Which keys may open the menu.
    const char* why = nullptr;
    CHECK(Bindable(Default(), &why) && !why, "F8");
    CHECK(Bindable(P("F9"), nullptr), "F9");
    CHECK(Bindable(P("Insert"), nullptr), "Insert");
    CHECK(Bindable(P("Ctrl+K"), nullptr), "Ctrl+K");
    CHECK(Bindable(P("Shift+F8"), nullptr), "Shift+F8");
    CHECK(Bindable(P("Ctrl+F7"), nullptr), "Ctrl+F7 is not clean view");
    CHECK(!Bindable(P("Esc"), &why) && why, "Esc");
    CHECK(!Bindable(P("Ctrl+Esc"), nullptr), "Ctrl+Esc");
    CHECK(!Bindable(P("K"), nullptr), "bare letter");
    CHECK(!Bindable(P("7"), nullptr), "bare digit");
    CHECK(!Bindable(P("Enter"), nullptr), "Enter");
    CHECK(!Bindable(P("Up"), nullptr), "arrow");
    CHECK(!Bindable(P("Space"), nullptr), "Space");
    CHECK(!Bindable(P("F7"), nullptr), "F7 is clean view");
    CHECK(!Bindable(P("Ctrl+Shift+H"), nullptr), "Coach hide");
    CHECK(!Bindable(P("LShift"), nullptr), "modifier alone");
    CHECK(!Bindable(P("Mouse2"), nullptr), "mouse button");

    // The plain key ignores modifiers (as F8 always did); a combination needs exactly its own.
    CHECK(Matches(Default(), false, false, false), "F8");
    CHECK(Matches(Default(), true, false, false), "Ctrl+F8 still opens a plain F8 menu");
    const Hotkey cf9 = P("Ctrl+F9");
    CHECK(Matches(cf9, true, false, false), "Ctrl+F9");
    CHECK(!Matches(cf9, false, false, false), "F9 alone");
    CHECK(!Matches(cf9, true, true, false), "Ctrl+Shift+F9");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("hotkey: all checks passed\n");
    return 0;
}
