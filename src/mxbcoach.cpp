// mxbcoach.cpp - MXB Coach's recorder, a PiBoSo plugin (mxbcoach.dlo) for MX Bikes.
//
// The game loads it from its plugins\ folder and hands it the rider's own telemetry. It
// writes one .mxbc file per stint on track into <save path>\mxbcoach\sessions\, which the
// MXB Coach app reads afterwards. In practice it also shows the live cues the app writes to
// <save path>\mxbcoach\cues\, one short line at a time through the game's Draw callback. It
// hooks nothing: everything arrives through the published callbacks, and the rules live in
// coachrec.h and coachcue.h.
//
// It also records sitting and standing, by polling the rider's Sit bind (stance.h): a key
// through GetAsyncKeyState, a controller button through DirectInput from the device the bind
// names (XInput only as a guess, when DirectInput won't open it).
//
// And the other riders, from the Race* callbacks (others.h): who is in the event, where every
// bike is at 10 Hz, and everyone's lap and split times. Only while a stint is recording.
//
// It can also say each cue out loud as it shows, when <save path>\mxbcoach\cues\voice.ini
// turns that on (coachvoice.h). The clips are embedded as resources and played through
// winmm's waveOut, which mixes with other plugins' sounds and runs under Wine and Proton.
//
// In practice it draws MXB Coach's HUD round the cue (coachhud.h): the section and its tip, the
// gap to Coach's lap, sit or stand, a track map with Coach's ghost, and the setup card while
// stopped. The app's <save path>\mxbcoach\cues\<track>.<bike>.hud and hud.ini say what shows.
//
// MX Bikes only for now. GP Bikes and Kart Racing Pro send different telemetry structs.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "coachcue.h"
#include "coachhud.h"
#include "coachlog.h"
#include "coachrec.h"
#include "coachvoice.h"
#include "offsets.h"
#include "others.h"
#include "stance.h"
#include "version.h"

namespace {

std::mutex         g_mu;
coachrec::Recorder g_rec;
coachcue::Player   g_cues;
coachcue::Event    g_event;
others::Tracker    g_others;
std::string        g_user;  // <save path>, the game's user folder
std::string        g_base;  // <save path>\mxbcoach\

// SPluginsBikeData_t: speedometer m/s, world x and z, and the crashed flag.
constexpr size_t kDataSpeed   = 20;
constexpr size_t kDataPosX    = 24;
constexpr size_t kDataPosZ    = 32;
constexpr size_t kDataCrashed = 136;

// The HUD (coachhud.h).
std::string          g_plugins;  // the folder this .dlo was loaded from
coachhud::Sheet      g_hud_sheet;
coachhud::RefLap     g_ref;
coachhud::LapClock   g_clock;
coachhud::StopWatch  g_stop;
coachhud::Track      g_track;
coachhud::Settings   g_hud_set;
coachhud::Frame      g_frame;
std::string          g_setup;
bool                 g_practice = false;
bool                 g_have_sample = false;
float                g_time = 0, g_pos = 0;
coachhud::Pt         g_rider;
stance::Confidence   g_stance_conf = stance::CONF_NONE;
// hud.ini as last read: whether it was there and when it was written.
bool                 g_ini_seen = false;
FILETIME             g_ini_time = {};
ULONGLONG            g_ini_checked = 0;
// Whether this stint has already written its one "the game called Draw" line.
bool                 g_logged_draw = false;

// PiBoSo draw items, as in mxb_example.c. The game reads them after Draw() returns, so they
// live in these statics.
//
// m_iFont is "1 based index in FontName buffer" (mxb_api.h): the buffer THIS plugin hands back
// from DrawInit, not a font the game already has. Until v0.23 the recorder registered no fonts
// and still asked for font 1, so every string it drew indexed an empty table and was dropped
// without a word - which is why the cue had never been seen in game since it shipped. The font
// is registered in DrawInit below; quads are unaffected, since sprite 0 means "fill with
// m_ulColor" and needs no table.
struct SPluginQuad_t {
    float         m_aafPos[4][2];  // corners, 0..1, counter-clockwise from the top left
    int           m_iSprite;       // 0 = solid fill
    unsigned long m_ulColor;       // ABGR
};
struct SPluginString_t {
    char          m_szString[100];
    float         m_afPos[2];      // 0..1, top-left origin
    int           m_iFont;         // 1-based; 1 = the game's own
    float         m_fSize;
    int           m_iJustify;      // 0 left, 1 centre, 2 right
    unsigned long m_ulColor;       // ABGR
};
SPluginQuad_t   g_quad[coachhud::kMaxQuads];
SPluginString_t g_text[coachhud::kMaxTexts];

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < (1u << 20)) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

