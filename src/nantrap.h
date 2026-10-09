// nantrap=1 in frostmod_radar.cfg: catch the FIRST invalid floating-point operation in MX Bikes'
// physics step and say where it was. Diagnostic only, off by default.
//
// Why: about a third of the crashes we collect are a NaN in the bike's sim state (the
// "refused a NaN height query" trail, then +0x1A29AE / +0x1F1923). Every crash site we have is
// a CONSUMER of the NaN. The producer has long since returned by the time anything faults,
// and no dump can name it. See research/nan-root-cause.md on the Windows PC.
//
// How: x86 SSE raises #XM for an invalid operation (0/0, inf-inf, 0*inf, sqrt(-x), a NaN into
// comiss or cvttss2si) only when MXCSR.IM is clear. The game runs with it set, like every
// Windows program. So around each call of the sim step (0x1BE3A0) the trap saves MXCSR,
// clears the sticky flags, unmasks ONLY IM, calls the step, and restores MXCSR exactly. The
// first invalid op inside the step faults; a vectored handler records RIP, the bytes there,
// xmm0-7 and the integer registers, and a short stack, then masks IM in the faulting context
// and resumes the same instruction - which now produces the default NaN, exactly as stock.
// Arithmetic on a NaN that already exists does not raise invalid (only comparisons and
// conversions do), so the first trap is the instruction that MADE the NaN.
//
// Everything here is pure (no Win32), so the arithmetic, the cfg parsing, the opcode hint and
// the report layout run in CI: tests/nantrap_test.cpp. The live half is src/nantrap.cpp, and
// tests/nantrap_harness.cpp proves it on a real 0/0 on Windows.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "crashreport.h"

namespace nantrap {

namespace crash = frostmod::crash;

// ---- MXCSR -------------------------------------------------------------------------------
inline constexpr uint32_t kIE    = 1u << 0;   // invalid-operation flag (sticky)
inline constexpr uint32_t kFlags = 0x3Fu;     // IE DE ZE OE UE PE, all sticky
inline constexpr uint32_t kIM    = 1u << 7;   // invalid-operation mask
inline constexpr uint32_t kStatusInvalid  = 0xC0000090u;  // STATUS_FLOAT_INVALID_OPERATION
inline constexpr uint32_t kStatusMultiple = 0xC00002B5u;  // STATUS_FLOAT_MULTIPLE_TRAPS

/// What the step runs under: the game's MXCSR with the sticky flags cleared and only the
/// invalid-operation exception unmasked. Rounding, FTZ/DAZ and every other mask are kept.
inline uint32_t ArmedMxcsr(uint32_t saved) { return (saved & ~kFlags) & ~kIM; }
/// What the faulting instruction is resumed under: invalid masked again, flags cleared, so it
/// completes with the default NaN as it would have on a stock game.
inline uint32_t ResumeMxcsr(uint32_t trapped) { return (trapped | kIM) & ~kFlags; }
/// Whether a context's MXCSR is one we armed (invalid unmasked). The game never unmasks it.
inline bool IsArmed(uint32_t mxcsr) { return (mxcsr & kIM) == 0; }
inline bool IsTrapCode(uint32_t code) { return code == kStatusInvalid || code == kStatusMultiple; }

// ---- frostmod_radar.cfg -------------------------------------------------------------------
inline constexpr int kMaxReportsCap = 8;     // nantrapmax= is clamped to 1..8
/// Past this many traps in a session the trap disarms even if fewer reports were written:
/// a site that faults every step would otherwise cost an exception per step for the rest of
/// the session.
inline constexpr unsigned kMaxTraps = 64;

struct Config {
    bool on = false;   // nantrap=1. Anything else, or no line at all, is off.
    int max = 1;       // nantrapmax=N: distinct sites reported per session (default 1)
};

/// One cfg line. Returns true if it was one of ours. `nantrap=1` exactly turns it on; `0`, a
/// typo, a comment or a missing line leave it off.
inline bool ParseCfgLine(const char* line, Config& c) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;
    auto value = [](const char* p, int& out) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') return false;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        if (*p < '0' || *p > '9') return false;
        long v = 0;
        while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
        out = neg ? (int)-v : (int)v;
        return true;
    };
    int v = 0;
    if (std::strncmp(line, "nantrapmax", 10) == 0) {
        if (!value(line + 10, v)) return false;
        c.max = v < 1 ? 1 : v > kMaxReportsCap ? kMaxReportsCap : v;
        return true;
    }
    if (std::strncmp(line, "nantrap", 7) == 0) {
        if (!value(line + 7, v)) return false;
        c.on = (v == 1);
        return true;
    }
    return false;
}

