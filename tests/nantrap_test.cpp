// nantrap=1's pure half (src/nantrap.h): the MXCSR arithmetic, the cfg keys, the sim-step
// signature, the opcode hint and the report layout. Runs anywhere; the live trap is proved on
// Windows by tests/nantrap_harness.cpp.

#include "../src/nantrap.h"
#include "../src/offsets.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
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

using namespace nantrap;

static void collect(void* user, const char* line) {
    auto* s = (std::string*)user;
    *s += line;
    *s += "\n";
}

// The same rule as frostmod.cpp's MatchAt, so the signature is checked the way it is used.
static bool match(const uint8_t* p, const char* pat, const char* mask) {
    for (; *mask; ++mask, ++p, ++pat)
        if (*mask == 'x' && (uint8_t)*pat != *p) return false;
    return true;
}

static void mxcsr() {
    // The Windows default: all masked, round to nearest, no flags.
    CHECK(ArmedMxcsr(0x1F80) == 0x1F00, "default -> only IM cleared (got %08X)", ArmedMxcsr(0x1F80));
    // Sticky flags cleared; rounding, FTZ (bit 15), DAZ (bit 6) and the other masks kept.
    CHECK(ArmedMxcsr(0x9FFF) == 0x9F40, "flags cleared, FTZ/DAZ kept (got %08X)", ArmedMxcsr(0x9FFF));
    CHECK(ArmedMxcsr(0x7F80) == 0x7F00, "rounding mode kept");
    CHECK(IsArmed(ArmedMxcsr(0x1F80)) && !IsArmed(0x1F80), "IsArmed");
    // Resume: IM back on, IE cleared, everything else as the trap found it.
    CHECK(ResumeMxcsr(0x1F01) == 0x1F80, "resume masks and clears (got %08X)", ResumeMxcsr(0x1F01));
    CHECK(ResumeMxcsr(0x9F41) == 0x9FC0, "resume keeps FTZ/DAZ (got %08X)", ResumeMxcsr(0x9F41));
    CHECK(!IsArmed(ResumeMxcsr(ArmedMxcsr(0x1F80))), "resume disarms");
    CHECK(IsTrapCode(0xC0000090u) && IsTrapCode(0xC00002B5u), "trap codes");
    CHECK(!IsTrapCode(0xC0000005u) && !IsTrapCode(0xC000008Eu), "not AV / div-by-zero");
}

static void cfg() {
    Config c;
    CHECK(!c.on && c.max == 1, "default is off, one report");
    CHECK(ParseCfgLine("nantrap=1\n", c) && c.on, "nantrap=1 turns it on");
    CHECK(ParseCfgLine("nantrap=0\n", c) && !c.on, "nantrap=0 turns it off");
    c.on = false;
    CHECK(ParseCfgLine("nantrap=2\n", c) && !c.on, "only exactly 1 is on");
    CHECK(ParseCfgLine("  nantrap = 1\r\n", c) && c.on, "spaces and CRLF");
    c = Config{};
    CHECK(!ParseCfgLine("# nantrap=1\n", c) && !c.on, "a comment is not a setting");
    CHECK(!ParseCfgLine("nantrap=\n", c) && !c.on, "no value");
    CHECK(!ParseCfgLine("nantrapx=1\n", c) && !c.on, "another key");
    CHECK(!ParseCfgLine("memdiag=1\n", c), "someone else's key");
    CHECK(ParseCfgLine("nantrapmax=3\n", c) && c.max == 3 && !c.on, "nantrapmax does not turn it on");
    CHECK(ParseCfgLine("nantrapmax=99\n", c) && c.max == kMaxReportsCap, "max clamped high");
    CHECK(ParseCfgLine("nantrapmax=0\n", c) && c.max == 1, "max clamped low");
    CHECK(ParseCfgLine("nantrapmax=-4\n", c) && c.max == 1, "negative max");
    CHECK(!ParseCfgLine(nullptr, c), "null line");
}