std::string ReadText(const std::string& path) {
    std::vector<uint8_t> b = ReadFile(path);
    return std::string(b.begin(), b.end());
}

// ---------------------------------------------------------------------------------------
// The diagnostic log (coachlog.h).
//
// One file per run of the game, at <save path>\mxbcoach\mxbcoach.log, so "send me your log"
// means this session and not a year of them. Every decision the recorder makes about drawing,
// sheets, practice and sound goes in it: each one used to be invisible, which made a report of
// "the HUD does not work" impossible to tell apart from "the sheet was for another track".
// Flushed every line, because the game is often closed by being killed.
std::FILE* g_log       = nullptr;
size_t     g_log_bytes = 0;

std::string LogStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    snprintf(s, sizeof(s), "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

void Log(const char* tag, const std::string& message) {
    if (!g_log) return;
    const std::string line = coachlog::Line(LogStamp(), tag, message);
    // Full: stop writing rather than grow without limit. The head of the log holds the startup
    // decisions, which are the ones worth keeping.
    if (coachlog::ShouldRestart(g_log_bytes, line.size())) return;
    std::fwrite(line.data(), 1, line.size(), g_log);
    std::fflush(g_log);
    g_log_bytes += line.size();
}

void LogOpen() {
    if (g_log) std::fclose(g_log);
    g_log       = std::fopen((g_base + "mxbcoach.log").c_str(), "wb");
    g_log_bytes = 0;
}

void LogClose() {
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
}

/// The version that actually ran, for MXB Coach to show. A rider who believes they are on the
/// latest recorder and a plugins folder holding an older .dlo look identical from the app.
void WriteRecorderInfo() {
    std::FILE* f = std::fopen((g_base + "recorder.ini").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "[recorder]\nversion=%s\n", FROSTMOD_VERSION);
    std::fclose(f);
}

// The app's sheet for this track and bike, else for the track, if it was made for this track.
void LoadCues() {
    for (const std::string& name : coachcue::SheetNames(g_event)) {
        std::vector<uint8_t> b = ReadFile(g_base + "cues\\" + name);
        coachcue::Sheet s;
        if (!b.empty() && coachcue::Parse(b.data(), b.size(), s) && coachcue::Fits(s, g_event)) {
            const int cues = int(s.cues.size());
            g_cues.load(std::move(s));
            Log("cues", coachlog::SheetText("cue sheet", name, true, cues));
            return;
        }
    }
    g_cues.clear();
    Log("cues", coachlog::SheetText("cue sheet", "", false, 0));
}

// ---------------------------------------------------------------------------------------
// Speaking the cues.
//
// One waveOut device, opened on the first cue spoken and kept. Two buffers, so a new clip
// never touches one the device may still be reading. waveOutWrite returns at once; a buffer
// is finished when the device sets WHDR_DONE, checked before it is reused.

struct VoiceSlot {
    WAVEHDR              hdr{};
    std::vector<int16_t> pcm;
    bool                 prepared = false;
};

struct Voice {
    coachvoice::Settings settings;
    // voice.ini as last read: whether it was there, when it was written and its size.
    bool                 seen   = false;
    FILETIME             stamp{};
    DWORD                size   = 0;
    bool                 read   = false;
    float                next_check = 0;
    int                  loaded_volume = -1;  // the volume the clips below were scaled to
    std::vector<int16_t> clips[coachvoice::kClipCount];
    HWAVEOUT             out    = nullptr;
    bool                 failed = false;  // the device wouldn't open; not retried until the next event
    VoiceSlot            slots[2];
    uint8_t              priority = 0;  // of the cue last spoken
};
Voice g_voice;

HMODULE ThisModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ThisModule), &h);
    return h;
}

// The embedded clips, scaled to the rider's volume.
void LoadClips() {
    const HMODULE mod = ThisModule();
    for (int i = 0; i < coachvoice::kClipCount; ++i) {
        g_voice.clips[i].clear();
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(coachvoice::kResourceBase + i + 1), MAKEINTRESOURCEA(10));
        HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
        const void* p = data ? LockResource(data) : nullptr;
        std::vector<int16_t> pcm;
        if (p && coachvoice::ParseWav(static_cast<const uint8_t*>(p), SizeofResource(mod, res), pcm))
            g_voice.clips[i] = coachvoice::Scale(pcm, g_voice.settings.volume);
    }
    g_voice.loaded_volume = g_voice.settings.volume;
}

bool VoiceBusy() {
    for (const VoiceSlot& s : g_voice.slots)
        if (s.prepared && !(s.hdr.dwFlags & WHDR_DONE)) return true;
    return false;
}

// Hands back the buffers the device has finished with.
void ReapSlots() {
    for (VoiceSlot& s : g_voice.slots) {
        if (!s.prepared || !(s.hdr.dwFlags & WHDR_DONE)) continue;
        waveOutUnprepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr));
        s.prepared = false;
    }
}

