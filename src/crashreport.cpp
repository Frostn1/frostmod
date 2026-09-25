// The live half of the crash report. See crashreport.h for what a report is and why.

#include "crashreport.h"
#include "nanwatch.h"

#include <windows.h>
#include <dbghelp.h>

namespace frostmod::crash {
namespace {

void (*g_log)(const char*) = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_ours = nullptr;
char g_dumpDir[MAX_PATH] = {0};
// One name per crash, shared by the dump and the report file beside it, so the pair is
// obviously a pair in a folder listing and the report can name its own dump.
char g_stem[64] = {0};
char g_version[32] = {0};
unsigned long long g_startTick = 0;
// One report per process. A fault inside the filter, or a second thread faulting
// while we write, would otherwise interleave two reports into one unreadable one.
std::atomic<bool> g_reporting{false};

// MiniDumpWriteDump, resolved at Install time. Loading dbghelp from inside a crash
// filter means running the loader lock on a process that is already broken.
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_writeDump = nullptr;

// `Emit` in the header is the sink TYPE a report writes through; these two are
// this file's own writer and the adapter that hands it to WriteContext.
void Say(const char* line) { if (g_log) g_log(line); }
void SaySink(void*, const char* line) { Say(line); }

unsigned long long NowMs() { return GetTickCount64() - g_startTick; }

const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "access violation";
        case EXCEPTION_IN_PAGE_ERROR:            return "in-page error - a mapped file could not be read";
        case EXCEPTION_STACK_OVERFLOW:           return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION:         return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "integer divide by zero";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "float divide by zero";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "array bounds exceeded";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
        case 0xC0000374:                         return "heap corruption";
        case 0xC0000409:                         return "stack buffer overrun";
        default:                                 return "unhandled exception";
    }
}

// "module+0xRVA" for an address, so a faulting site is something that can be looked
// up in a disassembler rather than a bare pointer that means nothing once ASLR has
// moved on.
void DescribeAddress(const void* addr, char* out, size_t n) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod && GetModuleFileNameA(mod, name, sizeof(name))) {
        const char* leaf = strrchr(name, '\\');
        _snprintf_s(out, n, _TRUNCATE, "%s+0x%llX", leaf ? leaf + 1 : name,
                    (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        _snprintf_s(out, n, _TRUNCATE, "0x%016llX (no module)", (unsigned long long)(uintptr_t)addr);
    }
}

// x64 unwind, straight off the PE's own tables: no symbols, no allocation, no disk.
// A frame with no function entry is a leaf (or hand-written assembly, which our own
// hook trampolines are) - there the return address is simply at [rsp].
// The frames, kept rather than only printed: the log wants them numbered and the sidecar
// wants them as an array, and unwinding twice from a context we have already walked is
// both slower and a second chance to fault.
char g_frames[kMaxFrames][160];
int  g_frameCount = 0;

// The walk itself, so the crash report and a live capture share one implementation. Fills
// `out` with "module+0xRVA" per frame, nearest first, and returns how many it wrote.
int Unwind(const CONTEXT& start, char (*out)[160], int max, int skip) {
    CONTEXT ctx = start;   // RtlVirtualUnwind mutates it; never touch the real one
    char where[MAX_PATH + 64];
    int written = 0;
    for (int frame = 0; frame < kMaxFrames && written < max; ++frame) {
        if (!ctx.Rip) break;
        if (frame >= skip) {
            DescribeAddress((const void*)ctx.Rip, where, sizeof(where));
            _snprintf_s(out[written++], sizeof(out[0]), _TRUNCATE, "%s", where);
        }

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {
            // Leaf frame. Stop rather than guess if [rsp] isn't readable.
            if (IsBadReadPtr((const void*)ctx.Rsp, sizeof(DWORD64))) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += sizeof(DWORD64);
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                         &handlerData, &establisher, nullptr);
    }
    return written;
}

void WriteStack(const CONTEXT& start) {
    g_frameCount = Unwind(start, g_frames, kMaxFrames, 0);
    char line[MAX_PATH + 128];
    for (int i = 0; i < g_frameCount; ++i) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[crash]   #%-2d %s", i, g_frames[i]);
        Say(line);
    }
}

