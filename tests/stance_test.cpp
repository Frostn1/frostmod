// Sit and stand for MXB Coach's recorder (src/stance.h).
//
// The game's own files say which bind sits the rider and how. These pin how they are read,
// how bind polls become stance, and the two records' bytes, which are a wire contract with
// MXB Coach's Rust reader. Pure C++, runs anywhere.

#include "../src/coachrec.h"
#include "../src/stance.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

using namespace stance;

static const char* kPad = "028E045E-0000-0000-0000-504944564944";

// Lines as the game's writer emits them: name, type, device for a controller, number, then
// tuning values this reader ignores.
static const std::string kControls =
    "CTRL_THROTTLE C_AXIS 028E045E-0000-0000-0000-504944564944 5 1 0.020000 0.000000 1.000000\r\n"
    "CTRL_SITDirect C_BUTTON 028E045E-0000-0000-0000-504944564944 1 0.000000\r\n"
    "CTRL_SIT KEY 18 0.000000\r\n"
    "CTRL_LRLEAN C_POV 028E045E-0000-0000-0000-504944564944 0 0\r\n";

static void ParsesBindLines() {
    std::string name;
    Bind b;
    CHECK(ParseLine("CTRL_SIT KEY 18 0.000000", name, b) && name == "CTRL_SIT", "key line");
    CHECK(b.input == IN_KEY && b.index == 18 && b.device.empty(), "key %d %d", b.input, b.index);

    CHECK(ParseLine(std::string("  CTRL_SITDirect\tC_BUTTON ") + kPad + " 3 1.0\r", name, b), "button line");
    CHECK(name == "CTRL_SITDirect" && b.input == IN_PAD_BUTTON && b.index == 3 && b.device == kPad,
          "button %s %d %d %s", name.c_str(), b.input, b.index, b.device.c_str());

    CHECK(ParseLine(std::string("CTRL_SIT C_AXIS ") + kPad + " 2 1", name, b) && b.input == IN_PAD_AXIS, "axis");
    CHECK(ParseLine(std::string("CTRL_SIT C_POV ") + kPad + " 0", name, b) && b.input == IN_PAD_POV, "pov");
    CHECK(ParseLine("CTRL_SIT BUTTON mouse 0", name, b) && b.input == IN_OTHER, "plain button");

    // Broken numbers leave the control unbound rather than bound to something else.
    CHECK(ParseLine("CTRL_SIT KEY E", name, b) && b.input == IN_NONE, "a key name is not a scan code");
    CHECK(ParseLine("CTRL_SIT KEY 0", name, b) && b.input == IN_NONE, "scan code 0");
    CHECK(ParseLine("CTRL_SIT KEY 256", name, b) && b.input == IN_NONE, "scan code past 255");
    CHECK(ParseLine(std::string("CTRL_SIT C_BUTTON ") + kPad, name, b) && b.input == IN_NONE, "button with no number");
    CHECK(ParseLine("CTRL_SIT NONE", name, b) && b.input == IN_NONE, "unknown type");
    CHECK(!ParseLine("CTRL_SIT", name, b) && !ParseLine("   ", name, b), "not a bind line");
}

static void FindsTheControlByExactName() {
    // CTRL_SIT is a prefix of CTRL_SITDirect: each must find its own line, whichever comes first.
    Bind hold = FindBind(kControls, kHoldControl), toggle = FindBind(kControls, kToggleControl);
    CHECK(hold.input == IN_PAD_BUTTON && hold.index == 1, "hold bind %d %d", hold.input, hold.index);
    CHECK(toggle.input == IN_KEY && toggle.index == 18, "toggle bind %d %d", toggle.input, toggle.index);
    CHECK(FindBind(kControls, "CTRL_DAB").input == IN_NONE, "absent control");
    CHECK(FindBind("", kHoldControl).input == IN_NONE, "empty file");
}

static void ReadsIniValues() {
    const std::string ini = "\xEF\xBB\xBF[info]\r\nbikeid=YZ450F\r\n\r\n[Input]\r\n sit_direct = 1 \r\n"
                            "; comment=2\r\n[aids]\r\nautoriderdab=1\r\nautoridersit=0\r\n";
    CHECK(IniValue(ini, "input", "sit_direct") == "1", "sit_direct '%s'", IniValue(ini, "input", "sit_direct").c_str());
    CHECK(IniValue(ini, "aids", "AutoRiderSit") == "0", "autoridersit");
    CHECK(IniValue(ini, "aids", "sit_direct").empty(), "wrong section");
    CHECK(IniValue(ini, "", "bikeid") == "YZ450F", "any section");
    CHECK(IniValue(ini, "input", "comment").empty(), "comment read as a key");
    CHECK(IniValue("", "input", "sit_direct").empty(), "empty file");
    // global.ini: the profile in use.
    CHECK(IniValue("[global]\nlastprofile=feel_demo\n", "", "lastprofile") == "feel_demo", "lastprofile");
}

