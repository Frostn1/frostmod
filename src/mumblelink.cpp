#include "mumblelink.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mumblelink {
namespace {

// Mumble's Link interface, verbatim from its documentation. The layout is a contract with
// another process — do not reorder, resize or "tidy" these fields.
//
// Mumble reads `uiVersion` to know it understands us, and watches `uiTick` to know we are
// still alive: a block whose tick stops advancing is treated as a game that has exited,
// and positional audio is dropped. So the tick must advance on every update, and only
// when the data is genuinely fresh.
struct LinkedMem {
    UINT32 uiVersion;
    DWORD  uiTick;
    float  fAvatarPosition[3];
    float  fAvatarFront[3];
    float  fAvatarTop[3];
    wchar_t name[256];
    float  fCameraPosition[3];
    float  fCameraFront[3];
    float  fCameraTop[3];
    wchar_t identity[256];
    UINT32 context_len;
    unsigned char context[256];
    wchar_t description[2048];
};

// The layout is shared with another process, so a silent size change here means we write
// Mumble's fields at the wrong offsets and it reads garbage — with no error anywhere.
// 5460 bytes on Windows (2-byte wchar_t); the assert is the only thing that would catch
// an accidental edit to the struct above.
static_assert(sizeof(LinkedMem) == 5460, "LinkedMem must match Mumble's layout exactly");
static_assert(sizeof(wchar_t) == 2, "Mumble's name/identity fields assume UTF-16");

constexpr UINT32 LINK_VERSION = 2;
constexpr float  DEG_TO_RAD = 0.017453292519943295f;

HANDLE      g_map = nullptr;
LinkedMem*  g_mem = nullptr;
std::mutex  g_mutex;
std::atomic<bool> g_enabled{true};

// Mumble compares raw bytes, so the context has to be byte-identical between two riders on
// the same server. Held here and written on every update rather than only when it changes:
// Mumble may be started after the game, and it reads whatever is in the block at the time.
char g_context[256] = {0};
UINT32 g_contextLen = 0;
wchar_t g_identity[256] = {0};
char g_server[100] = {0};
char g_track[100] = {0};

void WriteWide(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    wcsncpy_s(dst, cap, src ? src : L"", _TRUNCATE);
}

// Narrow -> wide for the name/identity fields. Rider names are free text and can carry
// anything, so a failed conversion yields an empty string rather than garbage.
void ToWide(wchar_t* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src || !*src) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n <= 0) dst[0] = 0;
}

} // namespace

bool Init() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_mem) return true;

    // Mumble's own name for the block. It creates this too — whoever gets there first
    // wins and the other attaches, which is why this works regardless of launch order.
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                               sizeof(LinkedMem), L"MumbleLink");
    if (!g_map) return false;

    g_mem = (LinkedMem*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkedMem));
    if (!g_mem) {
        CloseHandle(g_map);
        g_map = nullptr;
        return false;
    }

    // A freshly created mapping is already zeroed; an attached one may hold another game's
    // leftovers, so every field we own is set explicitly.
    memset(g_mem, 0, sizeof(LinkedMem));
    g_mem->uiVersion = LINK_VERSION;
    WriteWide(g_mem->name, 256, L"MX Bikes");
    WriteWide(g_mem->description, 2048, L"MX Bikes positional audio via FrostMod.");
    return true;
}

// Caller holds g_mutex. Server name plus track: the server alone would keep riders
// grouped across a track change, and the track alone would group strangers on unrelated
// servers running the same map — heard from positions on a track they aren't sharing.
static void RebuildContext() {
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "mxbikes|%s|%s", g_server, g_track);
    memset(g_context, 0, sizeof(g_context));
    // Length, not strlen at use: the context is compared as bytes, and Mumble is given the
    // count explicitly.
    size_t len = strnlen(buf, sizeof(g_context));
    memcpy(g_context, buf, len);
    g_contextLen = (UINT32)len;
}

void SetServer(const char* serverName) {
    std::lock_guard<std::mutex> lk(g_mutex);
    strncpy_s(g_server, serverName ? serverName : "", _TRUNCATE);
    RebuildContext();
}

void SetTrack(const char* trackName) {
    std::lock_guard<std::mutex> lk(g_mutex);
    strncpy_s(g_track, trackName ? trackName : "", _TRUNCATE);
    RebuildContext();
}

void SetIdentity(const char* riderName) {
    std::lock_guard<std::mutex> lk(g_mutex);
    ToWide(g_identity, 256, riderName);
}

bool HasIdentity() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_identity[0] != 0;
}

bool HasContext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_contextLen > 0;
}

void SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    if (!on) Clear();
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void Update(float x, float y, float z, float yawDeg) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_mem) return;

    // MX Bikes' world axes are already Mumble's: X right, Y up, Z forward, left-handed,
    // metres. Nothing is converted — see docs/MUMBLE.md for why that is not a coincidence
    // worth relying on silently.
    const float a = yawDeg * DEG_TO_RAD;
    const float fx = sinf(a), fz = cosf(a);

    g_mem->fAvatarPosition[0] = x;
    g_mem->fAvatarPosition[1] = y;
    g_mem->fAvatarPosition[2] = z;
    g_mem->fAvatarFront[0] = fx;
    g_mem->fAvatarFront[1] = 0.0f;
    g_mem->fAvatarFront[2] = fz;
    g_mem->fAvatarTop[0] = 0.0f;
    g_mem->fAvatarTop[1] = 1.0f;
    g_mem->fAvatarTop[2] = 0.0f;

    // The camera is what a listener hears from. We give it the rider's pose: MX Bikes'
    // chase cameras sit behind and above the bike, and hearing from there would put every
    // voice a few metres off from where the rider actually is.
    memcpy(g_mem->fCameraPosition, g_mem->fAvatarPosition, sizeof(float) * 3);
    memcpy(g_mem->fCameraFront, g_mem->fAvatarFront, sizeof(float) * 3);
    memcpy(g_mem->fCameraTop, g_mem->fAvatarTop, sizeof(float) * 3);

    memcpy(g_mem->context, g_context, sizeof(g_context));
    g_mem->context_len = g_contextLen;
    WriteWide(g_mem->identity, 256, g_identity);

    g_mem->uiVersion = LINK_VERSION;
    // Last, and only now: everything above is the frame this tick advertises as fresh.
    g_mem->uiTick++;
}

void ClearContext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_server[0] = 0; g_track[0] = 0;
    memset(g_context, 0, sizeof(g_context));
    g_contextLen = 0;
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_mem) return;
    // An all-zero position is the interface's own way of saying "no positional data".
    // Deliberately *not* stopping the tick — that would read as the game having exited.
    memset(g_mem->fAvatarPosition, 0, sizeof(float) * 3);
    memset(g_mem->fCameraPosition, 0, sizeof(float) * 3);
    g_mem->uiTick++;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_mem) {
        UnmapViewOfFile(g_mem);
        g_mem = nullptr;
    }
    if (g_map) {
        CloseHandle(g_map);
        g_map = nullptr;
    }
}

} // namespace mumblelink