// Silence now: on a crash, leaving practice, or the voice turned off.
void StopVoice() {
    if (!g_voice.out) return;
    waveOutReset(g_voice.out);  // marks every queued buffer done
    ReapSlots();
}

void CloseVoice() {
    StopVoice();
    if (g_voice.out) waveOutClose(g_voice.out);
    g_voice.out = nullptr;
}

// voice.ini, when it is new or has changed. Missing means off.
void ReadVoiceSettings(bool force) {
    const std::string path = g_base + "cues\\voice.ini";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool seen = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) != 0;
    if (!force && g_voice.read && seen == g_voice.seen &&
        (!seen || (CompareFileTime(&a.ftLastWriteTime, &g_voice.stamp) == 0 && a.nFileSizeLow == g_voice.size)))
        return;
    g_voice.read  = true;
    g_voice.seen  = seen;
    g_voice.stamp = a.ftLastWriteTime;
    g_voice.size  = a.nFileSizeLow;
    g_voice.settings = seen ? coachvoice::ReadSettings(path) : coachvoice::Settings{};
    if (!g_voice.settings.enabled) {
        StopVoice();
        Log("voice", coachlog::VoiceSettingsText(seen, false, g_voice.settings.volume, 0));
        return;
    }
    if (g_voice.loaded_volume != g_voice.settings.volume) {
        StopVoice();  // the buffers hold copies, but keep what's heard in step with the setting
        LoadClips();
    }
    // The clips are embedded, so zero of them with the voice on means the resources are not in
    // this build - which sounds exactly like a broken sound device from the rider's chair.
    int ready = 0;
    for (const std::vector<int16_t>& c : g_voice.clips) {
        if (!c.empty()) ++ready;
    }
    Log("voice", coachlog::VoiceSettingsText(seen, true, g_voice.settings.volume, ready));
}

bool OpenVoice() {
    if (g_voice.out) return true;
    if (g_voice.failed) return false;
    WAVEFORMATEX f{};
    f.wFormatTag      = WAVE_FORMAT_PCM;
    f.nChannels       = 1;
    f.nSamplesPerSec  = coachvoice::kRate;
    f.wBitsPerSample  = 16;
    f.nBlockAlign     = 2;
    f.nAvgBytesPerSec = coachvoice::kRate * 2;
    // The one step here that depends on the rider's machine, and the one that used to fail in
    // silence: no sound and no reason. The device is not retried until the next event.
    const MMRESULT mr = waveOutOpen(&g_voice.out, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        g_voice.out    = nullptr;
        g_voice.failed = true;
    }
    Log("voice", coachlog::VoiceDeviceText(g_voice.out != nullptr, uint32_t(mr)));
    return g_voice.out != nullptr;
}

// Says the cue that has just come up, by its kind. Never waits on the device.
void Speak(const coachcue::Cue& c) {
    if (!g_voice.settings.enabled || g_voice.settings.volume <= 0) return;
    const int clip = coachvoice::ClipFor(c.kind);
    if (clip < 0 || g_voice.clips[clip].empty() || !OpenVoice()) return;
    switch (coachvoice::Choose(VoiceBusy(), g_voice.priority, c.priority)) {
        case coachvoice::SKIP: return;
        case coachvoice::CUT_IN: waveOutReset(g_voice.out); break;
        case coachvoice::SPEAK: break;
    }
    ReapSlots();
    for (VoiceSlot& s : g_voice.slots) {
        if (s.prepared) continue;
        s.pcm = g_voice.clips[clip];
        s.hdr = WAVEHDR{};
        s.hdr.lpData         = reinterpret_cast<LPSTR>(s.pcm.data());
        s.hdr.dwBufferLength = DWORD(s.pcm.size() * sizeof(int16_t));
        if (waveOutPrepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) return;
        s.prepared = true;
        if (waveOutWrite(g_voice.out, &s.hdr, sizeof(s.hdr)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr));
            s.prepared = false;
            return;
        }
        g_voice.priority = c.priority;
        return;
    }
}

// ---------------------------------------------------------------------------------------
// The font the HUD draws its text with.
//
// A PiBoSo plugin draws text through its OWN font table: m_iFont indexes the zero-separated
// list handed back from DrawInit, and the base path for those names is the plugins folder
// (mxb_api.h). A plugin that registers nothing can draw no text at all, and the engine says
// nothing about it - the string is simply dropped.
//
// The .fnt is embedded and written out from inside DrawInit rather than shipped beside the
// .dlo. DrawInit is the only point at which the file is certainly on disk before the game
// reads it, and doing it here means a recorder installed by any means - the app, by hand, or
// already sitting in a plugins folder - brings its own font with it.
constexpr int         kFontResourceId = 200;
constexpr const char* kFontFolder     = "mxbcoach_data";
constexpr const char* kFontName       = "mxbcoach_data\\coach.fnt";
char                  g_font_name[64] = {0};  // handed to the game; must outlive DrawInit