void WriteRegisters(const CONTEXT& c) {
    char line[320];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] rip=%016llX rsp=%016llX rbp=%016llX flags=%08lX",
                (unsigned long long)c.Rip, (unsigned long long)c.Rsp,
                (unsigned long long)c.Rbp, (unsigned long)c.EFlags);
    Say(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX",
                (unsigned long long)c.Rax, (unsigned long long)c.Rbx,
                (unsigned long long)c.Rcx, (unsigned long long)c.Rdx);
    Say(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX",
                (unsigned long long)c.Rsi, (unsigned long long)c.Rdi,
                (unsigned long long)c.R8, (unsigned long long)c.R9);
    Say(line);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] r10=%016llX r11=%016llX r12=%016llX r13=%016llX r14=%016llX r15=%016llX",
                (unsigned long long)c.R10, (unsigned long long)c.R11,
                (unsigned long long)c.R12, (unsigned long long)c.R13,
                (unsigned long long)c.R14, (unsigned long long)c.R15);
    Say(line);

    // The float side. A NaN that became an index went through an XMM register on its way
    // to the integer one, and is usually still there (nanwatch.h). Low 64 bits of each, raw,
    // then which read as not a number - raw too, because plenty of XMM traffic is integer
    // data whose float reading means nothing, and only the code at the fault can say which.
    if ((c.ContextFlags & CONTEXT_FLOATING_POINT) != CONTEXT_FLOATING_POINT) {
        Say("[crash] no float registers in the context.");
        return;
    }
    uint64_t lows[16];
    for (int i = 0; i < 16; ++i) lows[i] = (uint64_t)c.FltSave.XmmRegisters[i].Low;
    for (int i = 0; i < 16; i += 4) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[crash] xmm%-2d %016llX  xmm%-2d %016llX  xmm%-2d %016llX  xmm%-2d %016llX",
                    i, (unsigned long long)lows[i], i + 1, (unsigned long long)lows[i + 1],
                    i + 2, (unsigned long long)lows[i + 2], i + 3, (unsigned long long)lows[i + 3]);
        Say(line);
    }
    // All 16 as "xmmN (double NaN)" is 294 bytes: room for every one of them.
    char which[320], summary[360];
    nanwatch::NonFiniteXmm(lows, which, sizeof(which));
    _snprintf_s(summary, sizeof(summary), _TRUNCATE, "[crash] not a number in: %s", which);
    Say(summary);
}

// Keep the newest kKeepDumps. A player who crashes nightly for a month should not
// discover FrostMod filled their disk, and the newest dumps are the ones anyone asks
// for. Deletes only files we name, never anything else in the folder.
void PruneOldest(const char* ext) {
    struct Found { char name[MAX_PATH]; FILETIME when; };
    Found found[64];
    int n = 0;
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\frostmod-crash-*.%s", g_dumpDir, ext);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n < (int)(sizeof(found) / sizeof(found[0]))) {
            strcpy_s(found[n].name, fd.cFileName);
            found[n].when = fd.ftLastWriteTime;
            ++n;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n <= kKeepDumps) return;

    // Oldest first, then delete everything past the newest kKeepDumps.
    for (int i = 1; i < n; ++i) {
        Found key = found[i];
        int j = i - 1;
        while (j >= 0 && CompareFileTime(&found[j].when, &key.when) > 0) {
            found[j + 1] = found[j];
            --j;
        }
        found[j + 1] = key;
    }
    for (int i = 0; i < n - kKeepDumps; ++i) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_dumpDir, found[i].name);
        DeleteFileA(path);
    }
}

struct DumpJob {
    EXCEPTION_POINTERS* ep;
    DWORD threadId;
    char path[MAX_PATH];
    BOOL ok;
};

// MiniDumpWriteDump is documented as best called from a thread other than the one
// that faulted, so the faulting thread's stack is captured as it was rather than as
// our filter left it. The filter waits on this.
DWORD WINAPI DumpThread(LPVOID param) {
    auto* job = (DumpJob*)param;
    HANDLE file = CreateFileA(job->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { job->ok = FALSE; return 0; }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = job->threadId;
    mei.ExceptionPointers = job->ep;
    mei.ClientPointers = FALSE;
    // Small enough to send, complete enough to read: every thread's stack and registers,
    // the module list, and the memory the stacks scan as pointing at something. Not the
    // heap, and deliberately not MiniDumpWithIndirectlyReferencedMemory - it is the flag
    // that turns a few megabytes into tens of them, and MXB App's log bundle skips any
    // single file over 64 MB. A dump nobody can upload answers nothing.
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpScanMemory |
                                               MiniDumpWithThreadInfo |
                                               MiniDumpWithUnloadedModules);
    job->ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                          &mei, nullptr, nullptr);
    CloseHandle(file);
    return 0;
}

