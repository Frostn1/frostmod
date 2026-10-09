// The live half of nantrap=1. See nantrap.h for what it is and why.
//
// Three rules this file keeps, because it runs inside the game's physics:
//   1. The game's MXCSR is restored exactly after every step, and the faulting instruction is
//      resumed with invalid masked, so it computes the same NaN a stock game computes.
//   2. Only a fault this trap armed is touched: right code, on a thread inside an armed step,
//      with IM clear in the faulting context. Anything else goes on down the chain untouched.
//   3. The handler does no I/O. It copies registers and bytes into a static record and adds a
//      few trail notes; the log lines and the sidecar are written after the step returns.

#include "nantrap.h"

#include <windows.h>
#include <xmmintrin.h>

#include <atomic>

namespace nantrap {
namespace {

void (*g_log)(const char*) = nullptr;
Config g_cfg;
std::atomic<bool> g_armed{false};
std::atomic<unsigned> g_reports{0};     // records captured this session
std::atomic<unsigned> g_traps{0};       // traps seen this session, reported or not
std::atomic<bool> g_pending{false};     // a captured record not yet written out
std::atomic<unsigned long long> g_steps{0};
PVOID g_veh = nullptr;
char g_exeLeaf[MAX_PATH] = "";
uintptr_t g_exeLo = 0, g_exeHi = 0;

// The record being filled and the ones already sent, by rip, so nantrapmax>1 reports distinct
// sites rather than the same one N times.
Record g_rec;
uint64_t g_seenRip[kMaxReportsCap] = {};

thread_local int t_depth = 0;           // >0 while this thread is inside an armed step
thread_local bool t_inHandler = false;

void Say(const char* line) { if (g_log) g_log(line); }
void SaySink(void*, const char* line) { Say(line); }

// SEH needs a frame without C++ objects in it.
bool ReadBytes(const uint8_t* p, uint8_t* out, int n) {
    __try {
        for (int i = 0; i < n; ++i) out[i] = p[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Capture(const EXCEPTION_POINTERS* ep, unsigned index) {
    const CONTEXT* c = ep->ContextRecord;
    Record& r = g_rec;
    r = Record{};
    r.code = (uint32_t)ep->ExceptionRecord->ExceptionCode;
    r.rip = (uint64_t)ep->ExceptionRecord->ExceptionAddress;
    r.mxcsr = c->MxCsr;
    r.index = index;
    r.thread = GetCurrentThreadId();
    r.uptimeMs = crash::ElapsedMs();
    r.steps = g_steps.load(std::memory_order_relaxed);
    crash::DescribeAddr((const void*)r.rip, r.site, sizeof(r.site));

    // Bytes around rip: whatever is readable, the instruction itself first.
    const uint8_t* at = (const uint8_t*)r.rip;
    for (int k = kBytesAfter; k > 0; --k)
        if (ReadBytes(at, r.bytes + kBytesBefore, k)) { r.after = k; break; }
    for (int k = kBytesBefore; k > 0; --k)
        if (ReadBytes(at - k, r.bytes + kBytesBefore - k, k)) { r.before = k; break; }
    if (r.before < kBytesBefore)   // keep `bytes` contiguous from index 0
        std::memmove(r.bytes, r.bytes + (kBytesBefore - r.before), (size_t)(r.before + r.after));

    const DWORD64 g[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                           c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
    for (int i = 0; i < 16; ++i) r.gpr[i] = g[i];
    for (int i = 0; i < 8; ++i) std::memcpy(r.xmm[i], &c->FltSave.XmmRegisters[i], 16);

    r.frameCount = crash::UnwindFrom(c, r.frames, kFrames);
    // The nearest frame in the game itself. A trap inside the CRT (pow, acos) names msvcr90;
    // the line that called it is what PiBoSo can look up.
    if (r.rip >= g_exeLo && r.rip < g_exeHi) {
        std::memcpy(r.gameSite, r.site, sizeof(r.site));
    } else {
        const size_t leafLen = std::strlen(g_exeLeaf);
        for (int i = 0; i < r.frameCount; ++i)
            if (leafLen && _strnicmp(r.frames[i], g_exeLeaf, leafLen) == 0 && r.frames[i][leafLen] == '+') {
                std::memcpy(r.gameSite, r.frames[i], sizeof(r.gameSite));
                break;
            }
    }

    // What FrostMod knows of the session. The bike is not known to this copy yet.
    const crash::Context& ctx = crash::TheContext();
    std::snprintf(r.track, sizeof(r.track), "%s", ctx.track);

    // The trail: lock-free, and it rides along in any crash report that follows seconds later.
    char notes[8][crash::kCrumbLen];
    const int n = TrailNotes(r, notes, 8);
    for (int i = 0; i < n; ++i) crash::Note("%s", notes[i]);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!IsTrapCode((uint32_t)ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    // Ours only inside an armed step on this thread, and only if the context still has IM clear.
    if (t_depth <= 0 || t_inHandler || !IsArmed(ep->ContextRecord->MxCsr))
        return EXCEPTION_CONTINUE_SEARCH;
    // This handler formats floats. Whatever MXCSR it was entered with, it runs with every
    // exception masked, so nothing in here can trap back into it. The resumed thread gets the
    // context's MXCSR, not this one.
    t_inHandler = true;
    _mm_setcsr(_mm_getcsr() | 0x1F80u);

    const unsigned traps = g_traps.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t rip = (uint64_t)ep->ExceptionRecord->ExceptionAddress;
    const unsigned have = g_reports.load(std::memory_order_relaxed);
    bool seen = false;
    for (unsigned i = 0; i < have && i < (unsigned)kMaxReportsCap; ++i) seen |= g_seenRip[i] == rip;
    if (!seen && have < (unsigned)g_cfg.max && !g_pending.load(std::memory_order_acquire)) {
        g_seenRip[have] = rip;
        Capture(ep, have + 1);
        g_reports.store(have + 1, std::memory_order_relaxed);
        g_pending.store(true, std::memory_order_release);
    }
    // Done for the session: every report taken, or a site that traps every step.
    if (g_reports.load(std::memory_order_relaxed) >= (unsigned)g_cfg.max || traps >= kMaxTraps)
        g_armed.store(false, std::memory_order_relaxed);

    // Mask it again and run the same instruction: it now gives the default NaN, as stock.
    const DWORD m = ResumeMxcsr(ep->ContextRecord->MxCsr);
    ep->ContextRecord->MxCsr = m;
    ep->ContextRecord->FltSave.MxCsr = m;
    t_inHandler = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void Install(void (*log)(const char*), const Config& cfg) {
    g_log = log;
    g_cfg = cfg;
    if (g_cfg.max < 1) g_cfg.max = 1;
    if (g_cfg.max > kMaxReportsCap) g_cfg.max = kMaxReportsCap;
    if (HMODULE exe = GetModuleHandleA(nullptr)) {
        char path[MAX_PATH] = "";
        if (GetModuleFileNameA(exe, path, sizeof(path))) {
            const char* leaf = std::strrchr(path, '\\');
            std::snprintf(g_exeLeaf, sizeof(g_exeLeaf), "%s", leaf ? leaf + 1 : path);
        }
        auto dos = (const IMAGE_DOS_HEADER*)exe;
        auto nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)exe + dos->e_lfanew);
        g_exeLo = (uintptr_t)exe;
        g_exeHi = g_exeLo + nt->OptionalHeader.SizeOfImage;
    }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1 /*first*/, Handler);
    if (!g_veh) { Say("[nantrap] could not register the exception handler - trap stays off."); return; }
    g_armed.store(true, std::memory_order_release);
}

bool Armed() { return g_armed.load(std::memory_order_relaxed); }

uint32_t Enter() {
    const uint32_t saved = _mm_getcsr();
    ++t_depth;
    _mm_setcsr(ArmedMxcsr(saved));
    return saved;
}

void Restore(uint32_t saved) {
    _mm_setcsr(saved);
    --t_depth;
    g_steps.fetch_add(1, std::memory_order_relaxed);
}

void Leave(uint32_t saved) {
    Restore(saved);
    if (g_pending.load(std::memory_order_acquire)) Flush();
}

void Flush() {
    if (!g_pending.load(std::memory_order_acquire)) return;
    static std::atomic<bool> busy{false};
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return;
    const Record r = g_rec;   // a copy: the handler may fill the next one while we write

    WriteLog(r, SaySink, nullptr);

    const char* dir = crash::ReportDir();
    if (dir && dir[0]) {
        SYSTEMTIME local, utc;
        GetLocalTime(&local);
        GetSystemTime(&utc);
        char whenUtc[32], path[MAX_PATH];
        _snprintf_s(whenUtc, sizeof(whenUtc), _TRUNCATE, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.wYear,
                    utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
        // frostmod-crash-*.json is what MXB App collects and posts to /v1/diagnostics/crash.
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\frostmod-crash-%04d%02d%02d-%02d%02d%02d-nan%u.json",
                    dir, local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                    local.wSecond, r.index);
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "wb") == 0 && fp) {
            // nantrap=1's tyre ring (tyrelog), if it is on: the ~2 s that led up to this.
            char ringName[MAX_PATH] = "";
            crash::WriteRing("trap", ringName, sizeof(ringName));
            Meta m;
            m.ringFile = ringName;
            m.version = crash::Version();
            m.game = g_exeLeaf;
            m.whenUtc = whenUtc;
            WriteJson(r, m, crash::TheContext(), crash::TheTrail(), crash::ElapsedMs(),
                      [](void* user, const char* line) {
                          std::fputs(line, (FILE*)user);
                          std::fputc('\n', (FILE*)user);
                      },
                      fp);
            std::fclose(fp);
            char line[MAX_PATH + 96];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[nantrap] report written: %s (MXB App sends it)", path);
            Say(line);
        } else {
            Say("[nantrap] could not write the report file; the log lines above are the record.");
        }
    }
    if (!g_armed.load(std::memory_order_relaxed))
        Say("[nantrap] disarmed for the rest of this session; the step runs as stock from here.");
    g_pending.store(false, std::memory_order_release);
    busy.store(false);
}

unsigned Reports() { return g_reports.load(std::memory_order_relaxed); }
const Record& Last() { return g_rec; }

}  // namespace nantrap
