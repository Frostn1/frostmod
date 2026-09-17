#pragma once
// ============================================================================
//  Crash reports - what the game was doing when it died.
//
//  MX Bikes goes to desktop on its own: landing an overjump, hitting an object,
//  clicking "go to track", and more often on a busy server. Those crashes predate
//  both MXB App and FrostMod, so nothing on our side caused them - but FrostMod is
//  already inside the process when they happen, which makes it the only thing in a
//  position to say where.
//
//  Until now it said almost nothing. The filter in frostmod.cpp logged one line -
//  "access violation at mxbikes.exe+0x11D753" - and that is genuinely useful (it is
//  how the GHS close guard in offsets.h was found), but one address off the top of
//  the stack cannot answer the two questions that decide whether a crash is fixable:
//  who called it, and what was the game doing at the time. A player's log would end
//  with that line after four hours of server-browser chatter, with no way to tell
//  whether they were on track, in the pits or in a menu.
//
//  So a report is now four things, in this order, all written before we chain on:
//
//    1. the fault itself      - kind, module+RVA, the address it was refused at
//    2. the registers         - RIP/RSP and the integer file, one line each
//    3. the call stack        - up to kMaxFrames frames, each as module+0xRVA
//    4. the trail             - the last kMaxCrumbs things that happened, and the
//                               session context: on track / in a menu, rider count,
//                               track, server, time since the last content reload
//
//  plus a minidump beside the log, which MXB App's "Send logs" already sweeps up
//  (logs.rs takes everything in the FrostMod folder that isn't one of our binaries).
//
//  Why unwind by hand rather than call dbghelp's StackWalk64: StackWalk64 wants
//  SymInitialize, which loads symbol modules at the worst possible moment. x64 has
//  unwind tables in the PE itself, so RtlLookupFunctionEntry + RtlVirtualUnwind walk
//  the frames with no allocation, no symbols and no disk. dbghelp is still loaded
//  (at Install time, never at crash time) for MiniDumpWriteDump, and a report is
//  written with or without it.
//
//  Two things it cannot promise, so neither is claimed in the log: a filter installed
//  after ours replaces it - Rearm() takes the top back, but only as often as it is
//  called - and a stack overflow may leave too little stack to run this at all.
//
//  The trail, the context and the report layout are host-independent and covered by
//  tests/crashreport_test.cpp. Everything that needs Win32 is behind _WIN32 below.
// ============================================================================

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace frostmod::crash {

// ---------------------------------------------------------------------------
// The trail: a fixed ring of short notes, written by the game threads as things
// happen and read by the faulting thread when one of them dies.
//
// Deliberately lock-free. A crash filter that takes a mutex can deadlock against a
// thread the fault left holding it, and the cost of that is the whole report; the
// cost of racing is at worst one garbled line in the middle of it. Writers claim a
// slot with one fetch_add and fill it; readers take what is there.
// ---------------------------------------------------------------------------

inline constexpr int kMaxCrumbs = 32;   // ~10 minutes of a normal session
inline constexpr int kCrumbLen  = 112;  // one line each, truncated not wrapped

struct Crumb {
    unsigned long long ms = 0;    // milliseconds since the process started
    char text[kCrumbLen] = {0};
};

class Trail {
  public:
    void Add(unsigned long long ms, const char* text) {
        const unsigned slot = m_written.fetch_add(1, std::memory_order_relaxed);
        Crumb& c = m_items[slot % kMaxCrumbs];
        c.ms = ms;
        std::snprintf(c.text, sizeof(c.text), "%s", text ? text : "");
    }

    /// How many notes are readable: everything, once the ring has wrapped.
    int Count() const {
        const unsigned n = m_written.load(std::memory_order_relaxed);
        return (int)(n < (unsigned)kMaxCrumbs ? n : (unsigned)kMaxCrumbs);
    }

    /// Oldest first, so a report reads in the order things happened. `i` is 0-based
    /// over Count(); out of range gives an empty crumb rather than a fault, because
    /// the one caller that matters is running inside a crash.
    const Crumb& At(int i) const {
        static const Crumb kEmpty{};
        const int n = Count();
        if (i < 0 || i >= n) return kEmpty;
        const unsigned written = m_written.load(std::memory_order_relaxed);
        const unsigned oldest  = (written > (unsigned)kMaxCrumbs) ? written - kMaxCrumbs : 0;
        return m_items[(oldest + (unsigned)i) % kMaxCrumbs];
    }

    /// Total ever added, including the ones the ring has dropped. The report says so:
    /// "last 32 of 210" is different information from "32 things happened".
    unsigned TotalAdded() const { return m_written.load(std::memory_order_relaxed); }

  private:
    Crumb m_items[kMaxCrumbs]{};
    std::atomic<unsigned> m_written{0};
};

// ---------------------------------------------------------------------------
// Session context: the answers to "where were they" that no address can give.
//
// Each field is written by whichever callback learns it and read once, in the
// filter. Same reasoning as the trail: plain stores, no lock.
// ---------------------------------------------------------------------------

enum class Where : int {
    Unknown  = -1,   // no Draw() yet: injected-only, or still in the menus
    OnTrack  = 0,    // Draw(_iState) as the plugin API defines it
    Spectate = 1,
    Replay   = 2,
};

inline const char* WhereName(Where w) {
    switch (w) {
        case Where::OnTrack:  return "on track";
        case Where::Spectate: return "spectating";
        case Where::Replay:   return "in a replay";
        default:              return "in the menus (or no Draw yet)";
    }
}

struct Context {
    std::atomic<int> where{(int)Where::Unknown};
    // Whether the game is in an event at all. Separate from `where` because the two copies
    // of FrostMod know different halves: only the plugin copy is called back with Draw's
    // state, while both can see that a track and a server are set. Without this the injected
    // copy's report would call a rider mid-lap "in the menus".
    std::atomic<bool> inSession{false};
    std::atomic<int> riders{-1};              // rider count incl. us; -1 = not in a race
    std::atomic<unsigned> reloads{0};         // content reloads run this process
    std::atomic<unsigned long long> lastDrawMs{0};
    std::atomic<unsigned long long> lastReloadMs{0};
    // Set once per event; short enough that a torn read is still legible.
    char track[96]  = {0};
    char server[96] = {0};
    char rider[96]  = {0};

    void SetTrack(const char* s)  { std::snprintf(track,  sizeof(track),  "%s", s ? s : ""); }
    void SetServer(const char* s) { std::snprintf(server, sizeof(server), "%s", s ? s : ""); }
    void SetRider(const char* s)  { std::snprintf(rider,  sizeof(rider),  "%s", s ? s : ""); }
};

// ---------------------------------------------------------------------------
// Report layout. Pure: it emits lines through a sink, so the test can assert on
// what a report says without a crash, a game or Win32.
// ---------------------------------------------------------------------------

/// Where the report goes. `user` is passed straight back.
using Emit = void (*)(void* user, const char* line);

/// The context + trail half of a report - everything that is true regardless of
/// which fault fired. The fault, registers and stack are prepended by the filter,
/// which is the only part that needs a CONTEXT record.
inline void WriteContext(const Context& ctx, const Trail& trail,
                         unsigned long long nowMs, Emit emit, void* user) {
    char line[320];
    const Where w = (Where)ctx.where.load(std::memory_order_relaxed);
    const int riders = ctx.riders.load(std::memory_order_relaxed);
    const unsigned long long lastDraw = ctx.lastDrawMs.load(std::memory_order_relaxed);

    if (w != Where::Unknown)
        std::snprintf(line, sizeof(line), "[crash] the game was %s", WhereName(w));
    else if (ctx.inSession.load(std::memory_order_relaxed))
        // In an event, but this copy of FrostMod is not the one the game draws through, so
        // it cannot say whether they were riding, spectating or sat in the pits.
        std::snprintf(line, sizeof(line),
                      "[crash] the game was in a session (this copy sees no frames)");
    else
        std::snprintf(line, sizeof(line), "[crash] the game was %s", WhereName(w));
    emit(user, line);

    if (lastDraw && nowMs >= lastDraw) {
        std::snprintf(line, sizeof(line), "[crash] last frame drawn %llums ago",
                      (unsigned long long)(nowMs - lastDraw));
        emit(user, line);
    }

    char count[16];
    if (riders >= 0) std::snprintf(count, sizeof(count), "%d", riders);
    else             std::snprintf(count, sizeof(count), "n/a");
    std::snprintf(line, sizeof(line), "[crash] track='%s' server='%s' rider='%s' riders=%s",
                  ctx.track[0] ? ctx.track : "<none>",
                  ctx.server[0] ? ctx.server : "<none>",
                  ctx.rider[0] ? ctx.rider : "<none>", count);
    emit(user, line);

    const unsigned reloads = ctx.reloads.load(std::memory_order_relaxed);
    const unsigned long long lastReload = ctx.lastReloadMs.load(std::memory_order_relaxed);
    if (reloads == 0) {
        emit(user, "[crash] no content reload has run this session");
    } else {
        std::snprintf(line, sizeof(line),
                      "[crash] %u content reload(s) this session, the last %llums ago",
                      reloads, (unsigned long long)(nowMs >= lastReload ? nowMs - lastReload : 0));
        emit(user, line);
    }

    const int n = trail.Count();
    const unsigned total = trail.TotalAdded();
    if (n == 0) {
        emit(user, "[crash] nothing on the trail - this crash came before anything we log");
        return;
    }
    if ((unsigned)n < total)
        std::snprintf(line, sizeof(line), "[crash] trail - the last %d of %u, oldest first:",
                      n, total);
    else
        std::snprintf(line, sizeof(line), "[crash] trail - all %d, oldest first:", n);
    emit(user, line);
    for (int i = 0; i < n; ++i) {
        const Crumb& c = trail.At(i);
        std::snprintf(line, sizeof(line), "[crash]   -%llums  %s",
                      (unsigned long long)(nowMs >= c.ms ? nowMs - c.ms : 0), c.text);
        emit(user, line);
    }
}

// ---------------------------------------------------------------------------
// The live half.
// ---------------------------------------------------------------------------
#ifdef _WIN32

inline constexpr int kMaxFrames = 40;   // deep enough for the game's bus dispatch
inline constexpr int kKeepDumps = 3;    // newest N kept beside the log; older pruned

/// Installs the unhandled-exception filter and, if dbghelp is there, arms the
/// minidump. `log` takes one preformatted line; `dumpDir` is the folder the dump
/// is written into (the log's folder) - pass nullptr for no dump.
/// `version` is stamped into the report so a pasted log names the build.
void Install(void (*log)(const char*), const char* dumpDir, const char* version);

/// Takes the top of the filter chain back if something installed over us. Cheap
/// (one SetUnhandledExceptionFilter); call it from the periodic tick.
void Rearm();

/// The shared trail + context. Note() timestamps for you.
Trail&   TheTrail();
Context& TheContext();
void Note(const char* fmt, ...);

/// Milliseconds since Install(). Every timestamp in a report is on this clock, so a
/// caller stamping Context::lastDrawMs / lastReloadMs must use this and not
/// GetTickCount64 - the report subtracts them.
unsigned long long ElapsedMs();

#else   // non-Windows hosts: the tests build the portable half only

inline Trail& TheTrail() { static Trail t; return t; }
inline Context& TheContext() { static Context c; return c; }

#endif  // _WIN32

}  // namespace frostmod::crash