// ---- what the faulting instruction was ------------------------------------------------------
// A hint, not a disassembler: the SSE/SSE2 scalar and packed ops that can raise invalid, read
// off the bytes at RIP. Good enough to say "divsd xmm13, xmm9" in a log line; the bytes are
// recorded beside it for anyone who wants to check.
inline void DescribeOp(const uint8_t* b, size_t n, char* out, size_t outN) {
    if (!outN) return;
    out[0] = 0;
    size_t i = 0;
    uint8_t pfx = 0, rex = 0;
    while (i < n && (b[i] == 0x66 || b[i] == 0xF2 || b[i] == 0xF3 || b[i] == 0x2E || b[i] == 0x3E ||
                     b[i] == 0x26 || b[i] == 0x36 || b[i] == 0x64 || b[i] == 0x65)) {
        if (b[i] == 0x66 || b[i] == 0xF2 || b[i] == 0xF3) pfx = b[i];
        ++i;
    }
    if (i < n && (b[i] & 0xF0) == 0x40) rex = b[i++];
    if (i < n && (b[i] == 0xC4 || b[i] == 0xC5)) { std::snprintf(out, outN, "AVX (VEX) op"); return; }
    if (i < n && b[i] >= 0xD8 && b[i] <= 0xDF) { std::snprintf(out, outN, "x87 op"); return; }
    if (i + 1 >= n || b[i] != 0x0F) { std::snprintf(out, outN, "unknown op"); return; }
    const uint8_t op = b[i + 1];
    const char* sfx = pfx == 0xF3 ? "ss" : pfx == 0xF2 ? "sd" : pfx == 0x66 ? "pd" : "ps";
    char name[24] = "";
    bool intDest = false;
    switch (op) {
        case 0x51: std::snprintf(name, sizeof(name), "sqrt%s", sfx); break;
        case 0x58: std::snprintf(name, sizeof(name), "add%s", sfx); break;
        case 0x59: std::snprintf(name, sizeof(name), "mul%s", sfx); break;
        case 0x5C: std::snprintf(name, sizeof(name), "sub%s", sfx); break;
        case 0x5D: std::snprintf(name, sizeof(name), "min%s", sfx); break;
        case 0x5E: std::snprintf(name, sizeof(name), "div%s", sfx); break;
        case 0x5F: std::snprintf(name, sizeof(name), "max%s", sfx); break;
        case 0xC2: std::snprintf(name, sizeof(name), "cmp%s", sfx); break;
        case 0x2E: std::snprintf(name, sizeof(name), "%s", pfx == 0x66 ? "ucomisd" : "ucomiss"); break;
        case 0x2F: std::snprintf(name, sizeof(name), "%s", pfx == 0x66 ? "comisd" : "comiss"); break;
        case 0x2C:
            std::snprintf(name, sizeof(name), "%s", pfx == 0xF3 ? "cvttss2si" : pfx == 0xF2 ? "cvttsd2si"
                                                    : pfx == 0x66 ? "cvttpd2pi" : "cvttps2pi");
            intDest = pfx == 0xF3 || pfx == 0xF2;
            break;
        case 0x2D:
            std::snprintf(name, sizeof(name), "%s", pfx == 0xF3 ? "cvtss2si" : pfx == 0xF2 ? "cvtsd2si"
                                                    : pfx == 0x66 ? "cvtpd2pi" : "cvtps2pi");
            intDest = pfx == 0xF3 || pfx == 0xF2;
            break;
        case 0x5A:
            std::snprintf(name, sizeof(name), "%s", pfx == 0xF3 ? "cvtss2sd" : pfx == 0xF2 ? "cvtsd2ss"
                                                    : pfx == 0x66 ? "cvtpd2ps" : "cvtps2pd");
            break;
        case 0x5B:
            std::snprintf(name, sizeof(name), "%s", pfx == 0x66 ? "cvtps2dq" : pfx == 0xF3 ? "cvttps2dq"
                                                                                       : "cvtdq2ps");
            break;
        case 0xE6:
            std::snprintf(name, sizeof(name), "%s", pfx == 0xF3 ? "cvtdq2pd" : pfx == 0xF2 ? "cvtpd2dq"
                                                                                       : "cvttpd2dq");
            break;
        case 0x38: case 0x3A: std::snprintf(out, outN, "SSE4 op (0F %02X)", op); return;
        default: std::snprintf(out, outN, "0F %02X", op); return;
    }
    if (i + 2 >= n) { std::snprintf(out, outN, "%s", name); return; }
    const uint8_t modrm = b[i + 2];
    const int reg = ((modrm >> 3) & 7) | ((rex & 0x4) ? 8 : 0);
    const int rm  = (modrm & 7) | ((rex & 0x1) ? 8 : 0);
    char dst[8], src[8];
    if (intDest) std::snprintf(dst, sizeof(dst), "r%d", reg);
    else         std::snprintf(dst, sizeof(dst), "xmm%d", reg);
    if ((modrm >> 6) == 3) std::snprintf(src, sizeof(src), "xmm%d", rm);
    else                   std::snprintf(src, sizeof(src), "[mem]");
    std::snprintf(out, outN, "%s %s, %s", name, dst, src);
}

