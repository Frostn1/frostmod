// Teardown at unload (src/unload.h) and the inventory it has to cover.
//
// The game FreeLibrary()s frostmod.dlo after Shutdown(). Anything still pointing into the dll
// then faults in an unmapped module - the 2026-10-08 quit crash, two access violations inside
// an unloaded frostmod.dlo after a normal exit. Unloading cannot be reproduced without the
// game, so this proves the two parts that can be checked anywhere:
//
//   1. the Teardown runs its steps once, stage by stage (threads, hooks, patches, handlers),
//      whatever order they were registered in;
//   2. the source still matches the inventory the teardown was written against. A new
//      MH_CreateHook outside the two known places, a new thread, or a new hand-written code
//      patch fails here until it is given a way out in Shutdown().

#include "../src/unload.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
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

using namespace unload;

// ---- a recording of what ran -----------------------------------------------------------------
static std::vector<std::string> g_ran;
static std::vector<bool>        g_blocked;
static void A(bool b) { g_ran.push_back("A"); g_blocked.push_back(b); }
static void B(bool b) { g_ran.push_back("B"); g_blocked.push_back(b); }
static void C(bool b) { g_ran.push_back("C"); g_blocked.push_back(b); }
static void D(bool b) { g_ran.push_back("D"); g_blocked.push_back(b); }
static void E(bool b) { g_ran.push_back("E"); g_blocked.push_back(b); }
static void Nop(bool) {}

static std::string Joined() {
    std::string s;
    for (auto& r : g_ran) s += r;
    return s;
}

static void TestOrder() {
    g_ran.clear(); g_blocked.clear();
    Teardown t;
    // Registered backwards on purpose: the stage decides, not the call order.
    CHECK(t.Add(Stage::Handlers, "handlers", &D), "add handlers");
    CHECK(t.Add(Stage::Patches,  "patches",  &C), "add patches");
    CHECK(t.Add(Stage::Hooks,    "hooks",    &B), "add hooks");
    CHECK(t.Add(Stage::Threads,  "threads",  &A), "add threads");
    CHECK(t.Add(Stage::Threads,  "threads2", &E), "add second thread step");
    CHECK(t.Size() == 5, "five steps, got %d", t.Size());
    CHECK(!t.Done(), "not done before Run");

    const int ran = t.Run(true);
    CHECK(ran == 5, "Run ran %d steps", ran);
    // Threads first (A then E, registration order inside a stage), hooks, patches, handlers last.
    CHECK(Joined() == "AEBCD", "order was %s, want AEBCD", Joined().c_str());
    CHECK(t.Done(), "done after Run");
    for (bool b : g_blocked) CHECK(b, "canBlock passed through as true");
}

static void TestOnce() {
    g_ran.clear(); g_blocked.clear();
    Teardown t;
    t.Add(Stage::Hooks, "hooks", &B);
    CHECK(t.Run(true) == 1, "first Run runs");
    // Shutdown() and then DllMain's detach both get here; the second must do nothing.
    CHECK(t.Run(false) == 0, "second Run is a no-op");
    CHECK(Joined() == "B", "ran once, got %s", Joined().c_str());
}

static void TestNoBlockUnderLoaderLock() {
    g_ran.clear(); g_blocked.clear();
    Teardown t;
    t.Add(Stage::Threads, "threads", &A);
    t.Run(false);
    CHECK(g_blocked.size() == 1 && !g_blocked[0], "canBlock=false reaches the step");
}

static std::vector<std::string> g_seen;
static void OnStep(Stage s, const char* name) { g_seen.push_back(std::string(StageName(s)) + ":" + name); }

static void TestOnStep() {
    g_seen.clear();
    Teardown t;
    t.Add(Stage::Patches, "tyres", &Nop);
    t.Add(Stage::Threads, "msg", &Nop);
    t.Run(true, &OnStep);
    CHECK(g_seen.size() == 2 && g_seen[0] == "threads:msg" && g_seen[1] == "patches:tyres",
          "onStep sees each step in run order");
}

static void TestRejects() {
    Teardown t;
    CHECK(!t.Add(Stage::Hooks, "null", nullptr), "null step rejected");
    CHECK(!t.Add(Stage::Count, "count", &Nop), "Stage::Count rejected");
    CHECK(!t.Add((Stage)-1, "neg", &Nop), "negative stage rejected");
    for (int i = 0; i < Teardown::kMaxSteps; ++i) CHECK(t.Add(Stage::Hooks, "x", &Nop), "add %d", i);
    CHECK(!t.Add(Stage::Hooks, "overflow", &Nop), "full table rejects");
    CHECK(t.Size() == Teardown::kMaxSteps, "size stays at capacity");
    CHECK(t.Run(true) == Teardown::kMaxSteps, "all kept steps run");
}

static void TestDetach() {
    int dummy = 0;
    CHECK(DetachIsUnload(nullptr), "null lpReserved is FreeLibrary: tear down");
    CHECK(!DetachIsUnload(&dummy), "non-null lpReserved is process exit: leave it");
    CHECK(std::strcmp(StageName(Stage::Threads), "threads") == 0, "stage name");
    CHECK(std::strcmp(StageName(Stage::Count), "?") == 0, "out-of-range stage name");
}

