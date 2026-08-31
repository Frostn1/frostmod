// The rules the editor's configurable keys run on (src/keybinds.h).
//
// The failure that matters here is silent: a binding that does not parse, or one that
// formats to something that cannot be read back, leaves someone with a key that does
// nothing and no error saying why. The round trip is the point of this file.
//
// Pure — no Win32, no game — so CI runs it.

#include "../src/keybinds.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

using namespace kb;

int main() {
    // ---- names --------------------------------------------------------------
    {
        CHECK(VkFromName("K") == 'K', "K -> 0x%X", VkFromName("K"));
        CHECK(VkFromName("k") == 'K', "lowercase letters must fold to the same key");
        CHECK(VkFromName("7") == '7', "7 -> 0x%X", VkFromName("7"));
        CHECK(VkFromName("F9") == 0x78, "F9 -> 0x%X", VkFromName("F9"));
        CHECK(VkFromName("f9") == 0x78, "key names are case-insensitive");
        CHECK(VkFromName("Numpad0") == 0x60, "Numpad0 -> 0x%X", VkFromName("Numpad0"));
        CHECK(VkFromName(",") == 0xBC, "the , alias -> 0x%X", VkFromName(","));
        CHECK(VkFromName("Comma") == 0xBC, "Comma -> 0x%X", VkFromName("Comma"));
        CHECK(VkFromName("nonsense") == 0, "an unknown name must not resolve");
        CHECK(VkFromName("") == 0, "an empty name must not resolve");
    }

    // ---- every named key round trips ---------------------------------------
    {
        int n = 0;
        const NamedKey* keys = NamedKeys(&n);
        for (int i = 0; i < n; ++i) {
            char scratch[8];
            const char* printed = NameFromVk(keys[i].vk, scratch, sizeof(scratch));
            CHECK(*printed != '\0', "vk 0x%X (%s) has no printable name", keys[i].vk, keys[i].name);
            CHECK(VkFromName(printed) == keys[i].vk,
                  "%s prints as %s, which reads back as 0x%X", keys[i].name, printed,
                  VkFromName(printed));
        }
    }

    // ---- parsing ------------------------------------------------------------
    {
        Bind b;
        CHECK(ParseBind("K", &b) && b.vk == 'K' && !b.ctrl && !b.alt && !b.shift, "plain K");
        CHECK(ParseBind("  F9  ", &b) && b.vk == 0x78, "surrounding space must be ignored");
        CHECK(ParseBind("F9\r\n", &b) && b.vk == 0x78, "a CRLF from the config file must be ignored");
        CHECK(ParseBind("Ctrl+S", &b) && b.vk == 'S' && b.ctrl && !b.alt, "Ctrl+S");
        CHECK(ParseBind("ctrl+shift+Numpad1", &b) && b.vk == 0x61 && b.ctrl && b.shift,
              "modifiers are case-insensitive and stack");
        CHECK(ParseBind("Alt+F4", &b) && b.vk == 0x73 && b.alt, "Alt+F4");
        CHECK(ParseBind("none", &b) && !b.bound(), "none must parse to unbound");

        CHECK(!ParseBind("Ctrl+", &b), "a modifier with no key must be refused");
        CHECK(!ParseBind("Meta+K", &b), "an unknown modifier must be refused");
        CHECK(!ParseBind("NotAKey", &b), "an unknown key must be refused");
        CHECK(!ParseBind("", &b), "an empty binding must be refused");
    }

    // ---- formatting round trip ----------------------------------------------
    {
        const char* cases[] = { "K", "F9", "Numpad3", "Ctrl+S", "Alt+Shift+Home", "Comma", "none" };
        for (const char* text : cases) {
            Bind b;
            CHECK(ParseBind(text, &b), "%s did not parse", text);
            char out[64];
            FormatBind(b, out, sizeof(out));
            Bind again;
            CHECK(ParseBind(out, &again), "%s formatted as %s, which does not parse", text, out);
            CHECK(again == b, "%s -> %s -> a different binding", text, out);
        }
        // an alias prints as its canonical name
        Bind b;
        ParseBind(",", &b);
        char out[64];
        FormatBind(b, out, sizeof(out));
        CHECK(std::strcmp(out, "Comma") == 0, ", printed as %s", out);
    }

    // ---- defaults ------------------------------------------------------------
    {
        Bind binds[ActionCount];
        LoadDefaults(binds);
        for (int a = 0; a < ActionCount; ++a) {
            CHECK(binds[a].bound(), "action %s ships unbound", Actions()[a].id);
            char out[64];
            FormatBind(binds[a], out, sizeof(out));
            CHECK(std::strcmp(out, "none") != 0, "action %s has a default with no name",
                  Actions()[a].id);
            CHECK(Conflict(binds, a) < 0, "default for %s collides with %s", Actions()[a].id,
                  Conflict(binds, a) >= 0 ? Actions()[Conflict(binds, a)].id : "?");
        }
        // the keys the feature shipped with, so a config-less install is unchanged
        CHECK(binds[RcSetKey].vk == 'K' && binds[RcSave].vk == 'S' && binds[RcLoad].vk == 'L',
              "the shipped defaults moved");
    }

    // ---- config lines --------------------------------------------------------
    {
        Bind binds[ActionCount];
        LoadDefaults(binds);

        CHECK(ApplyConfigLine("rcam_save=F9", binds), "rcam_save=F9 was not applied");
        CHECK(binds[RcSave].vk == 0x78, "save did not move to F9");
        CHECK(binds[RcLoad].vk == 'L', "applying one line changed another action");

        CHECK(ApplyConfigLine("rcam_load=Ctrl+Numpad0\n", binds), "a trailing newline broke the line");
        CHECK(binds[RcLoad].vk == 0x60 && binds[RcLoad].ctrl, "load did not move to Ctrl+Numpad0");

        CHECK(!ApplyConfigLine("radar=1", binds), "another feature's line was claimed as ours");
        CHECK(!ApplyConfigLine("rcam_save", binds), "a line with no '=' was accepted");
        CHECK(!ApplyConfigLine("rcam_nosuchaction=K", binds), "an unknown action was accepted");

        // a value that does not parse must leave the previous binding alone
        CHECK(!ApplyConfigLine("rcam_save=NotAKey", binds), "an unparseable value was accepted");
        CHECK(binds[RcSave].vk == 0x78, "an unparseable value unbound the action");

        // what we write is what we read
        for (int a = 0; a < ActionCount; ++a) {
            char key[64], val[64], line[160];
            ConfigKey(a, key, sizeof(key));
            FormatBind(binds[a], val, sizeof(val));
            std::snprintf(line, sizeof(line), "%s=%s", key, val);
            Bind fresh[ActionCount];
            LoadDefaults(fresh);
            CHECK(ApplyConfigLine(line, fresh), "our own line '%s' does not read back", line);
            CHECK(fresh[a] == binds[a], "'%s' read back as a different binding", line);
        }
    }

    // ---- conflicts -----------------------------------------------------------
    {
        Bind binds[ActionCount];
        LoadDefaults(binds);
        CHECK(Conflict(binds, RcSave) < 0, "the defaults already conflict");
        ApplyConfigLine("rcam_save=X", binds);          // X is delete's default
        CHECK(Conflict(binds, RcSave) == RcDelete, "a duplicate binding was not reported");
        CHECK(Conflict(binds, RcDelete) == RcSave, "conflict must be reported from both sides");
        // a modifier makes it a different binding
        ApplyConfigLine("rcam_save=Ctrl+X", binds);
        CHECK(Conflict(binds, RcSave) < 0, "Ctrl+X and X were treated as the same binding");
    }

    if (g_failures == 0) {
        std::printf("keybinds: all checks passed\n");
        return 0;
    }
    std::printf("keybinds: %d check(s) failed\n", g_failures);
    return 1;
}