static void signature() {
    // The first 43 bytes of 0x1BE3A0 in beta21e, transcribed from the disassembly on its own so
    // an edit to offsets.h is noticed here.
    static const uint8_t kBeta21e[] = {
        0x40, 0x55, 0x48, 0x83, 0xEC, 0x50, 0xFF, 0xC9, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x0F, 0x28,
        0xF1, 0x83, 0xF9, 0x02, 0x0F, 0x87, 0xD2, 0x04, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x60,
        0x48, 0x89, 0x74, 0x24, 0x68, 0x48, 0x89, 0x7C, 0x24, 0x70, 0x48, 0x63, 0xF9};
    CHECK(mxb::RVA_SIM_STEP == 0x1BE3A0, "rva");
    CHECK(std::strlen(mxb::SIG_SIM_STEP_MASK) == sizeof(mxb::SIG_SIM_STEP) - 1,
          "mask and signature the same length");
    CHECK(std::strlen(mxb::SIG_SIM_STEP_MASK) == sizeof(kBeta21e), "signature covers the prologue");
    CHECK(match(kBeta21e, mxb::SIG_SIM_STEP, mxb::SIG_SIM_STEP_MASK), "beta21e matches");
    // A shifted jump target is still the same function.
    uint8_t moved[sizeof(kBeta21e)];
    std::memcpy(moved, kBeta21e, sizeof(moved));
    moved[21] = 0x10; moved[22] = 0x05;
    CHECK(match(moved, mxb::SIG_SIM_STEP, mxb::SIG_SIM_STEP_MASK), "rel32 is masked");
    // Any other change is a different function: the hook must skip and log.
    for (size_t i = 0; i < sizeof(kBeta21e); ++i) {
        if (mxb::SIG_SIM_STEP_MASK[i] != 'x') continue;
        std::memcpy(moved, kBeta21e, sizeof(moved));
        moved[i] ^= 0x01;
        CHECK(!match(moved, mxb::SIG_SIM_STEP, mxb::SIG_SIM_STEP_MASK), "byte %zu changed", i);
    }
    // MinHook's jmp at the prologue is what a second hook would leave.
    std::memcpy(moved, kBeta21e, sizeof(moved));
    moved[0] = 0xE9;
    CHECK(mxb::LooksDetoured(moved) && !mxb::LooksDetoured(kBeta21e), "detour seen");
}

static void ophint() {
    char out[64];
    const uint8_t divsd[] = {0xF2, 0x45, 0x0F, 0x5E, 0xE9};   // divsd xmm13, xmm9
    DescribeOp(divsd, sizeof(divsd), out, sizeof(out));
    CHECK(std::strcmp(out, "divsd xmm13, xmm9") == 0, "divsd: %s", out);
    const uint8_t divss[] = {0xF3, 0x0F, 0x5E, 0x9F, 0x58, 0x07, 0x00, 0x00};   // divss xmm3,[rdi+758]
    DescribeOp(divss, sizeof(divss), out, sizeof(out));
    CHECK(std::strcmp(out, "divss xmm3, [mem]") == 0, "divss: %s", out);
    const uint8_t cvtt[] = {0xF3, 0x44, 0x0F, 0x2C, 0xDB};   // cvttss2si r11d, xmm3
    DescribeOp(cvtt, sizeof(cvtt), out, sizeof(out));
    CHECK(std::strcmp(out, "cvttss2si r11, xmm3") == 0, "cvttss2si: %s", out);
    const uint8_t comiss[] = {0x0F, 0x2F, 0xCB};
    DescribeOp(comiss, sizeof(comiss), out, sizeof(out));
    CHECK(std::strcmp(out, "comiss xmm1, xmm3") == 0, "comiss: %s", out);
    const uint8_t sqrtsd[] = {0xF2, 0x0F, 0x51, 0xC0};
    DescribeOp(sqrtsd, sizeof(sqrtsd), out, sizeof(out));
    CHECK(std::strcmp(out, "sqrtsd xmm0, xmm0") == 0, "sqrtsd: %s", out);
    const uint8_t mulps[] = {0x0F, 0x59, 0xC1};
    DescribeOp(mulps, sizeof(mulps), out, sizeof(out));
    CHECK(std::strcmp(out, "mulps xmm0, xmm1") == 0, "mulps: %s", out);
    const uint8_t vex[] = {0xC5, 0xFA, 0x5E, 0xC1};
    DescribeOp(vex, sizeof(vex), out, sizeof(out));
    CHECK(std::strcmp(out, "AVX (VEX) op") == 0, "vex: %s", out);
    const uint8_t junk[] = {0x90};
    DescribeOp(junk, sizeof(junk), out, sizeof(out));
    CHECK(std::strcmp(out, "unknown op") == 0, "junk: %s", out);
    DescribeOp(junk, 0, out, sizeof(out));
    CHECK(std::strcmp(out, "unknown op") == 0, "empty: %s", out);
}