// ---- the record ------------------------------------------------------------------------------
inline constexpr int kBytesBefore = 16;
inline constexpr int kBytesAfter  = 16;
inline constexpr int kFrames      = 12;

struct Record {
    uint32_t code = 0;
    uint64_t rip = 0;
    char site[160] = "";       // "mxbikes.exe+0x1A2E60": the faulting instruction
    char gameSite[160] = "";   // the nearest frame inside the game's exe (same as site if it is)
    uint32_t mxcsr = 0;        // as the trap saw it
    uint8_t bytes[kBytesBefore + kBytesAfter] = {};
    int before = 0;            // readable bytes before rip in `bytes` (<= kBytesBefore)
    int after = 0;             // readable bytes from rip on
    uint64_t gpr[16] = {};     // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint8_t xmm[8][16] = {};   // xmm0-7, raw
    char frames[kFrames][160] = {};
    int frameCount = 0;
    unsigned long long uptimeMs = 0;   // process clock (crash::ElapsedMs)
    unsigned long long steps = 0;      // sim steps run armed before this one
    unsigned index = 1;                // 1-based report number this session
    unsigned long thread = 0;
    char track[96] = "";
    char bike[96] = "";                // empty: FrostMod does not know the bike yet
};

inline const char* const kGprNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

inline float XmmF(const Record& r, int i, int lane) {
    float f; std::memcpy(&f, &r.xmm[i][lane * 4], 4); return f;
}
inline double XmmD(const Record& r, int i) {
    double d; std::memcpy(&d, &r.xmm[i][0], 8); return d;
}

/// The instruction at rip, as a hint ("divsd xmm13, xmm9").
inline void OpHint(const Record& r, char* out, size_t n) {
    DescribeOp(r.bytes + r.before, (size_t)r.after, out, n);
}