// ---- the inventory, read from the source -----------------------------------------------------
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int Count(const std::string& hay, const char* needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

// The body of the function whose definition starts with `head`, braces matched. "" if absent.
static std::string Body(const std::string& src, const char* head) {
    size_t p = src.find(head);
    if (p == std::string::npos) return "";
    p = src.find('{', p);
    if (p == std::string::npos) return "";
    int depth = 0;
    for (size_t i = p; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}' && --depth == 0) return src.substr(p, i - p + 1);
    }
    return "";
}

static bool Has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static void TestInventory() {
    const std::string dir = FROSTMOD_SRC;
    const std::string fm = ReadFile(dir + "/frostmod.cpp");
    const std::string cr = ReadFile(dir + "/crashreport.cpp");
    const std::string nt = ReadFile(dir + "/nantrap.cpp");
    CHECK(!fm.empty() && !cr.empty() && !nt.empty(), "sources readable under %s", dir.c_str());
    if (fm.empty()) return;

    // MinHook: exactly InstallHook() and the server-filter loop-top. Both refuse once unloading,
    // and MH_DisableHook(MH_ALL_HOOKS) at teardown covers every hook either made.
    CHECK(Count(fm, "MH_CreateHook(") == 2,
          "MH_CreateHook is called %d times; a new call site must refuse once g_unloading is set "
          "(or go through InstallHook) - then update this count", Count(fm, "MH_CreateHook("));
    CHECK(Has(Body(fm, "bool InstallHook(void* target, void* detour, void** original, const char* name) {"),
              "g_unloading"), "InstallHook refuses while unloading");
    CHECK(Has(Body(fm, "bool InstallServerFilterHook()"), "g_unloading"), "server filter refuses while unloading");
    CHECK(Has(Body(fm, "static void TdDisableHooks("), "MH_DisableHook(MH_ALL_HOOKS)"),
          "teardown disables every MinHook hook");
    CHECK(!Has(fm, "MH_Uninitialize("),
          "no MH_Uninitialize: a thread inside a detour still calls through its trampoline");

    // Hand-written code patches: the antifreeze displacement and the tyre-fallback span. Each
    // VirtualProtect is one of these two (apply, verify-revert, revert) - a new one is a new
    // patch that needs a revert in TdRevertPatches.
    CHECK(Count(fm, "VirtualProtect(") == 10,
          "VirtualProtect is called %d times (10 known: antifreeze 6, tyre fallback 4); a new code "
          "patch must be reverted in TdRevertPatches - then update this count", Count(fm, "VirtualProtect("));
    const std::string patches = Body(fm, "static void TdRevertPatches(");
    CHECK(Has(patches, "TyreFallbackRevert()"), "teardown reverts the tyre fallback span");
    CHECK(Has(patches, "AfRevert()"), "teardown reverts the antifreeze displacement");

    // Threads: each kept in a handle and stopped (and joined when it may block) first.
    CHECK(Count(fm, "CreateThread(") == 3,
          "CreateThread is called %d times (3 known: servermsg poll, memdiag report, Init); a new "
          "thread must be stopped in TdStopThreads - then update this count", Count(fm, "CreateThread("));
    CHECK(Has(fm, "g_msgThread = CreateThread("), "servermsg thread kept in a handle");
    CHECK(Has(fm, "g_mdThread = CreateThread("), "memdiag thread kept in a handle");
    CHECK(Has(fm, "g_initThread = CreateThread("), "init thread kept in a handle");
    const std::string threads = Body(fm, "static void TdStopThreads(");
    CHECK(Has(threads, "MsgFeedStop(") && Has(threads, "MdStop(") && Has(threads, "g_initThread"),
          "teardown stops all three threads");
    CHECK(Has(Body(fm, "static DWORD WINAPI MdThread("), "g_mdQuit"), "memdiag thread has a way out");

    // Exception handlers: removed, last.
    const std::string handlers = Body(fm, "static void TdRemoveHandlers(");
    CHECK(Has(handlers, "nantrap::Uninstall()"), "teardown removes the vectored handler");
    CHECK(Has(handlers, "crash::Uninstall()"), "teardown restores the unhandled-exception filter");
    CHECK(Has(Body(nt, "void Uninstall()"), "RemoveVectoredExceptionHandler"), "nantrap::Uninstall removes it");
    CHECK(Has(Body(cr, "void Uninstall()"), "SetUnhandledExceptionFilter(g_prev)"), "crash::Uninstall restores it");

    // Wired up: Shutdown() runs it; DllMain's FreeLibrary detach is the backstop; the module that
    // installs anything is pinned so calls already inside a detour return into mapped code.
    CHECK(Has(Body(fm, "__declspec(dllexport) void Shutdown()"), "RunTeardown(true"), "Shutdown runs the teardown");
    CHECK(Has(Body(fm, "BOOL APIENTRY DllMain("), "RunTeardown(false"), "DllMain detach runs it without blocking");
    CHECK(Has(Body(fm, "void EnsureInit()"), "PinSelf()"), "the hooking copy pins itself");
    CHECK(Has(Body(fm, "static void PinSelf()"), "GET_MODULE_HANDLE_EX_FLAG_PIN"), "PinSelf pins");
}

int main() {
    TestOrder();
    TestOnce();
    TestNoBlockUnderLoaderLock();
    TestOnStep();
    TestRejects();
    TestDetach();
    TestInventory();
    if (g_failures) {
        std::printf("unload_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("unload_test: all checks passed\n");
    return 0;
}