bool WriteFont(uint32_t& bytes) {
    bytes = 0;
    if (g_plugins.empty()) return false;
    const HMODULE mod  = ThisModule();
    HRSRC         res  = FindResourceA(mod, MAKEINTRESOURCEA(kFontResourceId), MAKEINTRESOURCEA(10));
    HGLOBAL       data = res ? LoadResource(mod, res) : nullptr;
    const void*   p    = data ? LockResource(data) : nullptr;
    const DWORD   n    = res ? SizeofResource(mod, res) : 0;
    if (!p || n == 0) return false;
    CreateDirectoryA((g_plugins + kFontFolder).c_str(), nullptr);
    // Rewritten every run: the file belongs to this build, and one left half-written by a crash
    // would draw nothing at all.
    std::FILE* f = std::fopen((g_plugins + kFontName).c_str(), "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(p, 1, n, f);
    std::fclose(f);
    if (wrote != n) return false;
    bytes = n;
    return true;
}

// The app's HUD sheet, found the same way as the cues.
void LoadHud() {
    g_hud_sheet = coachhud::Sheet{};
    std::string taken;
    for (const std::string& name : coachhud::HudNames(g_event)) {
        std::vector<uint8_t> b = ReadFile(g_base + "cues\\" + name);
        coachhud::Sheet s;
        if (!b.empty() && coachhud::Parse(b.data(), b.size(), s) && coachhud::Fits(s, g_event)) {
            g_hud_sheet = std::move(s);
            taken       = name;
            break;
        }
    }
    g_ref.load(g_hud_sheet.ref);
    // Without a sheet the map, the stance and the setup card still draw; only the gap, the
    // ghost and the section tips need one, so this is a note rather than a failure.
    Log("hud", coachlog::SheetText("HUD sheet", taken, !taken.empty(), int(g_hud_sheet.sections.size())));
}

// hud.ini, when it's new or changed since the last read (or always, with `force`). MXBMRP3's
// own map, in the same plugins folder, turns ours off unless the file asks for it.
void ReadHudSettings(bool force) {
    const std::string path = g_base + "hud.ini";
    WIN32_FILE_ATTRIBUTE_DATA a;
    const bool seen = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) != 0;
    if (!force && seen == g_ini_seen && (!seen || CompareFileTime(&a.ftLastWriteTime, &g_ini_time) == 0)) return;
    g_ini_seen = seen;
    if (seen) g_ini_time = a.ftLastWriteTime;
    const bool mxbmrp3 = !g_plugins.empty() && GetFileAttributesA((g_plugins + "mxbmrp3.dlo").c_str()) != INVALID_FILE_ATTRIBUTES;
    g_hud_set = coachhud::ParseSettings(seen ? ReadText(path) : std::string(), mxbmrp3);
}

/// hud.ini as it ended up, since a part switched off looks exactly like a part that is broken.
void LogHudSettings() {
    const coachhud::Settings& s = g_hud_set;
    auto on = [](bool v) { return v ? "1" : "0"; };
    Log("hud.ini", std::string(g_ini_seen ? "found" : "no file, so every part is on") +
                       " enabled=" + on(s.enabled) + " cue=" + on(s.cue) + " section=" + on(s.section) +
                       " gap=" + on(s.gap) + " stance=" + on(s.stance) + " map=" + on(s.map) +
                       " setup=" + on(s.setup));
}

// At most once a second, from the telemetry callback rather than Draw.
void MaybeReloadHudSettings() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_ini_checked < 1000) return;
    g_ini_checked = now;
    ReadHudSettings(false);
}