/// Hex of the recorded bytes with a bar before rip: "... 0F 28 C1 | F2 44 0F 5E E9 ...".
inline void BytesHex(const Record& r, char* out, size_t n) {
    size_t w = 0;
    out[0] = 0;
    const int total = r.before + r.after;
    for (int i = 0; i < total && w + 4 < n; ++i) {
        if (i == r.before && i > 0 && w + 2 < n) { out[w++] = '|'; out[w++] = ' '; }
        w += (size_t)std::snprintf(out + w, n - w, "%02X ", r.bytes[i]);
    }
    if (w && out[w - 1] == ' ') out[w - 1] = 0;
}

/// A float the way a log line wants it: %g, and nan/inf spelled out.
inline void Num(double v, char* out, size_t n) {
    if (std::isnan(v)) std::snprintf(out, n, "nan");
    else if (std::isinf(v)) std::snprintf(out, n, v > 0 ? "inf" : "-inf");
    else std::snprintf(out, n, "%.6g", v);
}

/// The text report for frostmod.log. Prefix "[nantrap]".
inline void WriteLog(const Record& r, crash::Emit emit, void* user) {
    char line[512], op[64], hex[160];
    OpHint(r, op, sizeof(op));
    std::snprintf(line, sizeof(line),
                  "[nantrap] FIRST invalid FP op #%u (0x%08X) at %s: %s - thread %lu, %llu ms "
                  "into the run, %llu armed steps in",
                  r.index, r.code, r.site, op, r.thread, r.uptimeMs, r.steps);
    emit(user, line);
    if (r.gameSite[0] && std::strcmp(r.gameSite, r.site) != 0) {
        std::snprintf(line, sizeof(line), "[nantrap] nearest game frame: %s", r.gameSite);
        emit(user, line);
    }
    BytesHex(r, hex, sizeof(hex));
    std::snprintf(line, sizeof(line), "[nantrap] bytes (rip after the bar): %s", hex);
    emit(user, line);
    std::snprintf(line, sizeof(line), "[nantrap] mxcsr=%08X track='%s' bike='%s'", r.mxcsr,
                  r.track[0] ? r.track : "<none>", r.bike[0] ? r.bike : "<unknown>");
    emit(user, line);
    for (int i = 0; i < 8; ++i) {
        char f[4][24], d[24];
        for (int l = 0; l < 4; ++l) Num(XmmF(r, i, l), f[l], sizeof(f[l]));
        Num(XmmD(r, i), d, sizeof(d));
        std::snprintf(line, sizeof(line), "[nantrap] xmm%d f=[%s %s %s %s] lo double=%s", i, f[0],
                      f[1], f[2], f[3], d);
        emit(user, line);
    }
    for (int i = 0; i < 16; i += 4) {
        std::snprintf(line, sizeof(line), "[nantrap] %-3s=%016llX %-3s=%016llX %-3s=%016llX %-3s=%016llX",
                      kGprNames[i], (unsigned long long)r.gpr[i], kGprNames[i + 1],
                      (unsigned long long)r.gpr[i + 1], kGprNames[i + 2],
                      (unsigned long long)r.gpr[i + 2], kGprNames[i + 3],
                      (unsigned long long)r.gpr[i + 3]);
        emit(user, line);
    }
    std::snprintf(line, sizeof(line), "[nantrap] stack (nearest first, %d frame(s)):", r.frameCount);
    emit(user, line);
    for (int i = 0; i < r.frameCount; ++i) {
        std::snprintf(line, sizeof(line), "[nantrap]   #%-2d %s", i, r.frames[i]);
        emit(user, line);
    }
}