static Record sample() {
    Record r;
    r.code = kStatusInvalid;
    r.rip = 0x1401A2E60;
    std::snprintf(r.site, sizeof(r.site), "mxbikes.exe+0x1A2E60");
    std::snprintf(r.gameSite, sizeof(r.gameSite), "mxbikes.exe+0x1A2E60");
    r.mxcsr = 0x1F00;
    const uint8_t before[] = {0x0F, 0x28, 0xC1};
    const uint8_t at[] = {0xF2, 0x45, 0x0F, 0x5E, 0xE9, 0x90};
    std::memcpy(r.bytes, before, sizeof(before));
    std::memcpy(r.bytes + sizeof(before), at, sizeof(at));
    r.before = (int)sizeof(before);
    r.after = (int)sizeof(at);
    for (int i = 0; i < 16; ++i) r.gpr[i] = 0x1000u + (uint64_t)i;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float f0[4] = {0.0f, 1.5f, nan, -std::numeric_limits<float>::infinity()};
    std::memcpy(r.xmm[0], f0, 16);
    const double d1 = 2.5e29;
    std::memcpy(r.xmm[1], &d1, 8);
    std::snprintf(r.frames[0], sizeof(r.frames[0]), "mxbikes.exe+0x1A2E60");
    std::snprintf(r.frames[1], sizeof(r.frames[1]), "mxbikes.exe+0x1AE972");
    r.frameCount = 2;
    r.uptimeMs = 123456;
    r.steps = 98765;
    std::snprintf(r.track, sizeof(r.track), "%s", "forest \"mx\"");
    return r;
}