std::string ModuleFolder() {
    const HMODULE self = ThisModule();
    if (!self) return {};
    char path[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path, n);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

// "20260914-153012-042": sortable, and a quick re-entry within a second doesn't collide.
std::string Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    snprintf(s, sizeof(s), "%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return s;
}

// ---------------------------------------------------------------------------------------
// Sit and stand: polling the rider's Sit bind.

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct SitInput {
    stance::Source        source = stance::SRC_NONE;
    int32_t               button = -1;
    int                   vk     = 0;
    IDirectInput8A*       di     = nullptr;
    IDirectInputDevice8A* dev    = nullptr;
    stance::Guid          dev_guid;  // as the bind names it
    XInputGetStateFn      xget  = nullptr;
    DWORD                 xslot = 0;
};
SitInput        g_sit;
stance::Tracker g_stance;

// The game's own top-level window: DirectInput wants one for its cooperative level.
HWND GameWindow() {
    struct Find {
        DWORD pid;
        HWND  hwnd;
    } f{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* f   = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            f->hwnd = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}

bool GameFocused() {
    HWND  h   = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

void ReleaseDevice() {
    if (!g_sit.dev) return;
    g_sit.dev->Unacquire();
    g_sit.dev->Release();
    g_sit.dev = nullptr;
}

enum class Open { OK, MISSING, FAILED };

// The attached controller the bind names, by instance GUID (as the game writes it) or product
// GUID, opened to read in the background without taking it from the game. Kept across stints
// while the bind is the same. MISSING: no attached device has that GUID, so the game can't
// read the bind either. FAILED: it may be there, but DirectInput wouldn't open it.
Open OpenDevice(const stance::Guid& want) {
    if (g_sit.dev && std::memcmp(&g_sit.dev_guid, &want, sizeof(want)) == 0) return Open::OK;
    ReleaseDevice();
    if (!g_sit.di &&
        FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                  reinterpret_cast<void**>(&g_sit.di), nullptr))) {
        g_sit.di = nullptr;
        return Open::FAILED;
    }
    struct Match {
        const stance::Guid* want;
        GUID                found;
        bool                ok;
    } m{&want, {}, false};
    g_sit.di->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        [](LPCDIDEVICEINSTANCEA d, LPVOID p) -> BOOL {
            auto* m = static_cast<Match*>(p);
            if (std::memcmp(&d->guidInstance, m->want, 16) != 0 && std::memcmp(&d->guidProduct, m->want, 16) != 0)
                return DIENUM_CONTINUE;
            m->found = d->guidInstance;
            m->ok    = true;
            return DIENUM_STOP;
        },
        &m, DIEDFL_ATTACHEDONLY);
    if (!m.ok) return Open::MISSING;
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(g_sit.di->CreateDevice(m.found, &dev, nullptr))) return Open::FAILED;
    HWND wnd = GameWindow();
    if (!wnd || FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(dev->SetCooperativeLevel(wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        dev->Release();
        return Open::FAILED;
    }
    dev->Acquire();
    g_sit.dev      = dev;
    g_sit.dev_guid = want;
    return Open::OK;
}

XInputGetStateFn LoadXInput() {
    static XInputGetStateFn fn    = nullptr;
    static bool             tried = false;
    if (tried) return fn;
    tried = true;
    for (const char* name : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"}) {
        HMODULE m = LoadLibraryA(name);
        if (!m) continue;
        fn = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
        if (fn) break;
    }
    return fn;
}

// Reads the rider's setup from their profile and gets ready to poll the bind. Writes the
// stint's STANCE_BIND record.
void SetupStance() {
    stance::Setup s;
    const std::string profile = stance::IniValue(ReadText(g_user + "global.ini"), "", "lastprofile");
    if (!profile.empty()) {
        const std::string dir = g_user + "profiles\\" + profile + "\\";
        s = stance::ReadSetup(ReadText(dir + "profile.ini"), ReadText(dir + "controls.txt"));
    }
    g_sit.source = stance::SRC_NONE;
    g_sit.button = s.bind.index;
    g_sit.vk     = 0;
    g_sit.xget   = nullptr;
    // Auto-sit makes every sample unknown; there is nothing to poll.
    if (s.auto_sit != stance::FLAG_ON && s.bind.input == stance::IN_KEY) {
        g_sit.vk = stance::FixedVk(s.bind.index);
        if (!g_sit.vk) g_sit.vk = int(MapVirtualKeyA(UINT(s.bind.index), MAPVK_VSC_TO_VK_EX));
        if (g_sit.vk) g_sit.source = stance::SRC_KEYBOARD;
    } else if (s.auto_sit != stance::FLAG_ON && s.bind.input == stance::IN_PAD_BUTTON) {
        // The bind's GUID already parsed in stance::ParseLine. XInput only when the pad is
        // there but DirectInput won't open it: the first XInput pad, so only a guess (Rate).
        stance::Guid g;
        stance::ParseGuid(s.bind.device, g);
        const Open o = OpenDevice(g);
        if (o == Open::OK) {
            g_sit.source = stance::SRC_DIRECTINPUT;
        } else if (o == Open::FAILED && stance::XInputMask(s.bind.index) && LoadXInput()) {
            XINPUT_STATE st;
            for (DWORD slot = 0; slot < 4; ++slot) {
                std::memset(&st, 0, sizeof(st));
                if (LoadXInput()(slot, &st) != ERROR_SUCCESS) continue;
                g_sit.xget   = LoadXInput();
                g_sit.xslot  = slot;
                g_sit.source = stance::SRC_XINPUT;
                break;
            }
        }
    }
    if (g_sit.source != stance::SRC_DIRECTINPUT) ReleaseDevice();
    const stance::Confidence c = stance::Rate(s, g_sit.source);
    const std::vector<uint8_t> p = stance::BindPayload(s, g_sit.source, c);
    g_rec.record(coachrec::STANCE_BIND, p.data(), uint32_t(p.size()));
    g_stance.configure(s.mode, c != stance::CONF_NONE);
    g_stance_conf = c;
}

// Whether the bind is down now. False when it can't be read.
bool ReadSit(bool& down) {
    down = false;
    switch (g_sit.source) {
        case stance::SRC_KEYBOARD:
            if (!GameFocused()) return false;
            down = (GetAsyncKeyState(g_sit.vk) & 0x8000) != 0;
            return true;
        case stance::SRC_DIRECTINPUT: {
            if (!g_sit.dev) return false;
            if (FAILED(g_sit.dev->Poll())) {
                g_sit.dev->Acquire();
                g_sit.dev->Poll();
            }
            DIJOYSTATE2 js;
            HRESULT hr = g_sit.dev->GetDeviceState(sizeof(js), &js);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                g_sit.dev->Acquire();
                hr = g_sit.dev->GetDeviceState(sizeof(js), &js);
            }
            if (FAILED(hr) || g_sit.button < 0 || g_sit.button >= 128) return false;
            down = (js.rgbButtons[g_sit.button] & 0x80) != 0;
            return true;
        }
        case stance::SRC_XINPUT: {
            XINPUT_STATE st;
            std::memset(&st, 0, sizeof(st));
            if (!g_sit.xget || g_sit.xget(g_sit.xslot, &st) != ERROR_SUCCESS) return false;
            down = (st.Gamepad.wButtons & stance::XInputMask(g_sit.button)) != 0;
            return true;
        }
        default: return false;
    }
}