static void ReadsTheSetup() {
    Setup s = ReadSetup("[input]\nsit_direct=1\n[aids]\nautoridersit=0\n", kControls);
    CHECK(s.mode == HOLD && s.auto_sit == FLAG_OFF && s.bind.input == IN_PAD_BUTTON, "hold setup");

    s = ReadSetup("[input]\nsit_direct=0\n[aids]\nautoridersit=1\n", kControls);
    CHECK(s.mode == TOGGLE && s.auto_sit == FLAG_ON && s.bind.input == IN_KEY && s.bind.index == 18,
          "toggle setup reads CTRL_SIT");

    s = ReadSetup("[input]\ncombined_brakes=0\n", kControls);
    CHECK(s.mode == MODE_UNKNOWN && s.auto_sit == FLAG_UNKNOWN && s.bind.input == IN_NONE, "no sit_direct");

    s = ReadSetup("[input]\nsit_direct=1\n", "CTRL_SIT KEY 18\n");
    CHECK(s.mode == HOLD && s.bind.input == IN_NONE, "hold mode ignores the toggle control's bind");
}

static void RatesConfidence() {
    Setup s;
    s.mode       = HOLD;
    s.auto_sit   = FLAG_OFF;
    s.bind.input = IN_PAD_BUTTON;
    CHECK(Rate(s, SRC_DIRECTINPUT) == CONF_SURE, "hold on the bound pad");
    CHECK(Rate(s, SRC_XINPUT) == CONF_GUESS, "hold on the first XInput pad");
    CHECK(Rate(s, SRC_NONE) == CONF_NONE, "nothing polled");
    s.bind.input = IN_KEY;
    CHECK(Rate(s, SRC_KEYBOARD) == CONF_SURE, "hold on a key");
    s.mode = TOGGLE;
    CHECK(Rate(s, SRC_KEYBOARD) == CONF_GUESS, "toggle");
    s.mode     = HOLD;
    s.auto_sit = FLAG_UNKNOWN;
    CHECK(Rate(s, SRC_KEYBOARD) == CONF_GUESS, "auto-sit unknown");
    s.auto_sit = FLAG_ON;
    CHECK(Rate(s, SRC_KEYBOARD) == CONF_NONE, "auto-sit on");
    s.auto_sit   = FLAG_OFF;
    s.bind.input = IN_PAD_AXIS;
    CHECK(Rate(s, SRC_DIRECTINPUT) == CONF_NONE, "an axis");
    s.bind.input = IN_KEY;
    s.mode       = MODE_UNKNOWN;
    CHECK(Rate(s, SRC_KEYBOARD) == CONF_NONE, "unknown mode");
}

static void ParsesGuids() {
    Guid g;
    CHECK(ParseGuid("6F1D2B61-D5A0-11CF-BFC7-444553540000", g), "GUID_Joystick");
    CHECK(g.d1 == 0x6F1D2B61u && g.d2 == 0xD5A0 && g.d3 == 0x11CF && g.d4[0] == 0xBF && g.d4[1] == 0xC7 &&
              g.d4[7] == 0x00 && g.d4[2] == 0x44,
          "fields %08x %04x %04x", g.d1, g.d2, g.d3);
    Guid h;
    CHECK(ParseGuid("{6f1d2b61-d5a0-11cf-bfc7-444553540000}", h) && std::memcmp(&g, &h, 16) == 0, "braces, lower case");
    CHECK(!ParseGuid("6F1D2B61-D5A0-11CF-BFC7-44455354000", g), "short");
    CHECK(!ParseGuid("6F1D2B61-D5A0-11CF-BFC7-44455354000G", g), "not hex");
    CHECK(!ParseGuid("6F1D2B61D5A0-11CF-BFC7-4445535400000", g), "dash misplaced");
    CHECK(!ParseGuid("", g), "empty");
}

