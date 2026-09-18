// Rider lean for MXB Coach's recorder (src/lean.h).
//
// The rider moves their body with two controls of their own, bound in the same controls.txt
// the Sit bind is read from. These pin how those lines are read, what a reading is worth when
// the game is moving the rider itself, and the two records' bytes, which are a wire contract
// with MXB Coach's Rust reader. Pure C++, runs anywhere.

#include "../src/coachrec.h"
#include "../src/lean.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

static const char* kPad = "6F1D2B60-D5A0-11CF-BFC7-444553540000";

// The ten tuning values every controls.txt line ends with.
static const char* kTune = "0.020000 0.000000 1.000000 1 0.200000 0.100000 0 1.000000 0.000000 0.000000";

static std::string Controls(const std::string& lr, const std::string& fb) {
    return "CTRL_THROTTLE AXIS " + std::string(kPad) + " 2 0 " + kTune + "\n" + lr + "\n" + fb + "\n";
}

static void bindings() {
    const std::string c = Controls(std::string("CTRL_LRLEAN AXIS ") + kPad + " 3 0 " + kTune,
                                   std::string("CTRL_FBLEAN AXIS ") + kPad + " 4 1 " + kTune);
    const lean::Setup s = lean::ReadSetup("[aids]\nautoriderlrlean=0\nautoriderfblean=0\n", c);

    CHECK(s.lr.bind.input == stance::IN_PAD_AXIS, "left/right is an axis");
    CHECK(s.lr.bind.index == 3, "left/right is axis 3, got %d", s.lr.bind.index);
    CHECK(s.lr.bind.sign == 0, "left/right runs the normal way, got %d", s.lr.bind.sign);
    CHECK(s.fb.bind.index == 4, "forward/back is axis 4, got %d", s.fb.bind.index);
    CHECK(s.fb.bind.sign == 1, "forward/back is turned round, got %d", s.fb.bind.sign);
    CHECK(s.lr.bind.device == kPad, "the pad's GUID is kept as written");
    CHECK(s.lr.bind.has_tuning, "the ten tuning values are kept");
    CHECK(std::fabs(s.lr.bind.tuning[0] - 0.02f) < 1e-6f, "the deadzone is the first of them, got %f",
          double(s.lr.bind.tuning[0]));
    CHECK(s.lr.aid == stance::FLAG_OFF && s.fb.aid == stance::FLAG_OFF, "neither aid is on");

    // The bike's own lean control is a different control and is not one of these.
    CHECK(stance::FindBind(c, "CTRL_LEAN").input == stance::IN_NONE, "CTRL_LEAN is the bike, not the rider");
}

// A keyboard rider binds one key per direction. Sit has one side and always did; these have
// two, and the second used to be dropped on the floor.
static void two_sided_keys() {
    const std::string c = Controls(std::string("CTRL_LRLEAN KEY 30 32 ") + kTune,
                                   std::string("CTRL_FBLEAN KEY 17 31 ") + kTune);
    const lean::Setup s = lean::ReadSetup("[aids]\nautoriderlrlean=0\nautoriderfblean=0\n", c);
    CHECK(s.lr.bind.input == stance::IN_KEY, "a key bind");
    CHECK(s.lr.bind.index == 30 && s.lr.bind.index2 == 32, "both sides, got %d and %d", s.lr.bind.index,
          s.lr.bind.index2);
    CHECK(lean::Rate(s.lr, stance::SRC_KEYBOARD) == stance::CONF_SURE, "two keys and no aid is a sure reading");

    // Sit still reads exactly as it did: one code, and no second.
    const std::string sit = std::string("CTRL_SIT KEY 57 ") + kTune + "\n";
    const stance::Bind b = stance::FindBind(sit, "CTRL_SIT");
    CHECK(b.input == stance::IN_KEY && b.index == 57 && b.index2 == -1, "Sit is unchanged, got %d/%d", b.index,
          b.index2);
}

// The game moving the rider is the end of the reading, the way autoridersit is for Sit.
static void aids_and_confidence() {
    const std::string c = Controls(std::string("CTRL_LRLEAN AXIS ") + kPad + " 3 0 " + kTune,
                                   std::string("CTRL_FBLEAN AXIS ") + kPad + " 4 0 " + kTune);

    const lean::Setup on = lean::ReadSetup("[aids]\nautoriderlrlean=1\nautoriderfblean=0\n", c);
    CHECK(lean::Rate(on.lr, stance::SRC_DIRECTINPUT) == stance::CONF_NONE, "the aid moves the rider: no reading");
    CHECK(lean::Rate(on.fb, stance::SRC_DIRECTINPUT) == stance::CONF_SURE, "the other axis is unaffected");

    const lean::Setup unknown = lean::ReadSetup("", c);
    CHECK(lean::Rate(unknown.lr, stance::SRC_DIRECTINPUT) == stance::CONF_GUESS,
          "an aid we could not read is only a guess");

    // A stick over XInput is a guess at which pad and which stick, so it is no reading at all.
    const lean::Setup off = lean::ReadSetup("[aids]\nautoriderlrlean=0\nautoriderfblean=0\n", c);
    CHECK(lean::Rate(off.lr, stance::SRC_XINPUT) == stance::CONF_NONE, "an axis needs DirectInput");
    CHECK(lean::Rate(off.lr, stance::SRC_KEYBOARD) == stance::CONF_NONE, "an axis is not on the keyboard");

    // A .dli plugin controller can't be seen at all.
    const std::string dli = Controls(std::string("CTRL_LRLEAN C_AXIS 1 0 ") + kTune,
                                     std::string("CTRL_FBLEAN C_AXIS 1 1 ") + kTune);
    const lean::Setup plug = lean::ReadSetup("[aids]\nautoriderlrlean=0\n", dli);
    CHECK(lean::Rate(plug.lr, stance::SRC_DIRECTINPUT) == stance::CONF_NONE, "a plugin controller is unreadable");

    // Unbound reads as nothing rather than as centred.
    const lean::Setup none = lean::ReadSetup("[aids]\nautoriderlrlean=0\n", Controls("", ""));
    CHECK(lean::Rate(none.lr, stance::SRC_DIRECTINPUT) == stance::CONF_NONE, "unbound is not centred");
}