void PollStance(float time, float pos, bool crashed) {
    if (!g_rec.recording()) return;
    bool down     = false;
    const bool ok = ReadSit(down);
    if (!g_stance.update(ok, down, crashed)) return;
    const auto p = stance::EventPayload(time, pos, g_stance.state());
    g_rec.record(coachrec::STANCE, p.data(), uint32_t(p.size()));
}

// Everyone already in the event, at the top of a stint.
void WriteRoster() {
    for (const auto& kv : g_others.roster())
        g_rec.record(coachrec::ENTRY, kv.second.data(), uint32_t(kv.second.size()));
}

// Everything the HUD shows this frame. Under g_mu.
void BuildHud() {
    coachhud::View v;
    v.set = g_hud_set;
    v.cue = g_cues.showing();
    const float m = g_pos * g_event.track_len;
    if (g_have_sample) {
        v.section = coachhud::SectionAt(g_hud_sheet, m);
        v.has_gap = coachhud::Gap(g_ref, g_clock, g_time, g_pos, v.gap);
        if (g_clock.valid()) v.has_ghost = g_ref.world_at(g_clock.elapsed(g_time), v.ghost.x, v.ghost.y);
        v.has_rider = true;
        v.rider     = g_rider;
        v.stopped   = g_stop.stopped();
        if (g_cues.active()) v.upcoming = coachhud::Upcoming(g_cues.sheet(), m, 5);
    }
    v.stance = g_stance.state();
    v.conf   = g_stance_conf;
    v.track  = &g_track;
    v.setup  = g_setup;
    v.sag    = (g_hud_sheet.flags & coachhud::FLAG_SAG) != 0;
    coachhud::Build(v, g_frame);
}

}  // namespace

extern "C" {

// The game drops a plugin whose identity disagrees with its build; offsets.h holds MX Bikes'.
__declspec(dllexport) char* GetModID() {
    static char id[32] = {0};
    if (!id[0]) strncpy_s(id, sizeof(id), GAME_MXB.plugin_id, _TRUNCATE);
    return id;
}
__declspec(dllexport) int GetModDataVersion() { return GAME_MXB.plugin_data_version; }
__declspec(dllexport) int GetInterfaceVersion() { return kPluginInterfaceVersion; }

__declspec(dllexport) int Startup(char* _szSavePath) {
    std::string dir = _szSavePath ? _szSavePath : "";
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    const std::string user = dir;
    dir += "mxbcoach\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_user    = user;
        g_base    = dir;
        g_plugins = ModuleFolder();
        LogOpen();
        Log("mxbcoach", std::string(FROSTMOD_VERSION) + " starting");
        Log("paths", "save=" + g_user + " plugins=" + g_plugins);
        WriteRecorderInfo();
        ReadHudSettings(true);
        LogHudSettings();
    }
    dir += "sessions\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.set_dir(dir);
    return coachrec::kTelemetryRate;
}

