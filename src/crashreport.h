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
// The same report, as JSON, for the machine that reads it.
//
// The text report above is for whoever opens the log. This is the one MXB App picks up
// and posts to the diagnostics endpoint, so crash sites can be counted across everyone
// rather than read one bundle at a time. Same facts, no prose.
//
// Written by hand rather than with a library because it is nine fields and the alternative
// is a dependency in a DLL that has to survive being loaded into somebody else's process.
// Everything that goes in is escaped through Escape(), including the strings that came off
// a server: a rider name is attacker-controlled text and a report that broke the parser
// would be a report nobody reads.
//
// Pure, like the layout above, and tested the same way.
// ---------------------------------------------------------------------------

/// JSON string body (no quotes) for `in`, truncated to fit `n`. Control characters go out
/// as \uXXXX, which keeps a name with a stray newline in it from ending the line.
inline void Escape(const char* in, char* out, size_t n) {
    if (!n) return;
    size_t w = 0;
    for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p; ++p) {
        char buf[8];
        int len;
        switch (*p) {
            case '"':  std::memcpy(buf, "\\\"", 2); len = 2; break;
            case '\\': std::memcpy(buf, "\\\\", 2); len = 2; break;
            case '\n': std::memcpy(buf, "\\n", 2);  len = 2; break;
            case '\r': std::memcpy(buf, "\\r", 2);  len = 2; break;
            case '\t': std::memcpy(buf, "\\t", 2);  len = 2; break;
            default:
                if (*p < 0x20) { len = std::snprintf(buf, sizeof(buf), "\\u%04x", *p); }
                else           { buf[0] = (char)*p; len = 1; }
                break;
        }
        if (w + (size_t)len >= n) break;   // truncate on a whole escape, never half of one
        std::memcpy(out + w, buf, (size_t)len);
        w += (size_t)len;
    }
    out[w] = 0;
}

/// What the fault itself was. The session half comes from Context, the history from Trail.
struct Fault {
    const char* kind = "";        // "access violation", and the rest of ExceptionName
    unsigned code = 0;            // 0xC0000005
    const char* site = "";        // "mxbikes.exe+0x11D753" - the key everything groups by
    const char* access = "";      // "reading" | "writing" | "executing" | "" when not an AV
    unsigned long long target = 0;
    bool haveTarget = false;
    const char* version = "";     // FrostMod's
    const char* game = "";        // the exe we are inside
    const char* whenUtc = "";     // ISO 8601, so two machines' reports sort together
    const char* dumpFile = "";    // the .dmp beside this, or empty
    unsigned long long uptimeMs = 0;
};