/// Short notes for the crash trail (each under crash::kCrumbLen). They ride along in any
/// crash report that follows, and in this report's own JSON trail, which the control plane
/// stores today without a schema change.
inline int TrailNotes(const Record& r, char (*out)[crash::kCrumbLen], int max) {
    int k = 0;
    char op[64];
    OpHint(r, op, sizeof(op));
    if (k < max) std::snprintf(out[k++], crash::kCrumbLen, "nantrap: first invalid op at %s %s", r.site, op);
    for (int base = 0; base < 8 && k < max; base += 2) {
        char a[24], b[24], c[24], d[24];
        Num(XmmF(r, base, 0), a, sizeof(a));
        Num(XmmD(r, base), b, sizeof(b));
        Num(XmmF(r, base + 1, 0), c, sizeof(c));
        Num(XmmD(r, base + 1), d, sizeof(d));
        std::snprintf(out[k++], crash::kCrumbLen, "nantrap: xmm%d f=%s d=%s | xmm%d f=%s d=%s", base, a,
                      b, base + 1, c, d);
    }
    if (k < max) {
        char hex[160];
        BytesHex(r, hex, sizeof(hex));
        // The tail end of the bytes is what matters: rip sits after the bar, near the middle.
        const char* bar = std::strchr(hex, '|');
        std::snprintf(out[k++], crash::kCrumbLen, "nantrap: bytes %s", bar ? (bar - hex > 24 ? bar - 24 : hex) : hex);
    }
    return k;
}

/// JSON number, or a string for the values JSON has no spelling for.
inline void JsonNum(double v, char* out, size_t n) {
    if (std::isnan(v)) std::snprintf(out, n, "\"nan\"");
    else if (std::isinf(v)) std::snprintf(out, n, v > 0 ? "\"inf\"" : "\"-inf\"");
    else std::snprintf(out, n, "%.9g", v);
}

struct Meta {
    const char* version = "";   // FrostMod's
    const char* game = "";      // the exe
    const char* whenUtc = "";
    const char* ringFile = "";  // tyrelog's CSV beside this, or empty (key left out)
};