static void report() {
    const Record r = sample();
    std::string log;
    WriteLog(r, collect, &log);
    CHECK(log.find("[nantrap] FIRST invalid FP op #1 (0xC0000090) at mxbikes.exe+0x1A2E60: divsd xmm13, xmm9") != std::string::npos,
          "headline:\n%s", log.c_str());
    CHECK(log.find("bytes (rip after the bar): 0F 28 C1 | F2 45 0F 5E E9 90") != std::string::npos, "bytes:\n%s", log.c_str());
    CHECK(log.find("xmm0 f=[0 1.5 nan -inf]") != std::string::npos, "xmm0 floats:\n%s", log.c_str());
    CHECK(log.find("lo double=2.5e+29") != std::string::npos, "xmm1 double:\n%s", log.c_str());
    CHECK(log.find("r13=000000000000100D") != std::string::npos, "gpr r13:\n%s", log.c_str());
    CHECK(log.find("#1  mxbikes.exe+0x1AE972") != std::string::npos, "stack:\n%s", log.c_str());
    CHECK(log.find("bike='<unknown>'") != std::string::npos, "bike unknown said so");
    CHECK(log.find("nearest game frame") == std::string::npos, "no game-frame line when it is the site");

    char notes[8][crash::kCrumbLen];
    const int n = TrailNotes(r, notes, 8);
    CHECK(n == 6, "six trail notes (got %d)", n);
    CHECK(std::strstr(notes[0], "mxbikes.exe+0x1A2E60 divsd") != nullptr, "note 0: %s", notes[0]);
    for (int i = 0; i < n; ++i)
        CHECK(std::strlen(notes[i]) < (size_t)crash::kCrumbLen, "note %d fits", i);

    crash::Context ctx;
    ctx.SetServer("Some Server");
    ctx.inSession.store(true);
    crash::Trail trail;
    trail.Add(100, notes[0]);
    Meta m;
    m.version = "0.49.10";
    m.game = "mxbikes.exe";
    m.whenUtc = "2026-10-08T12:00:00Z";
    std::string js;
    WriteJson(r, m, ctx, trail, 200, collect, &js);
    CHECK(js.find("\"kind\": \"nan_first_fault\"") != std::string::npos, "kind:\n%s", js.c_str());
    CHECK(js.find("\"site\": \"mxbikes.exe+0x1A2E60\"") != std::string::npos, "site");
    CHECK(js.find("\"code\": \"0xC0000090\"") != std::string::npos, "code");
    CHECK(js.find("\"dump\": \"\"") != std::string::npos, "no dump");
    CHECK(js.find("\"f\": [0, 1.5, \"nan\", \"-inf\"]") != std::string::npos, "non-finite as strings:\n%s", js.c_str());
    CHECK(js.find("\"op\": \"divsd xmm13, xmm9\"") != std::string::npos, "op");
    CHECK(js.find("\"track\": \"forest \\\"mx\\\"\"") != std::string::npos, "track escaped:\n%s", js.c_str());
    CHECK(js.find("\"inSession\": true") != std::string::npos, "session");
    CHECK(js.find("\"beforeMs\": 100") != std::string::npos, "trail");
    // No identity fields: the app adds the account, never the DLL.
    CHECK(js.find("guid") == std::string::npos && js.find("steam") == std::string::npos, "no ids");
    // Balanced braces and brackets, and no bare nan/inf a JSON parser would reject.
    int depth = 0, minDepth = 0;
    bool inStr = false;
    for (size_t i = 0; i < js.size(); ++i) {
        const char ch = js[i];
        if (ch == '"' && (i == 0 || js[i - 1] != '\\')) inStr = !inStr;
        if (inStr) continue;
        if (ch == '{' || ch == '[') ++depth;
        if (ch == '}' || ch == ']') --depth;
        if (depth < minDepth) minDepth = depth;
        if (std::isalpha((unsigned char)ch) && js.compare(i, 4, "true") && js.compare(i, 5, "false") &&
            js.compare(i, 4, "null") && !(i > 0 && (std::isdigit((unsigned char)js[i - 1]) || js[i - 1] == '.') && (ch == 'e' || ch == 'E')))
            CHECK(false, "bare word at %zu: %.12s", i, js.c_str() + i);
        if (!js.compare(i, 4, "true")) i += 3;
        else if (!js.compare(i, 5, "false")) i += 4;
        else if (!js.compare(i, 4, "null")) i += 3;
    }
    CHECK(depth == 0 && minDepth == 0 && !inStr, "balanced JSON");

    // A trap outside the game names the nearest game frame too.
    Record crt = r;
    std::snprintf(crt.site, sizeof(crt.site), "MSVCR90.dll+0x3A1B0");
    std::snprintf(crt.gameSite, sizeof(crt.gameSite), "mxbikes.exe+0x1A3FBD");
    log.clear();
    WriteLog(crt, collect, &log);
    CHECK(log.find("nearest game frame: mxbikes.exe+0x1A3FBD") != std::string::npos, "game frame:\n%s", log.c_str());
}

int main() {
    mxcsr();
    cfg();
    signature();
    ophint();
    report();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nantrap: all passed\n");
    return 0;
}