static void MapsButtonsAndKeys() {
    CHECK(XInputMask(0) == 0x1000 && XInputMask(1) == 0x2000 && XInputMask(3) == 0x8000, "A B Y");
    CHECK(XInputMask(4) == 0x0100 && XInputMask(6) == 0x0020 && XInputMask(9) == 0x0080, "LB Back RS");
    CHECK(XInputMask(10) == 0 && XInputMask(-1) == 0, "past the pad");

    CHECK(FixedVk(0x12) == 0, "E follows the layout");
    CHECK(FixedVk(0x39) == 0x20 && FixedVk(0x2A) == 0xA0 && FixedVk(0x1D) == 0xA2, "space, shifts, ctrl");
    CHECK(FixedVk(0x3B) == 0x70 && FixedVk(0x44) == 0x79 && FixedVk(0x57) == 0x7A, "F keys");
    CHECK(FixedVk(0xC8) == 0x26 && FixedVk(0xD0) == 0x28 && FixedVk(0x9D) == 0xA3, "arrows, right ctrl");
    CHECK(FixedVk(0x4F) == 0x61 && FixedVk(0x9C) == 0x0D, "numpad");
}

static void HoldFollowsTheBind() {
    Tracker t;
    t.configure(HOLD, true);
    CHECK(t.update(true, false, false) && t.state() == STAND, "first sample always records");
    CHECK(!t.update(true, false, false), "no change, no record");
    CHECK(t.update(true, true, false) && t.state() == SIT, "held: sitting");
    CHECK(!t.update(true, true, false), "still held");
    CHECK(t.update(true, true, true) && t.state() == STAND, "a crash is standing");
    CHECK(t.update(true, true, false) && t.state() == SIT, "back on the bike, still held");
    CHECK(t.update(false, true, false) && t.state() == STANCE_UNKNOWN, "unreadable");
    CHECK(t.update(true, false, false) && t.state() == STAND, "readable again");
}

static void TogglePressesFlip() {
    Tracker t;
    t.configure(TOGGLE, true);
    CHECK(t.update(true, true, false) && t.state() == STAND, "held at the start is not a press");
    CHECK(!t.update(true, false, false), "released");
    CHECK(t.update(true, true, false) && t.state() == SIT, "press: sit");
    CHECK(!t.update(true, true, false) && t.state() == SIT, "held is one press");
    CHECK(!t.update(true, false, false), "release changes nothing");
    CHECK(t.update(true, true, false) && t.state() == STAND, "press again: stand");
    t.update(true, false, false);
    t.update(true, true, false);
    CHECK(t.state() == SIT, "sitting before the crash");
    t.update(true, false, false);
    CHECK(t.update(true, false, true) && t.state() == STAND, "a crash stands the rider up");
    CHECK(!t.update(true, true, true) && t.state() == STAND, "a press while down is ignored");
    t.update(true, false, false);
    CHECK(t.update(true, true, false) && t.state() == SIT, "presses count again after");

    // A gap keeps the count and doesn't turn a press already down into a new one.
    t.update(true, false, false);
    CHECK(t.update(false, false, false) && t.state() == STANCE_UNKNOWN, "gap");
    CHECK(t.update(true, true, false) && t.state() == SIT, "back to the kept state, no flip");
    t.update(true, false, false);
    CHECK(t.update(true, true, false) && t.state() == STAND, "next press flips");
}

static void UnusableIsAlwaysUnknown() {
    Tracker t;
    t.configure(HOLD, false);
    CHECK(t.update(true, true, false) && t.state() == STANCE_UNKNOWN, "first sample records unknown");
    CHECK(!t.update(true, false, false) && !t.update(true, true, false), "and never changes");
    t.configure(MODE_UNKNOWN, true);
    CHECK(t.update(true, true, false) && t.state() == STANCE_UNKNOWN, "unknown mode");
    // A new stint records its first sample again.
    t.configure(HOLD, true);
    CHECK(t.update(true, false, false) && t.state() == STAND, "reconfigured");
}