__declspec(dllexport) void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event_end();
    ReleaseDevice();
    if (g_sit.di) g_sit.di->Release();
    g_sit.di = nullptr;
    CloseVoice();
    Log("mxbcoach", "shutting down");
    LogClose();
}

__declspec(dllexport) void EventInit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event(_pData, _iDataSize);
    g_event = coachcue::ReadEvent(_pData, _iDataSize);
    Log("event", coachlog::EventText(g_event.type, g_event.track, g_event.bike, g_event.track_len, g_event.server));
    LoadCues();
    LoadHud();
    g_voice.failed = false;
    ReadVoiceSettings(true);
    // Before any stint, only a testing event is practice; a race event waits for its session.
    g_practice = g_event.type == 1;
    g_cues.set_practice(g_practice);
}

__declspec(dllexport) void EventDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event_end();
    g_cues.clear();
    g_hud_sheet = coachhud::Sheet{};
    g_ref.load({});
    g_track.clear();
    g_practice = false;
    ReleaseDevice();
    StopVoice();
}

// _pRaceData: floats, the start/finish line's distance along the centreline first.
__declspec(dllexport) void TrackCenterline(int _iNumSegments, void* _pasSegment, void* _pRaceData) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_centreline(_iNumSegments, _pasSegment, coachrec::kTrackSegmentSize);
    g_track.build(_iNumSegments, _pasSegment, coachrec::kTrackSegmentSize, static_cast<const float*>(_pRaceData));
}

__declspec(dllexport) void RunInit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_stance_conf = stance::CONF_NONE;
    if (g_rec.on_run_init(_pData, _iDataSize, Stamp().c_str())) {
        SetupStance();
        g_others.on_stint();
        WriteRoster();
    }
    const int session = coachcue::ReadSession(_pData, _iDataSize);
    g_practice        = coachcue::Practice(g_event.type, session);
    g_cues.set_practice(g_practice);
    Log("run", coachlog::PracticeText(g_event.type, session, g_practice));
    g_logged_draw = false;
    ReadVoiceSettings(false);
    g_setup = coachhud::SetupName(_pData, _iDataSize);
    g_clock.reset();
    g_stop.reset();
    g_have_sample = false;
    ReadHudSettings(true);
}

__declspec(dllexport) void RunDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_run_end();
    g_cues.set_practice(false);
    StopVoice();
    g_practice    = false;
    g_have_sample = false;
}

__declspec(dllexport) void RunStart() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_start();
}

__declspec(dllexport) void RunStop() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_stop();
}

__declspec(dllexport) void RunLap(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_lap(_pData, _iDataSize);
    g_cues.on_lap();
}

__declspec(dllexport) void RunSplit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_split(_pData, _iDataSize);
}

__declspec(dllexport) void RunTelemetry(void* _pData, int _iDataSize, float _fTime, float _fPos) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_sample(_pData, _iDataSize, _fTime, _fPos);
    g_others.on_sample(_fTime, _pData, _iDataSize);
    bool crashed = false;
    if (_pData && _iDataSize >= int(kDataCrashed + 4)) {
        const auto* b = static_cast<const uint8_t*>(_pData);
        crashed       = coachcue::U32(b + kDataCrashed) != 0;
        const float    speed = coachcue::F32(b + kDataSpeed);
        const uint32_t fired = g_cues.fires();
        g_cues.on_sample(_fPos, speed, _fTime, crashed);
        // Spoken the moment it shows. A cue the player skipped never fires, so is never said.
        if (crashed) {
            StopVoice();
        } else if (g_cues.fires() != fired && g_cues.showing()) {
            const coachcue::Cue& c = *g_cues.showing();
            Speak(c);
            Log("cue", "\"" + c.text + "\" at " + std::to_string(int(c.at_m)) + "m");
        }
        g_clock.on_sample(_fTime, _fPos);
        g_stop.on_sample(_fTime, speed);
        g_rider       = {coachcue::F32(b + kDataPosX), coachcue::F32(b + kDataPosZ)};
        g_time        = _fTime;
        g_pos         = _fPos;
        g_have_sample = true;
    }
    // voice.ini changed while riding: MXB Coach can be switched to mid-stint.
    if (_fTime >= g_voice.next_check || _fTime + 2.0f < g_voice.next_check) {
        g_voice.next_check = _fTime + 2.0f;
        ReadVoiceSettings(false);
    }
    PollStance(_fTime, _fPos, crashed);
    MaybeReloadHudSettings();
}

// The other riders. The roster is kept whether or not a stint is recording, since the entries
// arrive when the event starts; everything else is written only while one is.
__declspec(dllexport) void RaceEvent(void*, int) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_others.clear_roster();
}