static void axis_to_number() {
    // A device set to report 0..65535, centred.
    CHECK(std::fabs(lean::FromAxis(32767, 0, 65535, 0)) < 0.01f, "centred is nothing");
    CHECK(lean::FromAxis(65535, 0, 65535, 0) > 0.99f, "hard one way is +1");
    CHECK(lean::FromAxis(0, 0, 65535, 0) < -0.99f, "hard the other is -1");
    // The line's own direction flag turns it round, as the game does.
    CHECK(lean::FromAxis(65535, 0, 65535, 1) < -0.99f, "the sign flag reverses it");
    // Out of range is clamped rather than believed.
    CHECK(lean::FromAxis(99999, 0, 65535, 0) <= 1.0f, "a reading past the range is clamped");
    CHECK(!lean::IsKnown(lean::FromAxis(10, 0, 0, 0)), "a range we were never given is unknown");

    // Two sides, and both at once is the middle.
    CHECK(lean::FromPair(false, true) > 0.99f, "one way");
    CHECK(lean::FromPair(true, false) < -0.99f, "the other");
    CHECK(std::fabs(lean::FromPair(true, true)) < 0.01f, "both held is neither");
    CHECK(std::fabs(lean::FromPair(false, false)) < 0.01f, "neither held is neither");
}

// The bytes are a contract with the Rust reader: sizes and field places are pinned here.
static void record_bytes() {
    CHECK(lean::kBindSize == 196, "LEAN_BIND is 196 bytes, got %zu", lean::kBindSize);
    CHECK(lean::kEventSize == 16, "LEAN is 16 bytes, got %zu", lean::kEventSize);
    CHECK(coachrec::LEAN_BIND == 16 && coachrec::LEAN == 17, "the tags the reader looks for");

    const std::string c = Controls(std::string("CTRL_LRLEAN AXIS ") + kPad + " 3 1 " + kTune,
                                   std::string("CTRL_FBLEAN KEY 17 31 ") + kTune);
    const lean::Setup s = lean::ReadSetup("[aids]\nautoriderlrlean=0\nautoriderfblean=0\n", c);

    uint8_t buf[lean::kBindSize];
    lean::WriteBind(buf, s, stance::SRC_DIRECTINPUT);
    CHECK(buf[0] == lean::kBindLayout, "the layout version leads");

    const uint8_t* lr = buf + 4;
    CHECK(lr[0] == stance::IN_PAD_AXIS, "left/right is an axis");
    CHECK(lr[1] == stance::FLAG_OFF, "its aid is off");
    CHECK(lr[2] == stance::CONF_SURE, "and so it is a sure reading");
    CHECK(lr[3] == stance::SRC_DIRECTINPUT, "read through DirectInput");
    int32_t nums[3];
    std::memcpy(nums, lr + 4, sizeof(nums));
    CHECK(nums[0] == 3 && nums[1] == -1 && nums[2] == 1, "axis 3, one side, reversed: got %d/%d/%d", nums[0],
          nums[1], nums[2]);
    float tune[10];
    std::memcpy(tune, lr + 16, sizeof(tune));
    CHECK(std::fabs(tune[0] - 0.02f) < 1e-6f, "the tuning values ride along");
    CHECK(std::memcmp(lr + 56, kPad, std::strlen(kPad)) == 0, "the device GUID as written");

    // The second axis sits one axis further in, and a key bind carries no device.
    const uint8_t* fb = buf + 4 + lean::kAxisSize;
    CHECK(fb[0] == stance::IN_KEY, "forward/back is on the keyboard");
    std::memcpy(nums, fb + 4, sizeof(nums));
    CHECK(nums[0] == 17 && nums[1] == 31, "both of its keys: got %d/%d", nums[0], nums[1]);
    CHECK(fb[56] == 0, "a key has no device");

    uint8_t ev[lean::kEventSize];
    lean::WriteEvent(ev, 12.5f, 0.25f, -0.5f, lean::Unknown());
    float v[4];
    std::memcpy(v, ev, sizeof(v));
    CHECK(std::fabs(v[0] - 12.5f) < 1e-6f && std::fabs(v[1] - 0.25f) < 1e-6f, "time and lap position lead");
    CHECK(std::fabs(v[2] + 0.5f) < 1e-6f, "then left/right");
    CHECK(!lean::IsKnown(v[3]), "an axis with nothing to say is unknown, not centred");
}

int main() {
    bindings();
    two_sided_keys();
    aids_and_confidence();
    axis_to_number();
    record_bytes();
    if (g_failures) {
        std::printf("lean: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("lean: all checks passed\n");
    return 0;
}