bool WriteDump(EXCEPTION_POINTERS* ep) {
    if (!g_writeDump || !g_dumpDir[0]) {
        Say("[crash] no minidump: dbghelp.dll was unavailable when FrostMod started.");
        return false;
    }
    DumpJob job{};
    job.ep = ep;
    job.threadId = GetCurrentThreadId();
    _snprintf_s(job.path, sizeof(job.path), _TRUNCATE, "%s\\%s.dmp", g_dumpDir, g_stem);

    HANDLE t = CreateThread(nullptr, 0, DumpThread, &job, 0, nullptr);
    if (!t) { Say("[crash] no minidump: could not start the writer thread."); return false; }
    // Bounded: a dump that hasn't landed in 30s is not worth holding the process
    // open for, and the report above it is already written.
    const DWORD waited = WaitForSingleObject(t, 30000);
    CloseHandle(t);

    char line[MAX_PATH + 160];
    if (waited != WAIT_OBJECT_0) {
        Say("[crash] the minidump writer did not finish in 30s; carrying on without it.");
        return false;
    }
    if (!job.ok) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[crash] minidump FAILED (%lu) - the report above is all we have.",
                    GetLastError());
        Say(line);
        return false;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] minidump written: %s  (send it with the log - Settings > Send logs "
                "picks it up)", job.path);
    Say(line);
    PruneOldest("dmp");
    return true;
}

// The sidecar: the same report as JSON, beside the log, for MXB App to pick up and post.
//
// A file rather than a socket, and written by the dying process rather than sent by it: at
// this point the game has seconds at best, the network stack is the last thing to trust,
// and a crash that happened while the app was closed still has to arrive. The app finds it
// on its next look, sends it, and renames it. See tasks/game-crashes.md.
void WriteSidecar(const Fault& f) {
    if (!g_dumpDir[0]) return;   // the session plugin: its folder is nobody's to collect
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.json", g_dumpDir, g_stem);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) {
        Say("[crash] could not write the report file next to the log; the text above is all "
            "there is.");
        return;
    }
    const char* frames[kMaxFrames];
    for (int i = 0; i < g_frameCount; ++i) frames[i] = g_frames[i];
    WriteJson(f, TheContext(), TheTrail(), frames, g_frameCount, NowMs(),
              [](void* user, const char* line) {
                  std::fputs(line, (FILE*)user);
                  std::fputc('\n', (FILE*)user);
              },
              fp);
    std::fclose(fp);

    PruneOldest("json");

    char line[MAX_PATH + 96];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[crash] report written: %s (MXB App sends this one; the dump stays here "
                "until you say so)", path);
    Say(line);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    // One report, and never re-enter: a fault inside this filter must not recurse.
    bool expected = false;
    if (!g_reporting.compare_exchange_strong(expected, true))
        return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;

    // The same fault again: the filter we chained to resumed it rather than ending the
    // process. It was reported the first time; say so once, and past the limit stop handing
    // it back, so the game ends instead of hanging on one instruction. Only touched under
    // g_reporting, so one fault at a time sees the gate.
    if (ep && ep->ExceptionRecord) {
        static RepeatGate gate;
        const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        FaultKey key;
        key.code    = rec->ExceptionCode;
        key.address = (uintptr_t)rec->ExceptionAddress;
        key.target  = rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2
                          ? (uintptr_t)rec->ExceptionInformation[1]
                          : 0;
        key.thread  = GetCurrentThreadId();
        const unsigned seen = gate.See(key, GetTickCount64());
        if (seen > 1) {
            if (seen == 2)
                Say("[crash] the same fault again on the same thread: the handler before "
                    "FrostMod resumed it. Not reported again.");
            const bool give_up = seen >= kResumedFaultLimit;
            if (give_up)
                Say("[crash] still faulting on the same instruction; no longer handing it "
                    "back. The game is going down.");
            g_reporting.store(false);
            if (give_up) return EXCEPTION_EXECUTE_HANDLER;
            return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
        }
    }

    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* r = ep->ExceptionRecord;
        char where[MAX_PATH + 64], line[MAX_PATH + 160];
        DescribeAddress(r->ExceptionAddress, where, sizeof(where));

        // Name this crash once. Local time in the file name, because the player reading
        // their own folder is the one who has to match it against when the game died; UTC
        // inside the report, because everyone else's reports have to sort against it.
        SYSTEMTIME local, utc;
        GetLocalTime(&local);
        GetSystemTime(&utc);
        _snprintf_s(g_stem, sizeof(g_stem), _TRUNCATE,
                    "frostmod-crash-%04d%02d%02d-%02d%02d%02d",
                    local.wYear, local.wMonth, local.wDay,
                    local.wHour, local.wMinute, local.wSecond);
        char whenUtc[32];
        _snprintf_s(whenUtc, sizeof(whenUtc), _TRUNCATE, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                    utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);

        Fault fault;
        fault.kind = ExceptionName(r->ExceptionCode);
        fault.code = (unsigned)r->ExceptionCode;
        fault.site = where;
        fault.version = g_version;
        fault.whenUtc = whenUtc;
        fault.uptimeMs = NowMs();
        char gameName[MAX_PATH] = "";
        if (GetModuleFileNameA(nullptr, gameName, sizeof(gameName))) {
            const char* leaf = strrchr(gameName, '\\');
            fault.game = leaf ? leaf + 1 : gameName;
        }
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[crash] ***** %s (0x%08X) at %s *****",
                    ExceptionName(r->ExceptionCode), (unsigned)r->ExceptionCode, where);
        Say(line);

        // For these two the record carries what was being touched: parameter 0 is the
        // kind of access, parameter 1 the address it was refused at.
        if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && r->NumberParameters >= 2) {
            const ULONG_PTR op = r->ExceptionInformation[0];
            fault.access = op == 0 ? "reading" : op == 1 ? "writing" : "executing";
            fault.target = (unsigned long long)r->ExceptionInformation[1];
            fault.haveTarget = true;
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[crash] while %s address 0x%016llX",
                        fault.access, (unsigned long long)r->ExceptionInformation[1]);
            Say(line);
            // An in-page error carries the filesystem's own status as a third parameter,
            // and this is the shape a cloud-backed mods folder crashes in: the bytes were
            // never on the disk and the fetch failed underneath a mapped read.
            if (r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && r->NumberParameters >= 3) {
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[crash] the filesystem returned NTSTATUS 0x%08X for that page. If "
                            "the mods folder is on OneDrive/Dropbox, the file is a placeholder "
                            "rather than real bytes - right-click the folder and pick \"Always "
                            "keep on this device\".",
                            (unsigned)r->ExceptionInformation[2]);
                Say(line);
            }
        }

        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[crash] FrostMod v%s, thread %lu, %llums into the run.",
                    g_version, GetCurrentThreadId(), NowMs());
        Say(line);

        if (ep->ContextRecord) {
            WriteRegisters(*ep->ContextRecord);
            Say("[crash] call stack (nearest first):");
            WriteStack(*ep->ContextRecord);
        } else {
            Say("[crash] no register context was handed to the filter; no stack.");
        }

        WriteContext(TheContext(), TheTrail(), NowMs(), SaySink, nullptr);
        char dumpName[80] = "";
        if (WriteDump(ep)) _snprintf_s(dumpName, sizeof(dumpName), _TRUNCATE, "%s.dmp", g_stem);
        fault.dumpFile = dumpName;
        WriteSidecar(fault);
        Say("[crash] end of report - the game process is going down.");
    }
    // Chain rather than swallow: whatever reporting the game or Steam had set up
    // still runs.
    g_reporting.store(false);
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