/// One JSON object, emitted as lines. `frames` is nearest-first.
inline void WriteJson(const Fault& f, const Context& ctx, const Trail& trail,
                      const char* const* frames, int frameCount,
                      unsigned long long nowMs, Emit emit, void* user) {
    char line[512], esc[256];
    emit(user, "{");
    std::snprintf(line, sizeof(line), "  \"schema\": 1,");
    emit(user, line);
    Escape(f.version, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"frostmod\": \"%s\",", esc);
    emit(user, line);
    Escape(f.game, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"game\": \"%s\",", esc);
    emit(user, line);
    Escape(f.whenUtc, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"when\": \"%s\",", esc);
    emit(user, line);
    std::snprintf(line, sizeof(line), "  \"uptimeMs\": %llu,", f.uptimeMs);
    emit(user, line);
    Escape(f.dumpFile, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "  \"dump\": \"%s\",", esc);
    emit(user, line);

    emit(user, "  \"fault\": {");
    Escape(f.kind, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"kind\": \"%s\",", esc);
    emit(user, line);
    std::snprintf(line, sizeof(line), "    \"code\": \"0x%08X\",", f.code);
    emit(user, line);
    Escape(f.site, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"site\": \"%s\",", esc);
    emit(user, line);
    Escape(f.access, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"access\": \"%s\",", esc);
    emit(user, line);
    if (f.haveTarget)
        std::snprintf(line, sizeof(line), "    \"target\": \"0x%016llX\"", f.target);
    else
        std::snprintf(line, sizeof(line), "    \"target\": null");
    emit(user, line);
    emit(user, "  },");

    emit(user, "  \"frames\": [");
    for (int i = 0; i < frameCount; ++i) {
        Escape(frames[i], esc, sizeof(esc));
        std::snprintf(line, sizeof(line), "    \"%s\"%s", esc, i + 1 < frameCount ? "," : "");
        emit(user, line);
    }
    emit(user, "  ],");

    const Where w = (Where)ctx.where.load(std::memory_order_relaxed);
    const int riders = ctx.riders.load(std::memory_order_relaxed);
    const unsigned long long lastDraw = ctx.lastDrawMs.load(std::memory_order_relaxed);
    const unsigned long long lastReload = ctx.lastReloadMs.load(std::memory_order_relaxed);
    emit(user, "  \"session\": {");
    Escape(WhereName(w), esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"where\": \"%s\",", esc);
    emit(user, line);
    std::snprintf(line, sizeof(line), "    \"inSession\": %s,",
                  ctx.inSession.load(std::memory_order_relaxed) ? "true" : "false");
    emit(user, line);
    Escape(ctx.track, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"track\": \"%s\",", esc);
    emit(user, line);
    Escape(ctx.server, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"server\": \"%s\",", esc);
    emit(user, line);
    Escape(ctx.rider, esc, sizeof(esc));
    std::snprintf(line, sizeof(line), "    \"rider\": \"%s\",", esc);
    emit(user, line);
    if (riders >= 0) std::snprintf(line, sizeof(line), "    \"riders\": %d,", riders);
    else             std::snprintf(line, sizeof(line), "    \"riders\": null,");
    emit(user, line);
    std::snprintf(line, sizeof(line), "    \"reloads\": %u,",
                  ctx.reloads.load(std::memory_order_relaxed));
    emit(user, line);
    // Null rather than zero when it never happened: "no frame has ever been drawn" and
    // "a frame was drawn this instant" are opposite answers and must not share a value.
    if (lastDraw) std::snprintf(line, sizeof(line), "    \"sinceFrameMs\": %llu,",
                                nowMs >= lastDraw ? nowMs - lastDraw : 0);
    else          std::snprintf(line, sizeof(line), "    \"sinceFrameMs\": null,");
    emit(user, line);
    if (lastReload) std::snprintf(line, sizeof(line), "    \"sinceReloadMs\": %llu",
                                  nowMs >= lastReload ? nowMs - lastReload : 0);
    else            std::snprintf(line, sizeof(line), "    \"sinceReloadMs\": null");
    emit(user, line);
    emit(user, "  },");

    emit(user, "  \"trail\": [");
    const int n = trail.Count();
    for (int i = 0; i < n; ++i) {
        const Crumb& c = trail.At(i);
        Escape(c.text, esc, sizeof(esc));
        std::snprintf(line, sizeof(line), "    { \"beforeMs\": %llu, \"text\": \"%s\" }%s",
                      nowMs >= c.ms ? nowMs - c.ms : 0, esc, i + 1 < n ? "," : "");
        emit(user, line);
    }
    emit(user, "  ]");
    emit(user, "}");
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

/// The call stack as it is right now, nearest caller first, each frame as "module+0xRVA".
///
/// The same unwinder the crash report uses, exposed because a fault is not the only moment
/// worth a stack: a guard that turns a crash away has, at that instant, the one thing no
/// crash dump can give - the stack of the call that was ABOUT to go wrong, with everything
/// above it intact. That is how the thing upstream gets a name.
///
/// Costs a few dozen table lookups and no allocation, but it is not free: call it on the
/// first few occurrences of something, never on every one.
int CaptureStack(char (*out)[160], int max);

/// Milliseconds since Install(). Every timestamp in a report is on this clock, so a
/// caller stamping Context::lastDrawMs / lastReloadMs must use this and not
/// GetTickCount64 - the report subtracts them.
unsigned long long ElapsedMs();

#else   // non-Windows hosts: the tests build the portable half only

inline Trail& TheTrail() { static Trail t; return t; }
inline Context& TheContext() { static Context c; return c; }

#endif  // _WIN32

}  // namespace frostmod::crash