__declspec(dllexport) void RaceDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_others.clear_roster();
}

__declspec(dllexport) void RaceAddEntry(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    others::Entry e;
    if (g_others.added(_pData, _iDataSize, e) && g_rec.recording())
        g_rec.record(coachrec::ENTRY, e.data(), uint32_t(e.size()));
}

__declspec(dllexport) void RaceRemoveEntry(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    others::Entry e;
    if (g_others.removed(_pData, _iDataSize, e) && g_rec.recording())
        g_rec.record(coachrec::ENTRY, e.data(), uint32_t(e.size()));
}

__declspec(dllexport) void RaceTrackPosition(int _iNumVehicles, void* _pArray, int _iElemSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_rec.recording()) return;
    std::vector<uint8_t> p;
    if (g_others.positions(_iNumVehicles, _pArray, _iElemSize, p))
        g_rec.record(coachrec::POSITIONS, p.data(), uint32_t(p.size()));
}

__declspec(dllexport) void RaceLap(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<uint8_t> p;
    if (g_rec.recording() && g_others.lap(_pData, _iDataSize, p))
        g_rec.record(coachrec::RACE_LAP, p.data(), uint32_t(p.size()));
}

__declspec(dllexport) void RaceSplit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<uint8_t> p;
    if (g_rec.recording() && g_others.split(_pData, _iDataSize, p))
        g_rec.record(coachrec::RACE_SPLIT, p.data(), uint32_t(p.size()));
}

// Called once when the game starts, to register the sprites and fonts this plugin draws with.
// The names are zero-separated and resolve against the plugins folder; the buffer must outlive
// the call, so it is a static. Returns 0 when the pointers are set, -1 when they are not, as
// PiBoSo's own mxb_example.c does.
//
// This is what makes our text appear at all - see the font block above.
__declspec(dllexport) int DrawInit(int* _piNumSprites, char** _pszSpriteName, int* _piNumFonts,
                                   char** _pszFontName) {
    if (_piNumSprites) *_piNumSprites = 0;
    if (_pszSpriteName) *_pszSpriteName = nullptr;
    if (_piNumFonts) *_piNumFonts = 0;
    if (_pszFontName) *_pszFontName = nullptr;
    std::lock_guard<std::mutex> lock(g_mu);
    uint32_t   bytes = 0;
    const bool ok    = WriteFont(bytes);
    Log("drawinit", coachlog::DrawInitText(ok, kFontName, bytes));
    if (!ok || !_piNumFonts || !_pszFontName) return -1;
    strncpy_s(g_font_name, sizeof(g_font_name), kFontName, _TRUNCATE);
    *_piNumFonts  = 1;
    *_pszFontName = g_font_name;
    return 0;
}

// Each frame on track, spectating or in a replay: the HUD, only while riding in practice. The
// cue sits below MXBMRP3's Notices and Timing panels, the rest round it (coachhud.h). Nothing
// may throw into the game, so the body is guarded like MXBMRP3's API_GUARD_CATCH.
__declspec(dllexport) void Draw(int _iState, int* _piNumQuads, void** _ppQuad, int* _piNumString,
                                void** _ppString) {
    int quads = 0, strings = 0;
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        if (_iState == 0 && g_practice) {
            BuildHud();
            for (const coachhud::Quad& in : g_frame.quads) {
                if (quads >= int(coachhud::kMaxQuads)) break;
                SPluginQuad_t& q = g_quad[quads++];
                std::memcpy(q.m_aafPos, in.p, sizeof(q.m_aafPos));
                q.m_iSprite = in.sprite;
                q.m_ulColor = in.color;
            }
            for (const coachhud::Text& in : g_frame.texts) {
                if (strings >= int(coachhud::kMaxTexts)) break;
                SPluginString_t& s = g_text[strings++];
                strncpy_s(s.m_szString, in.s.c_str(), _TRUNCATE);
                s.m_afPos[0] = in.x;
                s.m_afPos[1] = in.y;
                s.m_iFont    = 1;
                s.m_fSize    = in.size;
                s.m_iJustify = in.justify;
                s.m_ulColor  = in.color;
            }
        }
        // One line a stint, whatever it drew. Zero quads while riding says the HUD was switched
        // off or this is not practice; the line proves the game is calling us either way, which
        // is the first thing worth knowing when nothing shows.
        if (!g_logged_draw) {
            g_logged_draw = true;
            Log("draw", coachlog::DrawText(_iState, g_practice, quads, strings));
        }
    } catch (...) {
        quads = strings = 0;
    }
    if (_piNumQuads) *_piNumQuads = quads;
    if (_ppQuad) *_ppQuad = g_quad;
    if (_piNumString) *_piNumString = strings;
    if (_ppString) *_ppString = g_text;
}

}  // extern "C"