unsigned long long ElapsedMs() { return NowMs(); }

int CaptureStack(char (*out)[160], int max) {
    if (!out || max <= 0) return 0;
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    // Skip this function and the caller's own frame: what is being asked for is who called
    // the thing that is asking, and two frames of us at the top is noise in every line.
    return Unwind(ctx, out, max, 2);
}

Trail& TheTrail() { static Trail t; return t; }
Context& TheContext() { static Context c; return c; }

void Note(const char* fmt, ...) {
    char buf[kCrumbLen];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    TheTrail().Add(NowMs(), buf);
}

void Install(void (*log)(const char*), const char* dumpDir, const char* version) {
    g_log = log;
    g_startTick = GetTickCount64();
    _snprintf_s(g_version, sizeof(g_version), _TRUNCATE, "%s", version ? version : "?");
    if (dumpDir) _snprintf_s(g_dumpDir, sizeof(g_dumpDir), _TRUNCATE, "%s", dumpDir);

    // Pre-resolve: the loader lock is not somewhere to go during a crash.
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll"))
        g_writeDump = (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump");

    g_ours = Filter;
    g_prev = SetUnhandledExceptionFilter(Filter);
}

void Rearm() {
    if (!g_ours) return;
    LPTOP_LEVEL_EXCEPTION_FILTER now = SetUnhandledExceptionFilter(g_ours);
    if (now == g_ours) return;              // still ours: nothing happened
    g_prev = now;                           // someone installed over us; chain to them
    Say("[crash] another filter had taken the top of the chain; FrostMod took it back "
         "and will chain to theirs.");
}

}  // namespace frostmod::crash