static void PayloadBytes() {
    Setup s = ReadSetup("[input]\nsit_direct=1\n[aids]\nautoridersit=0\n", kControls);
    std::vector<uint8_t> p = BindPayload(s, SRC_DIRECTINPUT, CONF_SURE);
    CHECK(p.size() == 52, "bind size %zu", p.size());
    const uint8_t head[8] = {1, IN_PAD_BUTTON, HOLD, FLAG_OFF, CONF_SURE, SRC_DIRECTINPUT, 0, 0};
    CHECK(std::memcmp(p.data(), head, 8) == 0, "bind head");
    int32_t index;
    std::memcpy(&index, &p[8], 4);
    CHECK(index == 1, "button %d", index);
    CHECK(std::memcmp(&p[12], kPad, 36) == 0 && p[48] == 0 && p[51] == 0, "device GUID, NUL-padded");

    Setup none;
    p = BindPayload(none, SRC_NONE, CONF_NONE);
    std::memcpy(&index, &p[8], 4);
    CHECK(p[1] == IN_NONE && p[2] == MODE_UNKNOWN && p[3] == FLAG_UNKNOWN && index == -1 && p[12] == 0, "no bind");

    Setup longdev;
    longdev.bind.input  = IN_PAD_BUTTON;
    longdev.bind.device = std::string(60, 'x');
    p = BindPayload(longdev, SRC_NONE, CONF_NONE);
    CHECK(p.size() == 52 && p[12 + 38] == 'x' && p[12 + 39] == 0, "device cut to 39 and terminated");

    auto e = EventPayload(12.5f, 0.25f, SIT);
    float t, pos;
    std::memcpy(&t, &e[0], 4);
    std::memcpy(&pos, &e[4], 4);
    CHECK(e.size() == 12 && t == 12.5f && pos == 0.25f && e[8] == SIT && e[9] == 0 && e[11] == 0, "event");
}

// The recorder writes both records into the stint's file, and a reader that knows only the
// original tags (MXB Coach before stance) reads the rest of the file unchanged.
static void RecordsAndOldReadersSkipThem() {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp) tmp = std::getenv("TEMP");  // Windows
    std::string dir = tmp ? tmp : "/tmp";
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';

    coachrec::Recorder r;
    r.set_dir(dir);
    uint8_t event[820] = {0};
    r.on_event(event, sizeof(event));
    uint8_t session[112] = {0};
    CHECK(r.on_run_init(session, sizeof(session), "stance-test"), "open");
    Setup s = ReadSetup("[input]\nsit_direct=1\n", kControls);
    std::vector<uint8_t> bind = BindPayload(s, SRC_DIRECTINPUT, Rate(s, SRC_DIRECTINPUT));
    r.record(coachrec::STANCE_BIND, bind.data(), uint32_t(bind.size()));

    Tracker tr;
    tr.configure(s.mode, true);
    uint8_t bike[188] = {0};
    const bool held[] = {false, false, true, true, false};
    int stance_records = 0;
    for (int i = 0; i < 5; ++i) {
        r.on_sample(bike, sizeof(bike), 0.02f * i, 0.1f * i);
        if (tr.update(true, held[i], false)) {
            auto e = EventPayload(0.02f * i, 0.1f * i, tr.state());
            r.record(coachrec::STANCE, e.data(), uint32_t(e.size()));
            ++stance_records;
        }
    }
    int lap[4] = {0, 0, 61234, 1};
    r.on_lap(lap, sizeof(lap));
    const std::string path = r.path();
    r.on_run_end();
    r.record(coachrec::STANCE, bind.data(), 4);  // after the file closed: goes nowhere
    CHECK(stance_records == 3, "stance records %d", stance_records);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> b;
    if (f) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
        std::fclose(f);
    }
    // telemetry.rs's loop: skip every record by its length, act on the tags it knows.
    std::vector<uint8_t> tags;
    int samples = 0, laps = 0, unknown = 0;
    size_t at = 8;
    while (at + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, &b[at + 4], 4);
        if (at + 8 + len > b.size()) break;
        const uint8_t t = b[at];
        tags.push_back(t);
        if (t == coachrec::SAMPLE) ++samples;
        else if (t == coachrec::LAP) ++laps;
        else if (t > coachrec::END) ++unknown;
        if (t == coachrec::STANCE_BIND) CHECK(len == 52, "bind length %u", len);
        if (t == coachrec::STANCE) CHECK(len == 12, "stance length %u", len);
        at += 8 + len;
    }
    CHECK(at == b.size(), "records don't tile the file");
    CHECK(samples == 5 && laps == 1 && unknown == 4, "samples %d laps %d new records %d", samples, laps, unknown);
    // No centreline in this stint: EVENT, SESSION, then the bind.
    CHECK(tags.size() > 2 && tags[1] == coachrec::SESSION && tags[2] == coachrec::STANCE_BIND, "bind follows SESSION");
    CHECK(!tags.empty() && tags.back() == coachrec::END, "file ends with END");
    std::remove(path.c_str());
}

int main() {
    ParsesBindLines();
    FindsTheControlByExactName();
    ReadsIniValues();
    ReadsTheSetup();
    RatesConfidence();
    ParsesGuids();
    MapsButtonsAndKeys();
    HoldFollowsTheBind();
    TogglePressesFlip();
    UnusableIsAlwaysUnknown();
    PayloadBytes();
    RecordsAndOldReadersSkipThem();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stance: all checks passed\n");
    return 0;
}