/// The sidecar MXB App posts: the crash-report shape (so the existing /v1/diagnostics/crash
/// path takes it as is) with fault.kind "nan_first_fault", plus a "nan" object with the
/// registers and bytes. The game has not crashed; there is no dump.
inline void WriteJson(const Record& r, const Meta& m, const crash::Context& ctx,
                      const crash::Trail& trail, unsigned long long nowMs, crash::Emit emit,
                      void* user) {
    char line[640], esc[256], tmp[200];
    emit(user, "{");
    emit(user, "  \"schema\": 1,");
    crash::Escape(m.version, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"frostmod\": \"%s\",", esc); emit(user, line);
    crash::Escape(m.game, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"game\": \"%s\",", esc); emit(user, line);
    crash::Escape(m.whenUtc, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"when\": \"%s\",", esc); emit(user, line);
    std::snprintf(line, sizeof(line), "  \"uptimeMs\": %llu,", r.uptimeMs); emit(user, line);
    emit(user, "  \"dump\": \"\",");
    if (m.ringFile && m.ringFile[0]) {
        crash::Escape(m.ringFile, esc, sizeof(esc));
        std::snprintf(line, sizeof(line), "  \"nanRing\": \"%s\",", esc); emit(user, line);
    }

    emit(user, "  \"fault\": {");
    emit(user, "    \"kind\": \"nan_first_fault\",");
    std::snprintf(line, sizeof(line), "    \"code\": \"0x%08X\",", r.code); emit(user, line);
    crash::Escape(r.site, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"site\": \"%s\",", esc); emit(user, line);
    emit(user, "    \"access\": \"\",");
    emit(user, "    \"target\": null");
    emit(user, "  },");

    emit(user, "  \"frames\": [");
    for (int i = 0; i < r.frameCount; ++i) {
        crash::Escape(r.frames[i], esc, sizeof(esc));
        std::snprintf(line, sizeof(line), "    \"%s\"%s", esc, i + 1 < r.frameCount ? "," : "");
        emit(user, line);
    }
    emit(user, "  ],");

    emit(user, "  \"nan\": {");
    std::snprintf(line, sizeof(line), "    \"index\": %u,", r.index); emit(user, line);
    crash::Escape(r.gameSite, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"gameSite\": \"%s\",", esc); emit(user, line);
    OpHint(r, tmp, sizeof(tmp));
    crash::Escape(tmp, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"op\": \"%s\",", esc); emit(user, line);
    BytesHex(r, tmp, sizeof(tmp));
    crash::Escape(tmp, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"bytes\": \"%s\",", esc); emit(user, line);
    std::snprintf(line, sizeof(line), "    \"mxcsr\": \"0x%08X\",", r.mxcsr); emit(user, line);
    std::snprintf(line, sizeof(line), "    \"steps\": %llu,", r.steps); emit(user, line);
    crash::Escape(r.bike, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"bike\": \"%s\",", esc); emit(user, line);
    emit(user, "    \"xmm\": [");
    for (int i = 0; i < 8; ++i) {
        char f[4][32], d[32];
        for (int l = 0; l < 4; ++l) JsonNum(XmmF(r, i, l), f[l], sizeof(f[l]));
        JsonNum(XmmD(r, i), d, sizeof(d));
        std::snprintf(line, sizeof(line), "      { \"f\": [%s, %s, %s, %s], \"d\": %s }%s", f[0], f[1],
                      f[2], f[3], d, i < 7 ? "," : "");
        emit(user, line);
    }
    emit(user, "    ],");
    emit(user, "    \"gpr\": {");
    for (int i = 0; i < 16; ++i) {
        std::snprintf(line, sizeof(line), "      \"%s\": \"0x%016llX\"%s", kGprNames[i],
                      (unsigned long long)r.gpr[i], i < 15 ? "," : "");
        emit(user, line);
    }
    emit(user, "    }");
    emit(user, "  },");

    const crash::Where w = (crash::Where)ctx.where.load(std::memory_order_relaxed);
    const int riders = ctx.riders.load(std::memory_order_relaxed);
    emit(user, "  \"session\": {");
    crash::Escape(crash::WhereName(w), esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"where\": \"%s\",", esc); emit(user, line);
    std::snprintf(line, sizeof(line), "    \"inSession\": %s,",
                  ctx.inSession.load(std::memory_order_relaxed) ? "true" : "false");
    emit(user, line);
    crash::Escape(r.track[0] ? r.track : ctx.track, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"track\": \"%s\",", esc); emit(user, line);
    crash::Escape(ctx.server, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"server\": \"%s\",", esc); emit(user, line);
    if (riders >= 0) std::snprintf(line, sizeof(line), "    \"riders\": %d,", riders);
    else             std::snprintf(line, sizeof(line), "    \"riders\": null,");
    emit(user, line);
    std::snprintf(line, sizeof(line), "    \"reloads\": %u",
                  ctx.reloads.load(std::memory_order_relaxed));
    emit(user, line);
    emit(user, "  },");

    emit(user, "  \"trail\": [");
    const int n = trail.Count();
    for (int i = 0; i < n; ++i) {
        const crash::Crumb& c = trail.At(i);
        crash::Escape(c.text, esc, sizeof(esc));
        std::snprintf(line, sizeof(line), "    { \"beforeMs\": %llu, \"text\": \"%s\" }%s",
                      nowMs >= c.ms ? nowMs - c.ms : 0, esc, i + 1 < n ? "," : "");
        emit(user, line);
    }
    emit(user, "  ]");
    emit(user, "}");
}

// ---- the live half (src/nantrap.cpp) --------------------------------------------------------
#ifdef _WIN32
/// Registers the vectored handler (first in line) and arms the trap. `log` takes one line.
void Install(void (*log)(const char*), const Config& cfg);
/// Whether a step should be wrapped. False until Install, and again once the session's reports
/// are written.
bool Armed();
/// Around one call of the sim step, on the thread that runs it. Enter returns the MXCSR to hand
/// back to Leave, which restores it exactly and writes any pending report.
uint32_t Enter();
void Leave(uint32_t saved);
/// Leave without the flush: restores MXCSR and counts the step. For a caller that wants to do
/// something between the step and the report (tyrelog samples the step first), then Flush().
void Restore(uint32_t saved);
/// Writes a pending report (log + sidecar). Leave calls it; safe to call from anywhere.
void Flush();
/// For the harness: how many reports this session, and the last one.
unsigned Reports();
const Record& Last();
#endif

}  // namespace nantrap
