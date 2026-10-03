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
#include <GL/gl.h>
#include <mmsystem.h>
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>

#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "coachcue.h"
#include "coachline.h"
#include "coachhud.h"
#include "coachgear.h"
#include "coachjump.h"
#include "coachpace.h"
#include "coachlog.h"
#include "coachrec.h"
#include "coachterrain.h"
#include "coachvoice.h"
#include "offsets.h"
#include "others.h"
#include "lean.h"
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

// SPluginsBikeData_t: speedometer m/s, world x and z, the shocks' lengths, and the crashed
// flag. Every field ahead of these is a 4-byte int or float in mxb_api.h's struct, so the
// offsets are a straight walk of it - and the four that were already here landing exactly on
// the fields they are named for is what says the walk is right.
/// How far the bike must travel before its heading is worked out again. Short enough to turn
/// with the track, long enough that a bike sitting still doesn't spin its own arrow.
constexpr float  kDirStepM       = 1.5f;

/// How long the pointer stays up after the mouse stops moving.
constexpr ULONGLONG kPointerHoldMs = 2500;

constexpr size_t kDataGear       = 12;  // i32: 0 = neutral, 1.. (SPluginsBikeData_t m_iGear)
constexpr size_t kDataSpeed      = 20;
constexpr size_t kDataPosX       = 24;
constexpr size_t kDataPosY       = 28;
constexpr size_t kDataPosZ       = 32;
constexpr size_t kDataVelX       = 36;  // f32[3] m/s
constexpr size_t kDataSuspLength = 120;  // f32[2] metres: 0 = front, 1 = rear
constexpr size_t kDataCrashed    = 136;
constexpr size_t kDataWheelMat   = 168;  // i32[2]: the ground under each wheel, 0 = none (in the air)

// SPluginsBikeEvent_t: m_afSuspMaxTravel, f32[2] metres, 0 = front and 1 = rear. It is what
// the shocks' lengths are a proportion of, and it only arrives with the event.
constexpr size_t kEventSuspMaxTravel = 332;

// The HUD (coachhud.h).
std::string          g_plugins;  // the folder this .dlo was loaded from
coachhud::Sheet      g_hud_sheet;
// The track's own ground (`<track>.ground`, written by MXB Coach from the .trh), and the one-time
// check that it lines up with the rider (coachline::AlignCheck).
coachline::GroundGrid g_grid;
coachline::AlignCheck g_align;
std::string           g_grid_name;
ULONGLONG             g_grid_look_ms = 0;
bool                  g_grid_said_missing = false;
std::string           g_grid_bad;  // the stamp of a grid file that didn't parse, not read again unchanged
// The game's own ground (coachterrain.h): the game's height query, sampled into a grid in this
// process and preferred over the `.ground` file. Never written out, logged or sent anywhere.
// What the render thread reads, under g_mu:
coachline::GroundGrid        g_live;
coachline::AlignCheck        g_live_align;
const coachline::GroundGrid* g_ground_was = nullptr;  // the ground the line used last frame
// The sampler's own: touched only by LiveGroundStep, on the telemetry thread, and never under
// g_mu. The game's Draw waits on g_mu, so nothing that asks the game anything may hold it (0.46.0
// held it from the first telemetry tick and the game hung).
coachterrain::game::Terrain g_terrain;
int                         g_terrain_state = 0;  // 0 not looked yet, 1 found, -1 off until the game restarts
coachterrain::Builder       g_live_b;
coachterrain::Pacer         g_live_pace;
coachterrain::Header        g_live_hd, g_live_seen;  // what g_live was made from; the last look at the slot
bool                        g_live_built = false, g_live_said = false;
bool                        g_live_off = false;  // the watchdog tripped: off for the event
int                         g_live_slot = 0;     // the slot being tried, 0-based
ULONGLONG                   g_live_t0 = 0, g_live_check_ms = 0;
// Asked of the sampler from under g_mu: 1 start again from slot 1 (event start or end), 2 try
// the next slot (the one sampled is not under the rider).
std::atomic<int> g_live_req{0};
// Since when the rider has been riding (on track, moving, not crashed); 0 when not. RunTelemetry.
ULONGLONG g_ride_since = 0;
// Crashes, getting up and the game putting the bike back (coachline::RiderState).
coachline::RiderState g_rider_state;
// What the sheet adds to the line on the track: the ground across it and where its lap braked.
coachline::RibbonExtras g_extras;
// Pace hints over the line (coachpace.h): Coach's lap as they need it, the filtered hint, and
// the line's extras with the hint's colours, handed to the ribbon while a hint shows.
coachpace::Profile      g_pace_prof;
coachpace::Filter       g_pace;
coachpace::Hint         g_pace_hint;
coachline::RibbonExtras g_pace_ex;
uint32_t                g_pace_ver   = 0;
int                     g_pace_idx   = -1;
float                   g_pace_frac  = 0, g_pace_t = -1;
long                    g_pace_key   = -1;  // what the colours were last made for
float                   g_pace_phase = 0;   // the rider's metres along Coach's line, for the chevrons
coachpace::Kind         g_pace_said  = coachpace::NONE;
bool                    g_pace_grid_lips = false;  // the lips have been looked for on the ground grid
// Gear hints (coachgear.h): where Coach's lap shifts, the filtered hint, and whether the rider is
// in the air.
coachgear::Profile      g_gear_prof;
coachgear::Filter       g_gear;
coachgear::Hint         g_gear_hint;
coachgear::AirTracker   g_air;
float                   g_gear_t = -1;
coachgear::Dir          g_gear_said = coachgear::NONE;
float                   g_line_total  = 0;     // metres round Coach's line, for the snap's bins
coachline::Arc          g_arc;                 // metres along Coach's line at each sheet point
float                   g_rider_m     = NAN;   // the rider's place on it this frame, metres (NaN: not known)
volatile bool           g_dsnap_reset = true;  // the render thread restarts the ground snap
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
float g_rider_y = 0;  // the rider's world y, for the ground line's height bias
float g_rider_v[3] = {0, 0, 0};  // the bike's velocity, m/s, for the fallback helmet camera
coachhud::Pt         g_rider;
// Which way the bike is pointing, as a world x/z direction, and the point it was last worked
// out from. The plugin API hands a heading only to the race-position callback, which a rider
// practising alone never sees — so it comes from where the bike has travelled instead, which
// is true whenever it is moving and holds its last value when it stops.
coachhud::Pt         g_rider_dir{};
coachhud::Pt         g_dir_from{};
bool                 g_have_dir_from = false;
stance::Confidence   g_stance_conf = stance::CONF_NONE;
// The suspension: what the event said each shock's travel is, how much of it is in use now,
// and the deepest each has been this stint (the mark where it bottomed).
float                g_susp_travel[2] = {0, 0};
float                g_susp[2]        = {0, 0};
float                g_susp_deep[2]   = {0, 0};
bool                 g_have_susp      = false;
// hud.ini as last read: whether it was there and when it was written.
bool                 g_ini_seen = false;
FILETIME             g_ini_time = {};
// A part being dragged with the right button, and where in it the rider took hold.
struct DragState {
    coachhud::Part held = coachhud::PART_NONE;
    float          dx = 0, dy = 0;
    bool           was_down = false;
    // Where the pointer was, and when it last moved. The pointer is only drawn for a moment
    // after the rider moves the mouse: on screen the whole time it would be one more thing
    // over the track, and never drawn they would be aiming something they cannot see.
    float          at_x = 0, at_y = 0;
    bool           has_at = false;
    ULONGLONG      moved_ms = 0;
};
DragState            g_drag;
/// Whether the last telemetry sample said the rider was down, so the voice is stopped on the
/// crash rather than on every sample of lying in the dirt.
bool                 g_was_crashed = false;
ULONGLONG            g_ini_checked = 0;
// Whether this stint has already written its one "the game called Draw" line, and how many
// frames it has been asked to draw. Zero at the end of a stint means the game never called
// Draw at all - which no line inside Draw could ever tell us.
bool                 g_logged_draw = false;
uint32_t             g_draw_calls  = 0;

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
/// The sheet Coach last wrote, and when. MXB Coach picks the calls again as the rider rides —
/// that is the whole point of them moving on — so a sheet loaded once and kept for the event
/// is the same cues at the same metres every lap, however hard the app works.
std::string g_cue_file;
std::string g_cue_seen;       // DiskStamp of the candidates at the last look
ULONGLONG   g_cue_looked = 0;  // GetTickCount64 of the last look

/// Which of a sheet's candidate files are on disk, and when each was written, as one string.
/// A look compares it with the last one's, so a file is read only when it has changed. A
/// sheet that is there but made for another length of track is not re-read every second.
std::string DiskStamp(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA((g_base + "cues\\" + name).c_str(), GetFileExInfoStandard, &a)) continue;
        out += name + "@" + std::to_string(a.ftLastWriteTime.dwHighDateTime) + ":" +
               std::to_string(a.ftLastWriteTime.dwLowDateTime) + ";";
    }
    return out;
}

void LoadCues(bool quiet = false) {
    g_cue_seen = DiskStamp(coachcue::SheetNames(g_event));
    for (const std::string& name : coachcue::SheetNames(g_event)) {
        const std::string path = g_base + "cues\\" + name;
        std::vector<uint8_t> b = ReadFile(path);
        coachcue::Sheet s;
        if (!b.empty() && coachcue::Parse(b.data(), b.size(), s) && coachcue::Fits(s, g_event)) {
            const int cues = int(s.cues.size());
            g_cues.load(std::move(s));
            g_cue_file = path;
            if (!quiet) Log("cues", coachlog::SheetText("cue sheet", name, true, cues));
            else Log("cues", "picked up a newer sheet: " + std::to_string(cues) + " cues");
            return;
        }
    }
    g_cues.clear();
    g_cue_file.clear();
    if (!quiet) Log("cues", coachlog::SheetText("cue sheet", "", false, 0));
}

/// Take a sheet the app has written since the last look; coachcue::ShouldLook says when.
void LookForCues(bool at_line) {
    const ULONGLONG now = GetTickCount64();
    if (!coachcue::ShouldLook(!g_cue_file.empty(), at_line, now, g_cue_looked)) return;
    g_cue_looked = now;
    const std::string seen = DiskStamp(coachcue::SheetNames(g_event));
    // Nothing new, or the file went away: keep what is loaded rather than go quiet mid-lap.
    if (seen == g_cue_seen || seen.empty()) return;
    LoadCues(true);
}

// ---------------------------------------------------------------------------------------
// Speaking the cues.
//
// One waveOut device, opened on the first cue spoken and kept. Two buffers, handed to the
// device a clip at a time. Which buffers are ours and which the device still holds is
// coachvoice::Queue's business (coachvoice.h), where the rule can be tested without Win32.
//
// The rule, and what went wrong with it: a buffer is ours again only when winmm has BOTH
// finished with it (WHDR_DONE) AND accepted waveOutUnprepareHeader for it. v0.22 and v0.23
// took the flag as enough and cleared their own "in use" bit whatever the unprepare returned,
// so a buffer the driver still owned was refilled underneath it - its vector reallocated, its
// header zeroed - and from then on the completion flag the driver wrote landed in memory that
// had been reused. The slot never read as finished again. Two buffers means exactly two clips
// and then silence, which is what the rider heard: "Stand up", "Gas", nothing after.
//
// Two things changed besides the ownership. Buffers are drained every telemetry sample instead
// of only when the next cue turns up, so one comes back within 20 ms of its clip ending rather
// than waiting on a cue that may be half a lap away. And every waveOut call's MMRESULT is
// logged, because a voice that dies mid-session leaves no other trace: the device is open, the
// clips are loaded, the cues fire, and nothing is heard.

struct VoiceSlot {
    WAVEHDR              hdr{};
    std::vector<int16_t> pcm;
    bool                 prepared = false;  // prepared, and not yet accepted back by an unprepare
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
    int                  loaded_voice  = -1;  // and which voice's clips they are
    std::vector<int16_t> clips[coachvoice::kClipCount];
    HWAVEOUT             out    = nullptr;
    bool                 failed = false;  // the device wouldn't open; not retried until the next event
    VoiceSlot            slots[coachvoice::Queue::kSlots];
    coachvoice::Queue    queue;
};
Voice g_voice;

/// Which voice to speak in, never out of range whatever voice.ini said.
uint8_t VoiceChoice() {
    return g_voice.settings.voice < coachvoice::kVoiceCount ? g_voice.settings.voice : uint8_t(coachvoice::FEMALE);
}

/// Every waveOut call goes through here, so every one of them is in the log with its result.
MMRESULT VoiceCall(const char* name, int slot, MMRESULT mr) {
    Log("voice", coachlog::VoiceCallText(name, slot, uint32_t(mr)));
    return mr;
}

HMODULE ThisModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ThisModule), &h);
    return h;
}

// The embedded clips for the chosen voice, scaled to the rider's volume.
void LoadClips() {
    const HMODULE mod   = ThisModule();
    const uint8_t voice = VoiceChoice();
    for (int i = 0; i < coachvoice::kClipCount; ++i) {
        g_voice.clips[i].clear();
        const int id  = coachvoice::ResourceId(voice, coachvoice::kClips[i].kind);
        HRSRC     res = FindResourceA(mod, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10));
        HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
        const void* p = data ? LockResource(data) : nullptr;
        std::vector<int16_t> pcm;
        if (p && coachvoice::ParseWav(static_cast<const uint8_t*>(p), SizeofResource(mod, res), pcm))
            g_voice.clips[i] = coachvoice::Scale(pcm, g_voice.settings.volume);
    }
    g_voice.loaded_volume = g_voice.settings.volume;
    g_voice.loaded_voice  = int(voice);
}

// Takes back every buffer the device has finished with. A slot becomes ours again only when
// the unprepare is ACCEPTED: WAVERR_STILLPLAYING means the driver still holds that memory, and
// refilling it then is the fault that silenced the voice. A slot it refuses is simply left
// with the device and tried again on the next sample.
void Drain() {
    if (!g_voice.out) return;
    for (int i = 0; i < coachvoice::Queue::kSlots; ++i) {
        VoiceSlot& s = g_voice.slots[i];
        if (!s.prepared || !(s.hdr.dwFlags & WHDR_DONE)) continue;
        if (VoiceCall("waveOutUnprepareHeader", i,
                      waveOutUnprepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr))) != MMSYSERR_NOERROR)
            continue;
        s.prepared = false;
        g_voice.queue.release(i);
    }
}

// Silence now: on a crash, leaving practice, or the voice turned off. waveOutReset makes the
// device finish with everything it holds; the buffers still have to be taken back, which Drain
// does here and again on the samples after, if the driver wasn't ready on this one.
void StopVoice() {
    if (!g_voice.out) return;
    VoiceCall("waveOutReset", -1, waveOutReset(g_voice.out));
    Drain();
}

void CloseVoice() {
    StopVoice();
    if (g_voice.out) {
        VoiceCall("waveOutClose", -1, waveOutClose(g_voice.out));
        g_voice.out = nullptr;
    }
    // The device is gone, so it holds nothing, whatever it held a moment ago.
    for (VoiceSlot& s : g_voice.slots) s.prepared = false;
    g_voice.queue.clear();
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
        Log("voice", coachlog::VoiceSettingsText(seen, false, g_voice.settings.volume, 0,
                                                 coachvoice::kVoices[VoiceChoice()].key));
        return;
    }
    // A new volume or a new voice means the loaded clips are the wrong ones: stop, so nothing
    // half-spoken carries over in the old voice, and load the set the rider asked for.
    if (g_voice.loaded_volume != g_voice.settings.volume || g_voice.loaded_voice != int(VoiceChoice())) {
        StopVoice();
        LoadClips();
    }
    // The clips are embedded, so zero of them with the voice on means the resources are not in
    // this build - which sounds exactly like a broken sound device from the rider's chair.
    int ready = 0;
    for (const std::vector<int16_t>& c : g_voice.clips) {
        if (!c.empty()) ++ready;
    }
    Log("voice", coachlog::VoiceSettingsText(seen, true, g_voice.settings.volume, ready,
                                             coachvoice::kVoices[VoiceChoice()].key));
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
    const MMRESULT mr =
        VoiceCall("waveOutOpen", -1, waveOutOpen(&g_voice.out, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL));
    if (mr != MMSYSERR_NOERROR) {
        g_voice.out    = nullptr;
        g_voice.failed = true;
    } else {
        // A device just opened holds nothing, whatever the last one was left holding.
        for (VoiceSlot& s : g_voice.slots) s.prepared = false;
        g_voice.queue.clear();
    }
    Log("voice", coachlog::VoiceDeviceText(g_voice.out != nullptr, uint32_t(mr)));
    return g_voice.out != nullptr;
}

// Says the cue that has just come up, by its kind. Never waits on the device.
void Speak(const coachcue::Cue& c) {
    if (!g_voice.settings.enabled || g_voice.settings.volume <= 0) return;
    const int clip = coachvoice::ClipFor(c.kind);
    if (clip < 0 || g_voice.clips[clip].empty() || !OpenVoice()) return;
    Drain();  // whatever has finished is ours again before we go looking for a buffer
    switch (coachvoice::Choose(g_voice.queue.playing(), g_voice.queue.priority(), c.priority)) {
        case coachvoice::SKIP: return;
        case coachvoice::CUT_IN:
            VoiceCall("waveOutReset", -1, waveOutReset(g_voice.out));
            Drain();
            break;
        case coachvoice::SPEAK: break;
    }
    const int slot = g_voice.queue.take(c.priority);
    if (slot < 0) {
        // Both buffers are still with the device. Say nothing rather than queue up behind
        // them, and write down the counts: this line is what a voice running dry looks like.
        Log("voice", coachlog::VoiceBuffersText(g_voice.queue.taken(), g_voice.queue.released(),
                                                g_voice.queue.in_flight()));
        return;
    }
    VoiceSlot& s = g_voice.slots[slot];
    s.pcm        = g_voice.clips[clip];
    s.hdr        = WAVEHDR{};
    s.hdr.lpData         = reinterpret_cast<LPSTR>(s.pcm.data());
    s.hdr.dwBufferLength = DWORD(s.pcm.size() * sizeof(int16_t));
    if (VoiceCall("waveOutPrepareHeader", slot, waveOutPrepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr))) !=
        MMSYSERR_NOERROR) {
        g_voice.queue.giveback(slot);  // the device never took it
        return;
    }
    s.prepared = true;
    if (VoiceCall("waveOutWrite", slot, waveOutWrite(g_voice.out, &s.hdr, sizeof(s.hdr))) != MMSYSERR_NOERROR) {
        if (VoiceCall("waveOutUnprepareHeader", slot,
                      waveOutUnprepareHeader(g_voice.out, &s.hdr, sizeof(s.hdr))) == MMSYSERR_NOERROR) {
            s.prepared = false;
            g_voice.queue.giveback(slot);
        }
        // If even the unprepare is refused the buffer stays out, and Drain picks it up later.
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

// The app's HUD sheet, found the same way as the cues, and looked for again the same way: the
// gap and the ghost need Coach's lap, which on a new track only exists after the first lap.
std::string g_hud_file;
std::string g_hud_seen;
ULONGLONG   g_hud_looked = 0;

/// The line's extras from the sheet just loaded: its TRRN ground, and the colour of its lap (from
/// its DRIV chunk, or its own times and positions).
void BuildExtras() {
    const uint32_t ver = g_extras.version + 1;
    g_extras           = coachline::RibbonExtras{};
    g_extras.version   = ver;
    if (!g_hud_sheet.terrain.empty()) {
        g_extras.terrain_k    = g_hud_sheet.terrain_k;
        g_extras.terrain_step = g_hud_sheet.terrain_step;
        g_extras.terrain      = g_hud_sheet.terrain;
    }
    g_extras.refy = g_hud_sheet.refy;
    g_line_total  = 0;
    for (size_t k = 1; k < g_hud_sheet.ref.size(); ++k) {
        const float d = std::hypot(g_hud_sheet.ref[k].x - g_hud_sheet.ref[k - 1].x, g_hud_sheet.ref[k].z - g_hud_sheet.ref[k - 1].z);
        g_line_total += d < coachline::kMaxGapM ? d : 0.0f;
    }
    if (!g_hud_sheet.ref.empty()) {
        const float d = std::hypot(g_hud_sheet.ref.front().x - g_hud_sheet.ref.back().x, g_hud_sheet.ref.front().z - g_hud_sheet.ref.back().z);
        g_line_total += d < coachline::kMaxGapM ? d : 0.0f;
    }
    g_arc         = coachline::ArcOf(g_hud_sheet.ref);
    g_dsnap_reset = true;
    // The one line to change for a different colouring: any function giving RGBA per point.
    g_extras.rgba = coachline::LineColours(g_hud_sheet.ref, g_hud_sheet.drive);
    if (!g_hud_sheet.ref.empty()) {
        // Whether the line can lie on the track's own ground, and if not, why: the one thing a
        // rider seeing it float needs to know. MXB Coach writes TRRN from v0.43.2's sheet format
        // on, and only when it found the track's terrain and the laps sat steadily on it.
        Log("ground", std::string("sheet terrain=") + (g_extras.terrain_k ? "yes" : "no") +
                          (g_extras.terrain_k
                               ? " (the track's own ground under the line)"
                               : g_hud_sheet.drive.empty()
                                     ? " (an older sheet: Coach rewrites it after your first whole lap here; until then "
                                       "the line stands on your own ground and snaps to what the game draws)"
                                     : " (Coach couldn't use this track's terrain; the line stands on your own ground "
                                       "and snaps to what the game draws)"));
        Log("ground", std::string("colours: source=") + coachline::ColourSource(g_hud_sheet.ref, g_hud_sheet.drive) +
                          ", " +
                          std::to_string(coachline::ToneZones(g_hud_sheet.ref, coachline::Tone(g_hud_sheet.ref, g_hud_sheet.drive)).size()) +
                          " braking zones");
    }
    // Pace hints: Coach's speeds along the line, and the jump lips from the track's own ground
    // down the middle of it (none without TRRN).
    std::vector<float> heights;
    if (g_extras.terrain_k) {
        heights.assign(g_hud_sheet.ref.size(), NAN);
        for (size_t i = 0; i < heights.size(); ++i) coachline::TerrainAt(g_extras, i, 0.0f, heights[i]);
    }
    g_pace_prof = coachpace::BuildProfile(g_hud_sheet.ref, g_hud_sheet.drive, heights);
    g_pace.reset();
    g_pace_hint = coachpace::Hint{};
    g_pace_ex   = g_extras;
    g_pace_key  = -1;
    g_pace_t    = -1;
    g_pace_said = coachpace::NONE;
    g_pace_grid_lips = false;
    if (g_pace_prof.ok())
        Log("pace", std::string("speeds from ") + (g_hud_sheet.drive.empty() ? "the sheet's times" : "DRIV") + ", " +
                        std::to_string(g_pace_prof.lips.size()) + " jump lips");
    // Gear hints: Coach's shifts from the sheet's GEAR.
    g_gear_prof = coachgear::BuildProfile(g_hud_sheet.ref, g_hud_sheet.gear, heights, g_hud_sheet.refy);
    g_gear.reset();
    g_gear_hint = coachgear::Hint{};
    g_gear_t    = -1;
    g_gear_said = coachgear::NONE;
    // No sheet at all is not an old Coach: it just hasn't written one for this track yet.
    Log("gear", g_hud_sheet.ref.empty()    ? "no sheet yet: no gear hints until MXB Coach writes one"
                : g_hud_sheet.gear.empty() ? "no GEAR in the sheet (MXB Coach is older than the gear hints): no gear hints"
                                           : std::to_string(g_gear_prof.shifts.size()) + " shifts in Coach's lap");
}

/// Whether gear hints are worked out: asked for, with Coach's gears.
bool GearOn() { return g_hud_set.enabled && g_hud_set.gear && g_gear_prof.ok(); }

/// One telemetry sample for the gear hints. Under g_mu.
void GearSample(int gear, float speed, float time, bool crashed) {
    if (!GearOn()) {
        g_gear_hint = coachgear::Hint{};
        g_gear.reset();
        return;
    }
    const float dt = g_gear_t >= 0 && time > g_gear_t && time - g_gear_t < 0.5f ? time - g_gear_t : 0.02f;
    g_gear_t       = time;
    float      frac = 0;
    const int  idx  = coachline::AnchorPoint(g_hud_sheet.ref, g_rider.x, g_rider.y, g_pos, frac);
    const bool air  = g_air.update(g_have_susp, g_susp[0], g_susp[1], dt);
    const coachgear::Reading r =
        coachgear::Read(g_gear_prof, coachgear::PhaseAt(g_gear_prof, g_hud_sheet.ref, idx, frac), gear, speed, crashed, air);
    g_gear_hint = g_gear.step(r, dt, g_gear_prof.lap_m);
    if (r.dir != g_gear_said) {
        g_gear_said = r.dir;
        if (r.dir != coachgear::NONE) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s to %d (gear %d, Coach shifts %.0f m ahead)", r.dir == coachgear::UP ? "up" : "down",
                          r.target, gear, double(r.ahead_m));
            Log("gear", b);
        }
    }
}

/// Whether pace hints are drawn: asked for, over the line on the track, with Coach's speeds.
bool PaceOn() { return g_hud_set.enabled && g_hud_set.ground && g_hud_set.pace && g_pace_prof.ok(); }

/// One telemetry sample for the pace hints. Under g_mu.
void PaceSample(float speed, float time, bool crashed) {
    if (!PaceOn()) {
        g_pace_hint = coachpace::Hint{};
        return;
    }
    // Without TRRN, the jump lips come from the track's own ground grid once it is known to line
    // up with the track (it arrives after the sheet).
    if (!g_extras.terrain_k && g_extras.grid && !g_pace_grid_lips) {
        g_pace_grid_lips = true;
        std::vector<float> h(g_hud_sheet.ref.size(), NAN);
        for (size_t i = 0; i < h.size(); ++i) g_extras.grid->at(g_hud_sheet.ref[i].x, g_hud_sheet.ref[i].z, h[i]);
        g_pace_prof.lips = coachpace::FindLips(g_pace_prof.s, h, g_pace_prof.v);
        Log("pace", std::to_string(g_pace_prof.lips.size()) + " jump lips from the track's ground grid");
    }
    const float dt = g_pace_t >= 0 && time > g_pace_t && time - g_pace_t < 0.5f ? time - g_pace_t : 0.02f;
    g_pace_t       = time;
    g_pace_idx     = coachline::AnchorPoint(g_hud_sheet.ref, g_rider.x, g_rider.y, g_pos, g_pace_frac);
    const coachpace::Reading r =
        crashed ? coachpace::Reading{} : coachpace::Read(g_pace_prof, g_pace_idx, g_pace_frac, speed);
    g_pace_hint  = g_pace.step(r, dt);
    g_pace_phase = coachpace::PhaseAt(g_pace_prof, g_pace_idx, g_pace_frac);
    if (g_pace.wanted() != g_pace_said) {
        g_pace_said = g_pace.wanted();
        char b[96];
        std::snprintf(b, sizeof(b), "%s (%+.0f%% on Coach, overshoot %.0f m, corner %.0f m, lip %.0f m)",
                      g_pace_said == coachpace::FAST ? "too fast" : g_pace_said == coachpace::SLOW ? "too slow" : "on pace",
                      double(r.rel * 100.0f), double(r.overshoot_m), double(r.brake_m), double(r.lip_m));
        Log("pace", b);
    }
    // The line's colours with the hint, remade only when what they show changed: the strength
    // in twentieths, the advance in half metres, the point the rider is at.
    const long key = g_pace_hint.kind == coachpace::NONE
                         ? 0
                         : long(g_pace_hint.kind) + 4L * (long(g_pace_hint.level * 20.0f) +
                                                          32L * (long(g_pace_hint.advance_m * 2.0f) + 64L * (g_pace_idx + 1)));
    if (key == g_pace_key) return;
    g_pace_key      = key;
    g_pace_ex.rgba  = coachpace::Recolour(g_pace_prof, g_extras.rgba, g_pace_idx, g_pace_frac, g_pace_hint);
    g_pace_ex.version = 0x80000000u | (++g_pace_ver & 0x7FFFFFFFu);
}

/// `<save>\mxbcoach\cues\<track>.ground`: the track's own ground. Looked for at the event and then
/// once a second until it is there, since MXB Coach writes it within a second or two of seeing
/// the track ridden.
void LoadGround(bool quiet) {
    g_grid_look_ms = GetTickCount64();
    if (g_event.track.empty() || g_base.empty()) return;
    const std::string name = coachcue::SafeName(g_event.track) + ".ground";
    const std::string path = g_base + "cues\\" + name;
    // A grid that didn't parse is read again only once it has changed: it used to be read whole
    // (up to 64 MB) every second, under the lock the render thread's HUD draw waits on.
    std::string               stamp;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa))
        stamp = path + "|" + std::to_string(fa.nFileSizeLow) + "|" + std::to_string(fa.ftLastWriteTime.dwLowDateTime) +
                "|" + std::to_string(fa.ftLastWriteTime.dwHighDateTime);
    if (quiet && !stamp.empty() && stamp == g_grid_bad) return;
    FILE*             f    = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (!quiet || !g_grid_said_missing)
            Log("ground", "no track grid (" + name + ") yet: MXB Coach writes it from the track's .trh when it sees "
                          "the track ridden; a locked (.mxbsecure) track can't be read, and there the line snaps "
                          "to what the game draws instead");
        g_grid_said_missing = true;
        return;
    }
    std::vector<uint8_t> buf;
    uint8_t              chunk[65536];
    size_t               got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0 && buf.size() < (64u << 20)) buf.insert(buf.end(), chunk, chunk + got);
    std::fclose(f);
    if (!g_grid.parse(buf.data(), buf.size())) {
        g_grid_bad = stamp;
        Log("ground", "track grid " + name + " didn't read; the line snaps to what the game draws instead");
        return;
    }
    g_grid_name = name;
    g_align.reset();
    char msg[200];
    std::snprintf(msg, sizeof(msg), "track grid %s: %ux%u at %.2f m, on from the first second; checking it lines up",
                  name.c_str(), g_grid.width(), g_grid.height(), double(g_grid.step()));
    Log("ground", msg);
}

/// The game's ground, back to nothing: a new event or its end. Under g_mu.
void ResetLiveGround() {
    g_live.clear();
    g_live_align.reset();
    g_live_req.store(1);
}

/// The game's ground, once it lines up with the rider (or until the check says).
bool LiveGroundOn() { return g_live.ready() && (!g_live_align.result().done || g_live_align.result().ok); }

/// A marker kept on disk only while a grid is being asked for (it holds the version, nothing of
/// the ground). Still there when the game next starts means the game hung or died mid-way: the
/// sampling then stays off for that version rather than hang it again.
std::string LiveBusyPath() { return g_base + "terrain.busy"; }
void        LiveBusy(bool on) {
    if (!on) {
        DeleteFileA(LiveBusyPath().c_str());
        return;
    }
    if (std::FILE* f = std::fopen(LiveBusyPath().c_str(), "wb")) {
        std::fputs(FROSTMOD_VERSION, f);
        std::fclose(f);
    }
}
/// At startup, under g_mu.
void LiveBusyCheck() {
    const std::string was = ReadText(LiveBusyPath());
    if (was.empty()) return;
    if (was == FROSTMOD_VERSION) {
        g_terrain_state = -1;
        Log("terrain", "off: the last time this version asked the game for its ground it never finished (the game "
                       "hung or was closed mid-way). The line uses the track's grid file, or snaps to what the game "
                       "draws. Delete mxbcoach\\terrain.busy to try again");
        return;
    }
    LiveBusy(false);
}

/// The next slot with a heightfield in it, from `g_live_slot` on; -1 when there is none.
int NextLiveSlot() {
    for (int i = g_live_slot; i < mxb::TRACK_SLOT_COUNT; ++i)
        if (coachterrain::Plausible(coachterrain::ReadHeader(coachterrain::game::Slot(g_terrain, i)))) return i;
    return -1;
}

/// What RunTelemetry hands the sampler, read under g_mu.
struct LiveIn {
    bool      on        = false;  // asked for, a practice run, telemetry flowing
    ULONGLONG riding_ms = 0;      // how long the rider has been riding
};
/// What a slice hands back, applied under g_mu.
struct LiveOut {
    std::vector<std::string> said;
    bool                     publish = false, clear = false;
    coachline::GroundGrid    grid;
};

/// One slice of the game's ground. Not under g_mu (see g_terrain). Only once the rider has been
/// riding a few seconds and the slot's heightfield has settled; at most a millisecond of asking a
/// slice and a tenth of wall time, and off for the event if the watchdog trips.
void LiveGroundSlice(const LiveIn& in, LiveOut& out) {
    if (!in.on || g_live_off || g_terrain_state < 0 || double(in.riding_ms) < coachterrain::kSettleMs) return;
    if (g_terrain_state == 0) {
        std::string why;
        if (coachterrain::game::Locate(g_terrain, why)) {
            g_terrain_state = 1;
            char msg[220];
            std::snprintf(msg, sizeof(msg),
                          "the game's height query is at RVA 0x%zx%s, track slots at RVA 0x%zx: the line samples the "
                          "game's own ground, in memory only",
                          size_t(mxb::RVA_TERRAIN_SAMPLE), g_terrain.behind_hook ? " (behind FrostMod's guard)" : "",
                          size_t(mxb::RVA_TRACK_SLOTS));
            out.said.push_back(msg);
        } else {
            g_terrain_state = -1;
            out.said.push_back("off: " + why + ". The line uses the track's grid file, or snaps to what the game draws");
            return;
        }
    }
    const ULONGLONG now = GetTickCount64();
    // Built: still the slot's heightfield? A track reloaded under us is a different one.
    if (g_live_built) {
        if (now - g_live_check_ms < 1000) return;
        g_live_check_ms = now;
        if (coachterrain::ReadHeader(coachterrain::game::Slot(g_terrain, g_live_slot)) != g_live_hd) {
            out.said.push_back("the game's ground changed under the line (track reloaded); sampling it again");
            out.clear    = true;
            g_live_built = false;
            g_live_slot  = 0;
            g_live_seen  = {};
        }
        return;
    }
    if (g_live_b.state() != coachterrain::Builder::RUNNING) {
        if (now - g_live_check_ms < 1000) return;
        g_live_check_ms = now;
        const int slot  = NextLiveSlot();
        if (slot < 0) {
            if (!g_live_said)
                out.said.push_back(g_live_slot ? "no other track slot has a heightfield; the line uses the track's grid "
                                                 "file, or snaps to what the game draws"
                                               : "no heightfield loaded in any track slot yet");
            g_live_said = true;
            return;
        }
        g_live_said                   = false;
        const coachterrain::Header hd = coachterrain::ReadHeader(coachterrain::game::Slot(g_terrain, slot));
        // Settled: the same heightfield on two looks a second apart.
        if (slot != g_live_slot || hd != g_live_seen) {
            g_live_slot = slot;
            g_live_seen = hd;
            return;
        }
        g_live_b.start(hd);
        const coachterrain::Plan& p = g_live_b.plan();
        char                      msg[220];
        std::snprintf(msg, sizeof(msg), "sampling the game's ground, slot %d: %dx%d over %.0fx%.0f m, as %ux%u at %.2f m",
                      slot + 1, hd.cols, hd.rows, double(hd.size_x), double(hd.size_z), p.w, p.h, double(p.step));
        out.said.push_back(msg);
        g_live_t0 = now;
        g_live_pace.reset();
        LiveBusy(true);
        return;  // the first slice on the next tick, once this line is in the log
    }
    const uint8_t* slot = coachterrain::game::Slot(g_terrain, g_live_slot);
    if (coachterrain::ReadHeader(slot) != g_live_b.header()) {
        out.said.push_back("the slot's heightfield changed while it was sampled; starting again");
        g_live_b.reset();
        g_live_seen = {};
        LiveBusy(false);
        return;
    }
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const double ms_per_tick = 1000.0 / double(f.QuadPart);
    const double start_ms    = double(t0.QuadPart) * ms_per_tick;
    if (!g_live_pace.may_run(start_ms)) return;
    const LONGLONG cap = LONGLONG(coachterrain::Pacer::kSliceMs / ms_per_tick);
    const auto     ask = [&](float x, float z, float& y) { return coachterrain::game::Ask(g_terrain, slot, x, z, y); };
    do {
        g_live_b.run(ask, 64);
        QueryPerformanceCounter(&t);
    } while (g_live_b.state() == coachterrain::Builder::RUNNING && t.QuadPart - t0.QuadPart < cap);
    const double slice_ms = double(t.QuadPart - t0.QuadPart) * ms_per_tick;
    char         msg[260];
    if (!g_live_pace.done(start_ms + slice_ms, slice_ms)) {
        std::snprintf(msg, sizeof(msg),
                      "off for this event: the game took %.1f ms to answer one slice (%.0f ms in all), over the "
                      "watchdog's budget. The line uses the track's grid file, or snaps to what the game draws",
                      slice_ms, g_live_pace.busy_ms());
        out.said.push_back(msg);
        g_live_off = true;
        g_live_b.reset();
        LiveBusy(false);
        return;
    }
    switch (g_live_b.state()) {
        case coachterrain::Builder::DONE: {
            const double share = 100.0 * double(g_live_b.answered()) / double(g_live_b.total());
            g_live_hd          = g_live_b.header();
            out.grid           = g_live_b.take();
            out.publish        = true;
            g_live_built       = true;
            g_live_check_ms    = now;
            LiveBusy(false);
            std::snprintf(msg, sizeof(msg),
                          "the game's ground is ready: %ux%u at %.2f m, %.1f%% of it answered, after %.1f s (%.0f ms of "
                          "asking); checking it lines up",
                          out.grid.width(), out.grid.height(), double(out.grid.step()), share,
                          double(now - g_live_t0) / 1000.0, g_live_pace.busy_ms());
            out.said.push_back(msg);
            break;
        }
        case coachterrain::Builder::FAILED:
            if (g_live_b.asked() < g_live_b.total()) {
                out.said.push_back("the game's height query faulted; off until the game restarts. The line uses the "
                                   "track's grid file, or snaps to what the game draws");
                g_terrain_state = -1;
            } else {
                std::snprintf(msg, sizeof(msg), "slot %d answered too little of its grid to be the track's ground",
                              g_live_slot + 1);
                out.said.push_back(msg);
                ++g_live_slot;
                g_live_seen = {};
            }
            g_live_b.reset();
            LiveBusy(false);
            break;
        default: break;
    }
}

/// The game's ground, from RunTelemetry after it let go of g_mu: the telemetry thread, the one
/// the physics asks the same question on. g_mu only to hand the grid and the log lines over.
void LiveGroundStep(const LiveIn& in) {
    switch (g_live_req.exchange(0)) {
        case 1:
            g_live_off  = false;
            g_live_slot = 0;
            g_live_pace.reset();
            if (g_live_b.state() == coachterrain::Builder::RUNNING) LiveBusy(false);
            g_live_b.reset();
            g_live_built = g_live_said = false;
            g_live_seen  = {};
            break;
        case 2:
            if (g_live_built) ++g_live_slot;
            g_live_b.reset();
            g_live_built = g_live_said = false;
            g_live_seen  = {};
            break;
        default: break;
    }
    LiveOut out;
    LiveGroundSlice(in, out);
    if (out.said.empty() && !out.publish && !out.clear) return;
    std::lock_guard<std::mutex> lock(g_mu);
    // An event that started or ended meanwhile: what was made is not its ground.
    const bool stale = g_live_req.load() == 1;
    if (out.clear) {
        g_live.clear();
        g_live_align.reset();
    }
    if (out.publish && !stale) {
        g_live = std::move(out.grid);
        g_live_align.reset();
    }
    for (const std::string& m : out.said) Log("terrain", m);
}

/// The one-time check that the game's ground is under the rider, as for the grid file. Under g_mu.
void LiveGroundAlign(float x, float y, float z, bool grounded) {
    if (!g_live.ready() || !g_live_align.add(g_live, x, y, z, grounded)) return;
    const coachline::Alignment& a = g_live_align.result();
    char                        msg[240];
    std::snprintf(msg, sizeof(msg), "game ground aligned dx=%.0f dz=%.0f dy=%.2f spread=%.2f (%d samples on it, %d off it)%s",
                  double(a.dx), double(a.dz), double(a.dy), double(a.spread), a.on_grid, a.off_grid,
                  a.ok ? "" : " - NOT under the rider; trying the next track slot");
    Log("terrain", msg);
    if (a.ok) {
        g_live.shift(a.dx, a.dz);
        return;
    }
    g_live.clear();
    g_live_align.reset();
    int expect = 0;
    g_live_req.compare_exchange_strong(expect, 2);
}

void LoadHud(bool quiet = false) {
    g_hud_seen = DiskStamp(coachhud::HudNames(g_event));
    coachhud::Sheet found;
    std::string     taken;
    for (const std::string& name : coachhud::HudNames(g_event)) {
        std::vector<uint8_t> b = ReadFile(g_base + "cues\\" + name);
        coachhud::Sheet s;
        if (!b.empty() && coachhud::Parse(b.data(), b.size(), s) && coachhud::Fits(s, g_event)) {
            found = std::move(s);
            taken = name;
            break;
        }
    }
    // A newer file that doesn't parse leaves the sheet in use alone: dropping it for nothing
    // would blank the gap and the ghost mid-session. Its stamp is kept above, so it isn't
    // re-read every second either; the next write Coach makes is looked at again.
    if (quiet && taken.empty()) {
        if (!g_hud_file.empty()) Log("hud", "a newer sheet didn't read; keeping the one in use");
        return;
    }
    g_hud_sheet = std::move(found);
    g_hud_file  = taken;
    g_ref.load(g_hud_sheet.ref);
    BuildExtras();
    // Without a sheet the map, the stance and the setup card still draw; only the gap, the
    // ghost and the section tips need one, so this is a note rather than a failure.
    if (!quiet) {
        Log("hud", coachlog::SheetText("HUD sheet", g_hud_file, !g_hud_file.empty(), int(g_hud_sheet.sections.size())));
    } else {
        Log("hud", "picked up a newer sheet: " + std::to_string(g_hud_sheet.sections.size()) + " sections");
    }
}

void LookForHud(bool at_line) {
    const ULONGLONG now = GetTickCount64();
    if (!coachcue::ShouldLook(!g_hud_file.empty(), at_line, now, g_hud_looked)) return;
    g_hud_looked = now;
    const std::string seen = DiskStamp(coachhud::HudNames(g_event));
    if (seen == g_hud_seen || seen.empty()) return;
    LoadHud(true);
}

/// The line's look from hud.ini into the drawing code. On a change the line's colours are made
/// again and the ribbon, the pace colours and the marks rebuilt, so what the rider picks in Coach
/// shows within the second the file is re-read.
void ApplyLook() {
    const coachhud::LineLook& l = g_hud_set.look;
    if (coachline::Look() == l) return;
    coachline::Look() = l;
    g_extras.rgba     = coachline::LineColours(g_hud_sheet.ref, g_hud_sheet.drive);
    ++g_extras.version;
    g_pace_key = -1;  // the pace hint's colours are made from the line's
    char b[160];
    std::snprintf(b, sizeof(b), "line look: width x%.2f opacity %.2f text=%d size x%.2f style=%s", l.width, l.opacity,
                  l.text ? 1 : 0, l.text_size,
                  l.text_style == coachhud::TEXT_BOLD ? "bold" : l.text_style == coachhud::TEXT_ITALIC ? "italic" : "block");
    Log("hud.ini", b);
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
    ApplyLook();
}

/// hud.ini as it ended up, since a part switched off looks exactly like a part that is broken.
void LogHudSettings() {
    const coachhud::Settings& s = g_hud_set;
    auto on = [](bool v) { return v ? "1" : "0"; };
    Log("hud.ini", std::string(g_ini_seen ? "found" : "no file, so every part is on") +
                       " enabled=" + on(s.enabled) + " cue=" + on(s.cue) + " section=" + on(s.section) +
                       " gap=" + on(s.gap) + " stance=" + on(s.stance) + " map=" + on(s.map) +
                       " setup=" + on(s.setup) + " trail=" + on(s.trail) + " ground=" + on(s.ground) +
                       " pace=" + on(s.pace) + " gear=" + on(s.gear));
}

// At most once a second, from the telemetry callback rather than Draw.
void MaybeReloadHudSettings() {
    // Not while a part is being dragged: re-reading the file would snap it back to where it
    // was before the rider picked it up.
    if (g_drag.held != coachhud::PART_NONE) return;
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

// Rider lean: the two body-position binds. Its own device handle rather than the Sit one,
// because lean and Sit need not be bound on the same pad; the IDirectInput8A is shared.
struct LeanAxis {
    stance::Source source  = stance::SRC_NONE;
    int32_t        axis    = -1;  // the axis number the bind names
    int32_t        sign    = -1;
    int            vk_neg  = 0, vk_pos = 0;
    int32_t        btn_neg = -1, btn_pos = -1;
};
struct LeanInput {
    LeanAxis              lr, fb;
    IDirectInputDevice8A* dev = nullptr;
    stance::Guid          dev_guid;
};
LeanInput g_lean;

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

// ---------------------------------------------------------------------------------------
// Right-drag a part to move it
//
// The plugin API hands out no mouse at all, so this is read from Win32 the way the Sit bind
// already is. Everything that can be decided without Windows — what is under the cursor, where
// the part lands, how hud.ini is rewritten — is in coachhud.h, where it is tested.

/// The cursor over the game's window, as screen fractions. False when the game isn't focused,
/// has no window, or the cursor is outside it — a drag can't start from any of those.
bool CursorOnGame(float& x, float& y) {
    if (!GameFocused()) return false;
    const HWND h = GameWindow();
    if (!h) return false;
    POINT p;
    RECT  r;
    if (!GetCursorPos(&p) || !ScreenToClient(h, &p) || !GetClientRect(h, &r)) return false;
    if (r.right <= r.left || r.bottom <= r.top) return false;
    x = float(p.x) / float(r.right - r.left);
    y = float(p.y) / float(r.bottom - r.top);
    return x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
}

/// hud.ini with a part's position written into it, everything else left alone. Written aside
/// and moved in, so a read landing mid-write never sees half a file.
void SavePartPosition(coachhud::Part part) {
    const std::string path = g_base + "hud.ini";
    const std::string text = coachhud::WithHudKeys(ReadText(path), coachhud::PartKeys(g_hud_set, part));
    const std::string tmp  = path + ".tmp";
    std::FILE*        f    = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return;
    }
    // Our own write, so don't let the next check read it back as someone else's change.
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) {
        g_ini_seen = true;
        g_ini_time = a.ftLastWriteTime;
    }
}

/// Hold the right button over the map, the bars or the cue and it follows the cursor; let go
/// and where it ended up is written to hud.ini. Off with `move=0`.
void PollDrag() {
    if (!g_hud_set.enabled || !g_hud_set.move) {
        g_drag.held   = coachhud::PART_NONE;
        g_drag.has_at = false;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    float      x = 0, y = 0;
    const bool on = CursorOnGame(x, y);
    // Moving the mouse is what asks for the pointer. Held, it stays up regardless, so it never
    // fades out from under a part being dragged.
    if (on) {
        const bool moved = !g_drag.has_at || std::fabs(x - g_drag.at_x) > 0.0015f ||
                           std::fabs(y - g_drag.at_y) > 0.0015f;
        if (moved) g_drag.moved_ms = GetTickCount64();
        g_drag.at_x = x, g_drag.at_y = y, g_drag.has_at = true;
    } else {
        g_drag.has_at = false;
    }
    if (down && !g_drag.was_down && on) {
        // A press starts a drag only when it lands on a part, so right-clicking anywhere else
        // goes on meaning whatever the game wants it to mean.
        const coachhud::Part hit = coachhud::PartAt(g_hud_set, x, y);
        coachhud::Box        b;
        if (hit != coachhud::PART_NONE && coachhud::PartBox(g_hud_set, hit, b)) {
            g_drag.held = hit;
            g_drag.dx   = x - b.x0;
            g_drag.dy   = y - b.y0;
            Log("hud.move", std::string("picked up ") + coachhud::PartName(hit));
        }
    } else if (down && g_drag.held != coachhud::PART_NONE && on) {
        coachhud::SetPartOrigin(g_hud_set, g_drag.held, x - g_drag.dx, y - g_drag.dy);
    } else if (!down && g_drag.held != coachhud::PART_NONE) {
        SavePartPosition(g_drag.held);
        Log("hud.move", std::string("put down ") + coachhud::PartName(g_drag.held));
        g_drag.held = coachhud::PART_NONE;
    }
    g_drag.was_down = down;
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
void SetupLean(const lean::Setup& s);

void SetupStance() {
    stance::Setup s;
    // Both readers want the same two files, so they are read once and shared.
    std::string profile_ini, controls;
    const std::string profile = stance::IniValue(ReadText(g_user + "global.ini"), "", "lastprofile");
    if (!profile.empty()) {
        const std::string dir = g_user + "profiles\\" + profile + "\\";
        profile_ini = ReadText(dir + "profile.ini");
        controls    = ReadText(dir + "controls.txt");
        s = stance::ReadSetup(profile_ini, controls);
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
    SetupLean(lean::ReadSetup(profile_ini, controls));
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

// Which member of the joystick state an axis number means.
//
// ASSUMED, NOT PROVEN. The game's axis number indexes its own per-device table, and this is
// the natural DirectInput ordering. It has to be confirmed against a real pad - bind
// CTRL_LRLEAN to a known stick and see which of these moves - before any reading from it is
// worth trusting. Until then the records carry the axis number too, so a wrong mapping can be
// spotted and corrected without re-recording.
LONG AxisValue(const DIJOYSTATE2& js, int32_t n) {
    switch (n) {
        case 0: return js.lX;
        case 1: return js.lY;
        case 2: return js.lZ;
        case 3: return js.lRx;
        case 4: return js.lRy;
        case 5: return js.lRz;
        case 6: return js.rglSlider[0];
        case 7: return js.rglSlider[1];
        default: return 0;
    }
}

constexpr LONG kAxisLo = -32768;
constexpr LONG kAxisHi = 32767;

void ReleaseLeanDevice() {
    if (!g_lean.dev) return;
    g_lean.dev->Unacquire();
    g_lean.dev->Release();
    g_lean.dev = nullptr;
}

/// Opens the lean pad and puts every axis on one known range, so two devices give comparable
/// numbers. Buttons need no range, which is why nothing set one before.
bool OpenLeanDevice(const stance::Guid& want) {
    if (g_lean.dev && std::memcmp(&g_lean.dev_guid, &want, sizeof(want)) == 0) return true;
    ReleaseLeanDevice();
    if (OpenDevice(want) != Open::OK || !g_sit.dev) return false;
    // The Sit path just opened it; take our own reference so releasing one doesn't kill the
    // other, and leave the Sit handle exactly as it was.
    g_lean.dev = g_sit.dev;
    g_lean.dev->AddRef();
    g_lean.dev_guid = want;
    DIPROPRANGE r;
    r.diph.dwSize       = sizeof(r);
    r.diph.dwHeaderSize = sizeof(r.diph);
    r.diph.dwObj        = 0;
    r.diph.dwHow        = DIPH_DEVICE;
    r.lMin              = kAxisLo;
    r.lMax              = kAxisHi;
    g_lean.dev->SetProperty(DIPROP_RANGE, &r.diph);
    return true;
}

/// One axis of the rider's lean setup, from its bind.
void SetupLeanAxis(LeanAxis& out, const lean::Axis& a) {
    out = LeanAxis{};
    if (a.aid == stance::FLAG_ON) return;
    out.sign = a.bind.sign;
    if (a.bind.input == stance::IN_KEY) {
        const auto vk = [](int32_t dik) {
            int v = stance::FixedVk(dik);
            if (!v) v = int(MapVirtualKeyA(UINT(dik), MAPVK_VSC_TO_VK_EX));
            return v;
        };
        out.vk_neg = vk(a.bind.index);
        out.vk_pos = a.bind.index2 >= 0 ? vk(a.bind.index2) : 0;
        if (out.vk_neg || out.vk_pos) out.source = stance::SRC_KEYBOARD;
    } else if (a.bind.input == stance::IN_PAD_AXIS || a.bind.input == stance::IN_PAD_BUTTON) {
        stance::Guid g;
        if (!stance::ParseGuid(a.bind.device, g) || !OpenLeanDevice(g)) return;
        if (a.bind.input == stance::IN_PAD_AXIS) {
            out.axis = a.bind.index;
        } else {
            out.btn_neg = a.bind.index;
            out.btn_pos = a.bind.index2;
        }
        out.source = stance::SRC_DIRECTINPUT;
    }
}

/// Reads both lean binds and writes the stint's LEAN_BIND record.
void SetupLean(const lean::Setup& s) {
    SetupLeanAxis(g_lean.lr, s.lr);
    SetupLeanAxis(g_lean.fb, s.fb);
    if (g_lean.lr.source != stance::SRC_DIRECTINPUT && g_lean.fb.source != stance::SRC_DIRECTINPUT)
        ReleaseLeanDevice();
    // Both axes are read through whichever source actually opened, so rate each on its own.
    uint8_t buf[lean::kBindSize];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = lean::kBindLayout;
    lean::WriteAxis(buf + 4, s.lr, lean::Rate(s.lr, g_lean.lr.source), g_lean.lr.source);
    lean::WriteAxis(buf + 4 + lean::kAxisSize, s.fb, lean::Rate(s.fb, g_lean.fb.source), g_lean.fb.source);
    g_rec.record(coachrec::LEAN_BIND, buf, uint32_t(sizeof(buf)));
}

/// Where one axis is now, -1..+1, or unknown when it can't be read.
float ReadLeanAxis(const LeanAxis& a, const DIJOYSTATE2& js, bool have_js) {
    switch (a.source) {
        case stance::SRC_KEYBOARD: {
            if (!GameFocused()) return lean::Unknown();
            const bool neg = a.vk_neg && (GetAsyncKeyState(a.vk_neg) & 0x8000) != 0;
            const bool pos = a.vk_pos && (GetAsyncKeyState(a.vk_pos) & 0x8000) != 0;
            return lean::FromPair(neg, pos);
        }
        case stance::SRC_DIRECTINPUT: {
            if (!have_js) return lean::Unknown();
            if (a.axis >= 0 && a.axis < 8) return lean::FromAxis(int32_t(AxisValue(js, a.axis)), kAxisLo, kAxisHi, a.sign);
            if (a.btn_neg >= 0 && a.btn_neg < 128) {
                const bool neg = (js.rgbButtons[a.btn_neg] & 0x80) != 0;
                const bool pos = a.btn_pos >= 0 && a.btn_pos < 128 && (js.rgbButtons[a.btn_pos] & 0x80) != 0;
                return lean::FromPair(neg, pos);
            }
            return lean::Unknown();
        }
        default: return lean::Unknown();
    }
}

void PollLean(float time, float pos) {
    if (!g_rec.recording()) return;
    if (g_lean.lr.source == stance::SRC_NONE && g_lean.fb.source == stance::SRC_NONE) return;
    DIJOYSTATE2 js;
    std::memset(&js, 0, sizeof(js));
    bool have_js = false;
    if (g_lean.dev) {
        if (FAILED(g_lean.dev->Poll())) {
            g_lean.dev->Acquire();
            g_lean.dev->Poll();
        }
        HRESULT hr = g_lean.dev->GetDeviceState(sizeof(js), &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            g_lean.dev->Acquire();
            hr = g_lean.dev->GetDeviceState(sizeof(js), &js);
        }
        have_js = SUCCEEDED(hr);
    }
    const float lr = ReadLeanAxis(g_lean.lr, js, have_js);
    const float fb = ReadLeanAxis(g_lean.fb, js, have_js);
    if (!lean::IsKnown(lr) && !lean::IsKnown(fb)) return;
    uint8_t buf[lean::kEventSize];
    lean::WriteEvent(buf, time, pos, lr, fb);
    g_rec.record(coachrec::LEAN, buf, uint32_t(sizeof(buf)));
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
        v.rider_dir = g_rider_dir;
        v.pos       = g_pos;
        v.stopped   = g_stop.stopped();
        if (g_cues.active()) v.upcoming = coachhud::Upcoming(g_cues.sheet(), m, 5);
    }
    // The pointer, while the rider is moving the mouse or holding a part.
    if (g_hud_set.move && g_drag.has_at &&
        (g_drag.held != coachhud::PART_NONE || GetTickCount64() - g_drag.moved_ms < kPointerHoldMs)) {
        v.has_pointer = true;
        v.pointer     = {g_drag.at_x, g_drag.at_y};
    }
    v.stance = g_stance.state();
    v.conf   = g_stance_conf;
    v.track  = &g_track;
    v.setup  = g_setup;
    v.sag    = (g_hud_sheet.flags & coachhud::FLAG_SAG) != 0;
    // Coach's own points for the line to take. Empty without a sheet, and then no trail.
    v.ref       = &g_ref.points();
    v.has_susp  = g_have_susp;
    v.more_speed = PaceOn() && g_pace_hint.more_speed;
    if (GearOn() && g_gear_hint.level > 0) v.gear_dir = int(g_gear_hint.dir), v.gear_target = g_gear_hint.target;
    for (int i = 0; i < 2; ++i) {
        v.susp[i]     = g_susp[i];
        v.susp_max[i] = g_susp_deep[i];
    }
    coachhud::Build(v, g_frame);
}

// ---------------------------------------------------------------------------------------
// The line to take on the ground (coachline.h; design in DevHub notes/mxb-coach-ground-line-
// 2026-10-01.md). Read-only toward the game: we watch the matrices it hands to the fixed-function
// GL pipeline and, at swap time, draw one blue ribbon through the camera they describe. Nothing
// in the game's memory is written. Nothing is hooked until hud.ini asks for the line.
//
// Crash-safety rules, all of them here:
//   * every hook forwards to the original with the arguments unchanged
//   * the hooks only record on the thread that presents frames; any other thread is ignored
//   * nothing is drawn unless every hook went in, this frame produced a camera that passes
//     coachline::PickCamera, and the window's own framebuffer is the one bound
//   * the draw saves and restores what it touches: all attributes, both matrices, the shader
//     program, the active texture unit and the matrix mode the capture is tracking
//   * the render thread never waits for the plugin's lock: it skips the update instead
//   * Shutdown takes the hooks out before the DLL goes away

constexpr size_t kMaxCandidates = 8;

struct GlCapture {
    GLenum mode    = GL_MODELVIEW;
    DWORD  thread  = 0;      // the thread that presents frames
    bool   own     = false;  // our own calls, not the game's
    // A perspective projection, and everything multiplied onto it since, while the game is
    // still on GL_PROJECTION: the camera being assembled.
    bool                              building = false;
    coachline::Candidate              cur;
    std::vector<coachline::Candidate> cands;  // this frame's
    unsigned n_frustum = 0, n_cands = 0, n_frames = 0, n_picked = 0, n_drawn = 0, n_draw_errors = 0;
};
GlCapture             g_gl;
float                    g_snap[3] = {NAN, NAN, NAN};  // the bike, as of the last swap
coachline::ModelviewPick g_mv_best;  // this frame's best modelview camera
coachline::AxesLock      g_mv_lock;
unsigned                 g_mv_loads = 0;
coachline::CameraFollow  g_follow;
coachline::ViewRef       g_view_ref;      // where the drawing camera held the bike: a relock must agree
ULONGLONG                g_view_miss = 0; // since when no load has agreed with it, while riding
// The correction from the game's own depth under the line (coachline::DepthSnap). Render thread
// only; a new event or stint asks for a reset through the flag.
coachline::DepthSnap     g_dsnap;
// v0.43.5: corrections by place on the line, only while the depth check agrees (coachline.h).
coachline::LineSnap      g_lsnap;
coachline::SnapCheck     g_scheck;
bool                     g_snap_on   = false;
float                    g_ground_s   = NAN; // the rider's ground estimate, rate-limited
ULONGLONG                g_dsnap_ms = 0;
unsigned                 g_dsnap_reads = 0, g_dsnap_used = 0;
unsigned                 g_prev_picked = 0, g_prev_drawn = 0;  // for the 10 s line's deltas
ULONGLONG                g_frame_ms = 0;  // this frame's time, set at each swap
// The last camera drawn through, to carry a frame without one (CameraFollow's stale window).
struct LastCam {
    bool            ok = false;
    coachline::Mat4 proj, view, model;
    int             axes = 0;
} g_last_cam;
// Why a frame with a camera drew nothing, counted and logged every 10 s.
enum Why { WHY_NO_VERTS, WHY_FBO, WHY_CORE, WHY_GL_ERROR, WHY_STALE, WHY_HOLD, WHY_DOWN, WHY_EYE, WHY_VIEW, WHY_COUNT };
const char* const kWhyName[WHY_COUNT] = {"no_ribbon",    "offscreen_fbo", "core_profile", "gl_error",
                                         "stale_redraw", "held_no_draw",  "rider_down",   "camera_off_bike",
                                         "camera_not_where_it_was"};
coachline::EyeWatch g_eye;
unsigned            g_seen_gen = 0;
bool                g_line_on_ground = false;  // the line is on the track's own ground: no snap
unsigned g_why[WHY_COUNT] = {};
ULONGLONG                g_cam_since = 0;  // when a camera was last found (or the search began)
int                      g_source    = 0;  // 0 none, 1 uniform, 2 modelview, 3 fallback
coachline::Ribbon     g_ribbon;
coachline::HeightBias g_height;
bool                  g_gl_tried = false, g_gl_ok = false;
ULONGLONG             g_gl_logged = 0, g_diag_ms = 0;
int                   g_diag_left = 30;  // once a second, for the first 30 s riding with the line on
// What the render thread draws, copied out under the lock so the draw itself never holds it.
std::vector<coachline::Vert> g_draw_verts;
std::vector<coachline::Vert> g_draw_marks;  // the pace hints' chevrons and gate, as triangles

using glMatrixMode_t    = void(WINAPI*)(GLenum);
using glLoadMatrixf_t   = void(WINAPI*)(const GLfloat*);
using glLoadMatrixd_t   = void(WINAPI*)(const GLdouble*);
using glMultMatrixf_t   = void(WINAPI*)(const GLfloat*);
using glMultMatrixd_t   = void(WINAPI*)(const GLdouble*);
using glLoadIdentity_t  = void(WINAPI*)();
using glFrustum_t       = void(WINAPI*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
using glOrtho_t         = void(WINAPI*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
using glTranslatef_t    = void(WINAPI*)(GLfloat, GLfloat, GLfloat);
using glTranslated_t    = void(WINAPI*)(GLdouble, GLdouble, GLdouble);
using glRotatef_t       = void(WINAPI*)(GLfloat, GLfloat, GLfloat, GLfloat);
using glRotated_t       = void(WINAPI*)(GLdouble, GLdouble, GLdouble, GLdouble);
using glScalef_t        = void(WINAPI*)(GLfloat, GLfloat, GLfloat);
using glScaled_t        = void(WINAPI*)(GLdouble, GLdouble, GLdouble);
using wglSwap_t         = BOOL(WINAPI*)(HDC);
using glUseProgram_t    = void(WINAPI*)(GLuint);
using glActiveTexture_t = void(WINAPI*)(GLenum);
glMatrixMode_t    g_orig_mode      = nullptr;
glLoadMatrixf_t   g_orig_loadf     = nullptr;
glLoadMatrixd_t   g_orig_loadd     = nullptr;
glMultMatrixf_t   g_orig_multf     = nullptr;
glMultMatrixd_t   g_orig_multd     = nullptr;
glLoadIdentity_t  g_orig_identity  = nullptr;
glFrustum_t       g_orig_frustum   = nullptr;
glOrtho_t         g_orig_ortho     = nullptr;
glTranslatef_t    g_orig_translatef = nullptr;
glTranslated_t    g_orig_translated = nullptr;
glRotatef_t       g_orig_rotatef   = nullptr;
glRotated_t       g_orig_rotated   = nullptr;
glScalef_t        g_orig_scalef    = nullptr;
glScaled_t        g_orig_scaled    = nullptr;
wglSwap_t         g_orig_swap      = nullptr;
glUseProgram_t    g_use_program    = nullptr;
glActiveTexture_t g_active_texture = nullptr;
bool              g_gl_ext_looked  = false;

// GL enums past GL 1.1, which the Windows SDK's gl.h stops at.
constexpr GLenum kGlCurrentProgram         = 0x8B8D;
constexpr GLenum kGlDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kGlActiveTexture          = 0x84E0;
constexpr GLenum kGlTexture0               = 0x84C0;

bool GlMine() { return !g_gl.own && g_gl.thread != 0 && GetCurrentThreadId() == g_gl.thread; }
bool GlProj() { return GlMine() && g_gl.mode == GL_PROJECTION; }

/// The camera being assembled is complete: the game has stopped multiplying onto it.
void GlFinish() {
    if (!g_gl.building) return;
    g_gl.building = false;
    if (g_gl.cur.ops == 0 || !coachline::ValidView(g_gl.cur.view) || g_gl.cands.size() >= kMaxCandidates) return;
    g_gl.cands.push_back(g_gl.cur);
    ++g_gl.n_cands;
}
void GlStart(const coachline::Mat4& proj) {
    GlFinish();
    g_gl.building = true;
    g_gl.cur      = coachline::Candidate{};
    g_gl.cur.proj = proj;
}
void GlProjectionLoaded(const float* m) {
    GlFinish();
    coachline::Mat4 a;
    std::memcpy(a.m, m, sizeof(a.m));
    if (coachline::ValidProjection(a)) GlStart(a);
}
void GlProjectionTimes(const coachline::Mat4& m) {
    if (!g_gl.building) return;
    g_gl.cur.view = coachline::Mul(g_gl.cur.view, m);
    ++g_gl.cur.ops;
}
void GlProjectionMultiplied(const float* m) {
    coachline::Mat4 a;
    std::memcpy(a.m, m, sizeof(a.m));
    GlProjectionTimes(a);
}
/// A modelview load: offered as the camera (the view is baked into it), and kept by its
/// translation for the diagnostic.
void GlModelLoaded(const float* m) {
    ++g_mv_loads;
    if (std::isfinite(g_snap[0])) {
        coachline::Mat4 a;
        std::memcpy(a.m, m, sizeof(a.m));
        // Following a camera already found: only one next to it, so a one-frame outlier is
        // never taken. Searching: the nearest to the bike.
        const float* prev = g_mv_lock.locked() >= 0 ? g_follow.anchor(g_frame_ms) : nullptr;
        if (prev) coachline::ModelviewFollow(g_mv_best, a, g_snap[0], g_snap[1], g_snap[2], g_mv_lock.locked(), prev, &g_view_ref);
        else coachline::ModelviewOffer(g_mv_best, a, g_snap[0], g_snap[1], g_snap[2], g_mv_lock.locked(), &g_view_ref);
    }
    if (g_gl.cands.empty() || g_diag_left <= 0) return;
    coachline::Candidate& c = g_gl.cands.back();
    if (c.n_mv >= 4) return;
    c.mv[c.n_mv][0] = m[12], c.mv[c.n_mv][1] = m[13], c.mv[c.n_mv][2] = m[14];
    ++c.n_mv;
}

void WINAPI hkMode(GLenum m) {
    if (GlMine()) {
        if (m != GL_PROJECTION) GlFinish();
        g_gl.mode = m;
    }
    g_orig_mode(m);
}
void WINAPI hkLoadf(const GLfloat* m) {
    if (m && GlMine()) {
        if (g_gl.mode == GL_PROJECTION) GlProjectionLoaded(m);
        else if (g_gl.mode == GL_MODELVIEW) GlModelLoaded(m);
    }
    g_orig_loadf(m);
}
void WINAPI hkLoadd(const GLdouble* m) {
    if (m && GlMine()) {
        float f[16];
        for (int i = 0; i < 16; ++i) f[i] = float(m[i]);
        if (g_gl.mode == GL_PROJECTION) GlProjectionLoaded(f);
        else if (g_gl.mode == GL_MODELVIEW) GlModelLoaded(f);
    }
    g_orig_loadd(m);
}
void WINAPI hkMultf(const GLfloat* m) {
    if (m && GlProj()) GlProjectionMultiplied(m);
    g_orig_multf(m);
}
void WINAPI hkMultd(const GLdouble* m) {
    if (m && GlProj()) {
        float f[16];
        for (int i = 0; i < 16; ++i) f[i] = float(m[i]);
        GlProjectionMultiplied(f);
    }
    g_orig_multd(m);
}
void WINAPI hkIdentity() {
    // Identity on the projection starts a new one.
    if (GlProj()) GlFinish();
    g_orig_identity();
}
void WINAPI hkFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (GlProj()) {
        // glFrustum multiplies onto what is there; the game loads identity first, and should it
        // ever stop, the camera-next-to-the-bike test in PickCamera refuses the result.
        GlFinish();
        coachline::Mat4 p;
        if (coachline::Frustum(l, r, b, t, n, f, p) && coachline::ValidProjection(p)) GlStart(p);
        ++g_gl.n_frustum;
    }
    g_orig_frustum(l, r, b, t, n, f);
}
void WINAPI hkOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (GlProj()) GlFinish();
    g_orig_ortho(l, r, b, t, n, f);
}
void WINAPI hkTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    if (GlProj()) GlProjectionTimes(coachline::Translate(x, y, z));
    g_orig_translatef(x, y, z);
}
void WINAPI hkTranslated(GLdouble x, GLdouble y, GLdouble z) {
    if (GlProj()) GlProjectionTimes(coachline::Translate(float(x), float(y), float(z)));
    g_orig_translated(x, y, z);
}
void WINAPI hkRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    if (GlProj()) GlProjectionTimes(coachline::Rotate(a, x, y, z));
    g_orig_rotatef(a, x, y, z);
}
void WINAPI hkRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) {
    if (GlProj()) GlProjectionTimes(coachline::Rotate(float(a), float(x), float(y), float(z)));
    g_orig_rotated(a, x, y, z);
}
void WINAPI hkScalef(GLfloat x, GLfloat y, GLfloat z) {
    if (GlProj()) GlProjectionTimes(coachline::Scale(x, y, z));
    g_orig_scalef(x, y, z);
}
void WINAPI hkScaled(GLdouble x, GLdouble y, GLdouble z) {
    if (GlProj()) GlProjectionTimes(coachline::Scale(float(x), float(y), float(z)));
    g_orig_scaled(x, y, z);
}

std::string F3(float a, float b, float c) {
    char s[96];
    std::snprintf(s, sizeof(s), "(%.2f, %.2f, %.2f)", double(a), double(b), double(c));
    return s;
}

// ---------------------------------------------------------------------------------------
// The camera from the shaders' matrix uniforms (coachline.h, "The camera from shader uniforms").
// The upload entry points are the driver's, reached through wglGetProcAddress with the game's
// context current, so they are hooked from the first swap rather than at install time. Each
// hook records and forwards unchanged.

using glUniformMatrix4fv_t        = void(WINAPI*)(GLint, GLsizei, GLboolean, const GLfloat*);
using glProgramUniformMatrix4fv_t = void(WINAPI*)(GLuint, GLint, GLsizei, GLboolean, const GLfloat*);
glUniformMatrix4fv_t        g_orig_umat4  = nullptr;
glProgramUniformMatrix4fv_t g_orig_pumat4 = nullptr;
glUseProgram_t              g_orig_use    = nullptr;
bool                        g_sh_tried = false, g_sh_ok = false;
GLuint                      g_program  = 0;  // the game's current program, as glUseProgram set it

// This frame's uploads, the last one per (program, location): a per-object matrix uploaded a
// thousand times a frame keeps one slot, so the camera is never crowded out.
constexpr size_t kMaxUploads = 512;
constexpr size_t kSlots      = 1024;  // open addressing over (program, location)
std::vector<coachline::Upload> g_ups;
std::vector<float>             g_ups_dist;  // per upload: how near its eye came to the bike
int32_t                        g_slot[kSlots];
unsigned                       g_ups_frame = 0, g_ups_total = 0, g_ups_dropped = 0;
coachline::KeyLock             g_ukey;
unsigned                       g_u_picked = 0;
// How the line meets the game's depth buffer: decided by looking at it (DepthProbe). Until then
// it is drawn over the scene rather than risk a test that hides all of it.
enum DepthMode { DEPTH_NONE = 0, DEPTH_LEQUAL = 1, DEPTH_GEQUAL = 2 };
DepthMode g_depth = DEPTH_LEQUAL;  // the game tests LEQUAL (0x203); a probe confirms or flips it
ULONGLONG g_probe_ms = 0;
GLint     g_profile = -1;

/// How near this matrix's eye comes to the bike, under either layout and any axes. Infinite for
/// a matrix that is no camera.
float EyeDistance(const coachline::Mat4& m) {
    float best = INFINITY;
    if (!std::isfinite(g_snap[0])) return best;
    for (int t = 0; t < 2; ++t) {
        float ex, ey, ez;
        bool  persp = false;
        if (!coachline::EyeOf(t ? coachline::Transposed(m) : m, ex, ey, ez, persp)) continue;
        for (int k = 0; k < coachline::kNumAxes; ++k) {
            float gx, gy, gz;
            coachline::AxesAt(k).apply(g_snap[0], g_snap[1], g_snap[2], gx, gy, gz);
            const float d2 = (ex - gx) * (ex - gx) + (ey - gy) * (ey - gy) + (ez - gz) * (ez - gz);
            if (d2 < best) best = d2;
        }
    }
    return best;
}

void ClearUploads() {
    g_ups.clear();
    g_ups_dist.clear();
    std::fill(std::begin(g_slot), std::end(g_slot), -1);
}

void RecordUpload(GLuint program, GLint location, GLboolean transpose, const GLfloat* v) {
    ++g_ups_frame;
    const uint64_t h   = (uint64_t(program) * 2654435761u) ^ (uint64_t(uint32_t(location)) * 40503u);
    size_t         at  = size_t(h % kSlots);
    for (size_t probe = 0; probe < 16; ++probe, at = (at + 1) % kSlots) {
        const int32_t i = g_slot[at];
        if (i < 0) {
            if (g_ups.size() >= kMaxUploads) {
                ++g_ups_dropped;
                return;
            }
            g_slot[at] = int32_t(g_ups.size());
            g_ups.push_back({program, location, {}});
            g_ups_dist.push_back(INFINITY);
            break;
        }
        if (g_ups[size_t(i)].program == program && g_ups[size_t(i)].location == location) break;
        if (probe == 15) {
            ++g_ups_dropped;
            return;
        }
    }
    // Stored the way GL will use it: transpose=GL_TRUE means the caller gave rows. Of the many
    // uploads one location takes in a frame (a matrix per object), the one whose eye comes
    // nearest the bike is kept: that is the camera's, if any is.
    coachline::Mat4 m;
    std::memcpy(m.m, v, sizeof(m.m));
    if (transpose) m = coachline::Transposed(m);
    const size_t i = size_t(g_slot[at]);
    const float  d = EyeDistance(m);
    if (d < g_ups_dist[i] || !std::isfinite(g_ups_dist[i])) {
        g_ups[i].m    = m;
        g_ups_dist[i] = d;
    }
}

void WINAPI hkUseProgram(GLuint p) {
    if (GlMine()) g_program = p;
    g_orig_use(p);
}
void WINAPI hkUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (v && count >= 1 && loc >= 0 && GlMine()) RecordUpload(g_program, loc, transpose, v);
    g_orig_umat4(loc, count, transpose, v);
}
void WINAPI hkProgramUniformMatrix4fv(GLuint p, GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (v && count >= 1 && loc >= 0 && GlMine()) RecordUpload(p, loc, transpose, v);
    g_orig_pumat4(p, loc, count, transpose, v);
}

// Entry points watched only to count them (and the transposed loads, which also feed the
// modelview capture): the log then says how this game's renderer passes its matrices, whatever
// it turns out to be. All optional; all forward unchanged.
enum Ep {
    EP_ENV4FV, EP_ENV4DV, EP_ENV4F, EP_LOCAL4FV, EP_LOCAL4DV, EP_LOCAL4F, EP_ENVS4FV, EP_UNIFORM4FV,
    EP_LOADT_F, EP_LOADT_D, EP_GETF_MV, EP_GETD_MV, EP_COUNT
};
const char* const kEpName[EP_COUNT] = {"glProgramEnvParameter4fvARB",   "glProgramEnvParameter4dvARB",
                                       "glProgramEnvParameter4fARB",    "glProgramLocalParameter4fvARB",
                                       "glProgramLocalParameter4dvARB", "glProgramLocalParameter4fARB",
                                       "glProgramEnvParameters4fvEXT",  "glUniform4fv",
                                       "glLoadTransposeMatrixf",        "glLoadTransposeMatrixd",
                                       "glGetFloatv(MODELVIEW)",        "glGetDoublev(MODELVIEW)"};
unsigned g_ep[EP_COUNT] = {};

using pv4fv_t   = void(WINAPI*)(GLenum, GLuint, const GLfloat*);
using pv4dv_t   = void(WINAPI*)(GLenum, GLuint, const GLdouble*);
using pv4f_t    = void(WINAPI*)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
using pvs4fv_t  = void(WINAPI*)(GLenum, GLuint, GLsizei, const GLfloat*);
using u4fv_t    = void(WINAPI*)(GLint, GLsizei, const GLfloat*);
using getf_t    = void(WINAPI*)(GLenum, GLfloat*);
using getd_t    = void(WINAPI*)(GLenum, GLdouble*);
pv4fv_t  g_o_env4fv = nullptr, g_o_local4fv = nullptr;
pv4dv_t  g_o_env4dv = nullptr, g_o_local4dv = nullptr;
pv4f_t   g_o_env4f = nullptr, g_o_local4f = nullptr;
pvs4fv_t g_o_envs4fv = nullptr;
u4fv_t   g_o_u4fv = nullptr;
glLoadMatrixf_t g_o_loadtf = nullptr;
glLoadMatrixd_t g_o_loadtd = nullptr;
getf_t   g_o_getf = nullptr;
getd_t   g_o_getd = nullptr;

void Count(Ep e) {
    if (GlMine()) ++g_ep[e];
}
void WINAPI hkEnv4fv(GLenum t, GLuint i, const GLfloat* v) { Count(EP_ENV4FV); g_o_env4fv(t, i, v); }
void WINAPI hkEnv4dv(GLenum t, GLuint i, const GLdouble* v) { Count(EP_ENV4DV); g_o_env4dv(t, i, v); }
void WINAPI hkEnv4f(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    Count(EP_ENV4F);
    g_o_env4f(t, i, x, y, z, w);
}
void WINAPI hkLocal4fv(GLenum t, GLuint i, const GLfloat* v) { Count(EP_LOCAL4FV); g_o_local4fv(t, i, v); }
void WINAPI hkLocal4dv(GLenum t, GLuint i, const GLdouble* v) { Count(EP_LOCAL4DV); g_o_local4dv(t, i, v); }
void WINAPI hkLocal4f(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    Count(EP_LOCAL4F);
    g_o_local4f(t, i, x, y, z, w);
}
void WINAPI hkEnvs4fv(GLenum t, GLuint i, GLsizei n, const GLfloat* v) { Count(EP_ENVS4FV); g_o_envs4fv(t, i, n, v); }
void WINAPI hkUniform4fv(GLint l, GLsizei n, const GLfloat* v) { Count(EP_UNIFORM4FV); g_o_u4fv(l, n, v); }
void WINAPI hkLoadTf(const GLfloat* m) {
    if (m && GlMine()) {
        ++g_ep[EP_LOADT_F];
        const coachline::Mat4 t = coachline::Transposed([&] { coachline::Mat4 a; std::memcpy(a.m, m, sizeof(a.m)); return a; }());
        if (g_gl.mode == GL_PROJECTION) GlProjectionLoaded(t.m);
        else if (g_gl.mode == GL_MODELVIEW) GlModelLoaded(t.m);
    }
    g_o_loadtf(m);
}
void WINAPI hkLoadTd(const GLdouble* m) {
    if (m && GlMine()) {
        ++g_ep[EP_LOADT_D];
        float f[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) f[c * 4 + r] = float(m[r * 4 + c]);
        if (g_gl.mode == GL_PROJECTION) GlProjectionLoaded(f);
        else if (g_gl.mode == GL_MODELVIEW) GlModelLoaded(f);
    }
    g_o_loadtd(m);
}
void WINAPI hkGetf(GLenum p, GLfloat* v) {
    if (p == 0x0BA6 /* GL_MODELVIEW_MATRIX */) Count(EP_GETF_MV);
    g_o_getf(p, v);
}
void WINAPI hkGetd(GLenum p, GLdouble* v) {
    if (p == 0x0BA6) Count(EP_GETD_MV);
    g_o_getd(p, v);
}

/// Once a second for the first 30 s, beside the uniform lines: the counts per entry point, the
/// fixed-function projection and the best modelview load of the frame, and the fallback's eye.
void LogSources(float aspect, const coachline::Candidate* wp) {
    std::string counts;
    for (int e = 0; e < EP_COUNT; ++e)
        if (g_ep[e]) counts += std::string(" ") + kEpName[e] + "=" + std::to_string(g_ep[e]);
    Log("ground/diag", "entry points:" + (counts.empty() ? std::string(" none of the extra ones called") : counts) +
                           " modelview loads " + std::to_string(g_mv_loads));
    if (wp) {
        const float fovy = wp->proj.m[5] > 0 ? 2.0f * std::atan(1.0f / wp->proj.m[5]) * 57.2957795f : 0.0f;
        Log("ground/diag", "  projection fovy " + std::to_string(fovy) + " aspect " +
                               std::to_string(coachline::Aspect(wp->proj)) + " window " + std::to_string(aspect));
    } else {
        Log("ground/diag", "  no fixed-function projection matches the window this frame");
    }
    if (g_mv_best.ok) {
        float ex, ey, ez;
        coachline::CameraPos(g_mv_best.view, ex, ey, ez);
        Log("ground/diag", "  modelview camera eye " + F3(ex, ey, ez) + " axes " +
                               coachline::AxesName(coachline::AxesAt(g_mv_best.axes)) + " dist " +
                               std::to_string(g_mv_best.dist) + " lock " +
                               (g_mv_lock.locked() >= 0 ? "yes" : "no"));
    } else {
        Log("ground/diag", "  no modelview load this frame puts a camera 0.3-25 m from the bike");
    }
    coachline::Mat4 ob;
    if (coachline::OnboardView(g_rider.x, g_rider_y, g_rider.y, g_rider_v[0], g_rider_v[1], g_rider_v[2], ob)) {
        float ex, ey, ez;
        coachline::CameraPos(ob, ex, ey, ez);
        Log("ground/diag", "  fallback helmet eye (GL) " + F3(ex, ey, ez) + " velocity " +
                               F3(g_rider_v[0], g_rider_v[1], g_rider_v[2]));
    }
}

/// From the first swap, on the render thread with the game's context current. Same rule as the
/// fixed-function hooks: glUniformMatrix4fv must go in or none of them do.
void InstallShaderHooks() {
    if (g_sh_tried) return;
    g_sh_tried = true;
    std::fill(std::begin(g_slot), std::end(g_slot), -1);
    g_ups.reserve(kMaxUploads);
    glGetIntegerv(0x9126 /* GL_CONTEXT_PROFILE_MASK */, &g_profile);
    if (glGetError() != GL_NO_ERROR) g_profile = -1;
    struct H {
        const char* n;
        void*       d;
        void**      o;
        bool        required;
    };
    const H hs[] = {{"glUniformMatrix4fv", (void*)&hkUniformMatrix4fv, (void**)&g_orig_umat4, true},
                    {"glUseProgram", (void*)&hkUseProgram, (void**)&g_orig_use, true},
                    {"glProgramUniformMatrix4fv", (void*)&hkProgramUniformMatrix4fv, (void**)&g_orig_pumat4, false},
                    {"glProgramEnvParameter4fvARB", (void*)&hkEnv4fv, (void**)&g_o_env4fv, false},
                    {"glProgramEnvParameter4dvARB", (void*)&hkEnv4dv, (void**)&g_o_env4dv, false},
                    {"glProgramEnvParameter4fARB", (void*)&hkEnv4f, (void**)&g_o_env4f, false},
                    {"glProgramLocalParameter4fvARB", (void*)&hkLocal4fv, (void**)&g_o_local4fv, false},
                    {"glProgramLocalParameter4dvARB", (void*)&hkLocal4dv, (void**)&g_o_local4dv, false},
                    {"glProgramLocalParameter4fARB", (void*)&hkLocal4f, (void**)&g_o_local4f, false},
                    {"glProgramEnvParameters4fvEXT", (void*)&hkEnvs4fv, (void**)&g_o_envs4fv, false},
                    {"glUniform4fv", (void*)&hkUniform4fv, (void**)&g_o_u4fv, false},
                    {"glLoadTransposeMatrixf", (void*)&hkLoadTf, (void**)&g_o_loadtf, false},
                    {"glLoadTransposeMatrixd", (void*)&hkLoadTd, (void**)&g_o_loadtd, false},
                    {"glGetFloatv", (void*)&hkGetf, (void**)&g_o_getf, false},
                    {"glGetDoublev", (void*)&hkGetd, (void**)&g_o_getd, false}};
    const HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    std::vector<void*> made;
    std::string        names;
    for (const H& h : hs) {
        void* t = (void*)wglGetProcAddress(h.n);
        // wglGetProcAddress reports a missing entry as 0, 1, 2, 3 or -1 on some drivers, and
        // has nothing for what opengl32.dll exports itself (glGetFloatv and the like).
        if (reinterpret_cast<uintptr_t>(t) <= 3 || t == (void*)-1) t = nullptr;
        if (!t && gl32) t = (void*)GetProcAddress(gl32, h.n);
        if (t && std::find(made.begin(), made.end(), t) != made.end()) continue;  // an alias
        if (!t || MH_CreateHook(t, h.d, h.o) != MH_OK) {
            if (!h.required) continue;
            Log("ground", std::string("shader hook failed: ") + h.n + "; no ground line");
            for (void* m : made) MH_RemoveHook(m);
            return;
        }
        made.push_back(t);
        names += std::string(names.empty() ? "" : " ") + h.n;
    }
    for (void* m : made)
        if (MH_EnableHook(m) != MH_OK) {
            Log("ground", "shader hooks could not be enabled; no ground line");
            for (void* r : made) {
                MH_DisableHook(r);
                MH_RemoveHook(r);
            }
            return;
        }
    g_sh_ok       = true;
    g_use_program = g_orig_use;  // our own program switch must not be recorded as the game's
    Log("ground", "shader uniform hooks installed (read-only): " + names +
                      " profile mask " + std::to_string(g_profile));
}

/// The matrices to draw through for a picked upload: a projective one as it stands (it already
/// holds the projection), a rigid view with the projection the frame uploaded alongside it, or
/// failing that the fixed-function frustum. False when there is no projection to use.
bool DrawMatrices(const coachline::UniformPick& pk, float aspect, coachline::Mat4& proj, coachline::Mat4& view) {
    const coachline::Upload& u = g_ups[size_t(pk.index)];
    const coachline::Mat4    m = pk.transposed ? coachline::Transposed(u.m) : u.m;
    if (pk.perspective) {
        proj = m;
        view = coachline::Mat4{};
        return true;
    }
    view       = m;
    bool found = false;
    for (const coachline::Upload& o : g_ups)
        for (int t = 0; t < 2 && !found; ++t) {
            const coachline::Mat4 p = t ? coachline::Transposed(o.m) : o.m;
            if (coachline::ValidProjection(p) && std::fabs(coachline::Aspect(p) - aspect) < aspect * 0.08f) {
                proj  = p;
                found = true;
            }
        }
    for (size_t i = g_gl.cands.size(); i-- > 0 && !found;)
        if (std::fabs(coachline::Aspect(g_gl.cands[i].proj) - aspect) < aspect * 0.08f) {
            proj  = g_gl.cands[i].proj;
            found = true;
        }
    return found;
}

/// Once a second for the first 30 s: the bike, the upload count, and the eight uploads whose
/// eye comes nearest the bike under any layout and axes, each with its program, location, kind,
/// eye and distance. Under g_mu.
void LogUniforms(float aspect, const coachline::UniformPick& pk) {
    Log("ground/diag", "bike " + F3(g_rider.x, g_rider_y, g_rider.y) + " aspect " + std::to_string(aspect) +
                           " uploads/frame " + std::to_string(g_ups_frame) + " distinct " +
                           std::to_string(g_ups.size()) + " dropped " + std::to_string(g_ups_dropped) +
                           " ff cameras " + std::to_string(g_gl.cands.size()) + " picked " +
                           (pk.index >= 0 ? "p" + std::to_string(g_ups[size_t(pk.index)].program) + "/l" +
                                                std::to_string(g_ups[size_t(pk.index)].location) +
                                                (pk.transposed ? " T" : "") + " axes " +
                                                coachline::AxesName(coachline::AxesAt(pk.axes)) + " " +
                                                std::to_string(pk.dist) + " m"
                                          : std::string("none")) +
                           " lock " + (g_ukey.locked() ? "yes" : "no") + " depth " + std::to_string(int(g_depth)));
    struct Row {
        float d;
        size_t i;
        bool  t, persp;
        int   axes;
        float ex, ey, ez;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < g_ups.size(); ++i)
        for (int t = 0; t < 2; ++t) {
            const coachline::Mat4 m = t ? coachline::Transposed(g_ups[i].m) : g_ups[i].m;
            float ex, ey, ez;
            bool  persp = false;
            if (!coachline::EyeOf(m, ex, ey, ez, persp)) continue;
            Row r{1e30f, i, t != 0, persp, -1, ex, ey, ez};
            for (int k = 0; k < coachline::kNumAxes; ++k) {
                float gx, gy, gz;
                coachline::AxesAt(k).apply(g_rider.x, g_rider_y, g_rider.y, gx, gy, gz);
                const float d = std::sqrt((ex - gx) * (ex - gx) + (ey - gy) * (ey - gy) + (ez - gz) * (ez - gz));
                if (d < r.d) r.d = d, r.axes = k;
            }
            rows.push_back(r);
        }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    for (size_t k = 0; k < rows.size() && k < 8; ++k) {
        const Row& r = rows[k];
        const coachline::Upload& u = g_ups[r.i];
        char head[160];
        std::snprintf(head, sizeof(head), "  p%u/l%d%s %s eye ", u.program, u.location, r.t ? " T" : "",
                      r.persp ? "projective" : "rigid");
        Log("ground/diag", std::string(head) + F3(r.ex, r.ey, r.ez) + " dist " + std::to_string(r.d) + " axes " +
                               coachline::AxesName(coachline::AxesAt(r.axes)));
    }
    if (rows.empty()) Log("ground/diag", "  no upload this frame has a camera's shape");
}

// ---------------------------------------------------------------------------------------
// The depth reads, v0.45.4: synchronous, rare, and only while riding.
//
// v0.45.2 made them asynchronous (pixel-pack buffers, fences polled from the swap hook) and
// froze the game: the swap hook ran the reads and the polling on every frame the line had a
// camera, which includes the pause screen and the menus over the track (it only knew "practice,
// a telemetry sample once seen"), it read every snap point each time where v0.45.1 read one
// until the check agreed, and its fences were never flushed, so a poll could find one pending
// for as long as the driver kept it queued. Back to a plain glReadPixels, decided at once, through
// one coachline::ReadBudget: at most one probe a second, only while riding (coachline::Riding: a
// telemetry sample and an on-track HUD draw both moments ago). In a menu or a loading screen the
// swap hook does nothing but pass the frame on.

std::atomic<uint64_t> g_sample_ms{0};      // the last RunTelemetry sample
std::atomic<uint64_t> g_track_draw_ms{0};  // the last Draw with the rider on track (state 0)
coachline::ReadBudget g_read_budget;       // render thread only
unsigned              g_snap_gen = 0;      // bumped whenever the snap's state is started again

bool RidingNow(ULONGLONG now) { return coachline::Riding(now, g_sample_ms.load(), g_track_draw_ms.load()); }

constexpr GLenum kGlPixelPackBufferBinding = 0x88ED;
constexpr int    kReadMax                  = 1 + coachline::kSnapN;  // the check, then the snap points

enum ReadKind { READ_DEPTH_MODE, READ_SNAP };

/// One probe's reads and what is needed to make sense of them.
struct DepthRead {
    ReadKind kind = READ_SNAP;
    int      n    = 0;  // pixels read
    GLint    px[kReadMax] = {}, py[kReadMax] = {};
    float    sx[kReadMax] = {}, sy[kReadMax] = {};
    bool     ok[kReadMax] = {};  // read without a GL error
    float    d[kReadMax]  = {};
    GLint    game_func    = GL_LESS;  // READ_DEPTH_MODE: the game's depth func at the read
    // READ_SNAP: the camera and depth range it was read through, the bike, and the check.
    coachline::Mat4 proj, view;
    coachline::Axes ax;
    float           range[2]  = {0.0f, 1.0f};
    float           bike[3]   = {NAN, NAN, NAN};
    bool            has_check = false;  // a check was due (the bike moving)
    int             check     = -1;     // its pixel, or -1 when the point was off screen
    float           aim_x = NAN, aim_z = NAN;
    unsigned        gen = 0;  // g_snap_gen at the read

    int add(const GLint vp[4], float x, float y) {
        const int k = n++;
        sx[k] = x, sy[k] = y;
        px[k] = vp[0] + GLint(x * float(vp[2]));
        py[k] = vp[1] + GLint((1.0f - y) * float(vp[3]));
        d[k]  = 1.0f;
        return k;
    }
};

/// The depth-mode probe's reading (DepthProbe).
void ResolveDepthMode(const DepthRead& r) {
    if (r.n < 1 || !r.ok[0]) return;
    const float     d   = r.d[0];
    const DepthMode was = g_depth;
    // Depth 1.0 (or 0) under the line is the far plane or a cleared buffer, not the ground: no
    // reading. v0.43.4 took it as "the scene isn't in this buffer" and drew the line over
    // everything for a few seconds at a time.
    if (!(d > 1e-6f && d < 0.999999f)) return;
    const GLint game_func = r.game_func;
    g_depth = game_func == GL_GREATER || game_func == GL_GEQUAL ? DEPTH_GEQUAL : DEPTH_LEQUAL;
    if (g_depth != was) {
        std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
        if (lock.owns_lock())
            Log("ground", "depth at the line " + std::to_string(d) + ", game depth func 0x" +
                              [&] { char h[16]; std::snprintf(h, sizeof(h), "%X", unsigned(game_func)); return std::string(h); }() +
                              " -> " + (g_depth == DEPTH_NONE ? "drawn over the scene" : g_depth == DEPTH_LEQUAL ? "depth-tested" : "depth-tested (reversed)"));
    }
}

/// The snap's readings (SnapProbe): the check first, then, while it agrees, the corrections.
void ResolveSnap(const DepthRead& r) {
    if (r.gen != g_snap_gen || g_line_on_ground) return;
    auto good = [&](int k) { return r.ok[k] && r.d[k] > r.range[0] + 1e-6f && r.d[k] < r.range[1] - 1e-6f; };
    // The check: the ground 4 m ahead of the bike, read back, against the bike's own height.
    if (r.has_check) {
        float hx = NAN, hy = NAN, hz = NAN;
        if (r.check >= 0 && good(r.check))
            coachline::Unproject(r.proj, r.view, r.ax, r.sx[r.check], r.sy[r.check], r.d[r.check], hx, hy, hz,
                                 r.range[0], r.range[1]);
        g_scheck.add(r.bike[1], hx, hy, hz, r.aim_x, r.aim_z);
    }
    const bool on = g_scheck.on();
    if (on != g_snap_on) {
        g_snap_on = on;
        std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
        if (lock.owns_lock())
            Log("ground", on ? "snap on: the depth under the bike agrees with the bike (" + std::to_string(g_scheck.lift()) + " m over it)"
                             : "snap off: " + g_scheck.why());
    }
    if (!on) return;
    for (int k = 0; k < r.n; ++k) {
        if (k == r.check || !good(k)) continue;
        ++g_dsnap_reads;
        float hx, hy, hz, m;
        if (!coachline::Unproject(r.proj, r.view, r.ax, r.sx[k], r.sy[k], r.d[k], hx, hy, hz, r.range[0], r.range[1]))
            continue;
        const float dy = coachline::SnapReadingAt(g_draw_verts, g_lsnap, hx, hy, hz, r.bike[0], r.bike[2], m);
        if (std::isfinite(dy) && g_lsnap.feed(m, g_lsnap.at(m) + dy)) ++g_dsnap_used;
    }
}

/// Reads `r`'s pixels from the back buffer, synchronously, and decides them at once.
void ReadNow(DepthRead& r) {
    if (r.n > 0) {
        glReadBuffer(GL_BACK);
        while (glGetError() != GL_NO_ERROR) {
        }
        for (int k = 0; k < r.n; ++k) {
            glReadPixels(r.px[k], r.py[k], 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &r.d[k]);
            r.ok[k] = glGetError() == GL_NO_ERROR;
        }
    }
    if (r.kind == READ_DEPTH_MODE) ResolveDepthMode(r);
    else ResolveSnap(r);
}

/// Whether the game has a pixel-pack buffer bound: a read would land in it.
bool GamePackBound() {
    GLint pack = 0;
    glGetIntegerv(kGlPixelPackBufferBinding, &pack);
    return glGetError() == GL_NO_ERROR && pack != 0;
}

/// Looks at the game's depth buffer under the ribbon 10 m ahead, every two seconds at most: an
/// empty one (the scene was drawn elsewhere and copied in) means no depth test at all, a full
/// one is tested against the way the game tests (reversed or not). Inside the draw's
/// glPushAttrib, so the read buffer it may set is restored.
void DepthProbe(const coachline::Mat4& proj, const coachline::Mat4& view, const coachline::Axes& ax) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_probe_ms < 2000 || g_draw_verts.size() < 12) return;
    const coachline::Vert& a = g_draw_verts[10];
    const coachline::Vert& b = g_draw_verts[11];
    const coachline::Screen s = coachline::Project(proj, view, (a.x + b.x) * 0.5f, a.y, (a.z + b.z) * 0.5f, ax);
    if (!s.ok || s.x < 0 || s.x > 1 || s.y < 0 || s.y > 1) return;
    if (!g_read_budget.take(now, RidingNow(now))) return;
    g_probe_ms = now;
    if (GamePackBound()) return;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;
    DepthRead r;
    r.kind      = READ_DEPTH_MODE;
    r.game_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &r.game_func);
    r.add(vp, s.x, s.y);
    ReadNow(r);
}

/// Reads the game's depth under the ribbon at coachline::kSnapAt metres ahead and feeds the
/// correction (coachline::LineSnap) once the check agrees; until it does, only the check is read.
/// Inside the draw's glPushAttrib; skipped while the game has a pixel-pack buffer bound.
void SnapProbe(const coachline::Mat4& proj, const coachline::Mat4& view, const coachline::Axes& ax) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_dsnap_ms < coachline::kSnapEveryMs || g_draw_verts.size() < 8) return;
    if (!g_read_budget.take(now, RidingNow(now))) return;
    g_dsnap_ms = now;
    if (GamePackBound()) return;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;
    DepthRead r;
    r.kind = READ_SNAP;
    // The depth range the game left set: a split range (near and far passes) changes what a
    // depth value means, and v0.43.4 assumed 0..1.
    glGetFloatv(0x0B70 /* GL_DEPTH_RANGE */, r.range);
    if (glGetError() != GL_NO_ERROR || !(r.range[1] > r.range[0])) r.range[0] = 0.0f, r.range[1] = 1.0f;
    if (g_line_total > 0 && std::fabs(g_lsnap.total() - g_line_total) > 0.5f) {
        g_lsnap.reset(g_line_total);
        ++g_snap_gen;
    }
    r.proj = proj, r.view = view, r.ax = ax, r.gen = g_snap_gen;
    for (int k = 0; k < 3; ++k) r.bike[k] = g_snap[k];
    auto on_screen = [](const coachline::Screen& sc) { return sc.ok && sc.x > 0 && sc.x < 1 && sc.y > 0 && sc.y < 1; };
    // The check: the ground 4 m ahead of the bike, read back against the bike's own height.
    const float vx = g_rider_v[0], vz = g_rider_v[2], vh = std::hypot(vx, vz);
    if (vh > 2.0f) {
        const float lift = std::isfinite(g_scheck.lift()) ? g_scheck.lift() : coachline::kOriginGuess;
        r.has_check = true;
        r.aim_x     = g_snap[0] + vx / vh * 4.0f, r.aim_z = g_snap[2] + vz / vh * 4.0f;
        const coachline::Screen sc = coachline::Project(proj, view, r.aim_x, g_snap[1] - lift, r.aim_z, ax);
        if (on_screen(sc)) r.check = r.add(vp, sc.x, sc.y);
    }
    // The corrections' points: read only while the check agrees, as up to v0.45.1.
    if (g_scheck.on()) {
        const std::vector<coachline::Vert>& v = g_draw_verts;
        for (int k = 0; k < coachline::kSnapN; ++k) {
            size_t row = 0;
            float  bd  = INFINITY;
            for (size_t i = 0; i + 1 < v.size(); i += 2)
                if (std::fabs(v[i].s - coachline::kSnapAt[k]) < bd) bd = std::fabs(v[i].s - coachline::kSnapAt[k]), row = i;
            if (bd > 2.0f) continue;
            const float cx = (v[row].x + v[row + 1].x) * 0.5f, cz = (v[row].z + v[row + 1].z) * 0.5f;
            const float cy = (v[row].y + v[row + 1].y) * 0.5f + g_lsnap.at(v[row].m);
            const coachline::Screen sc = coachline::Project(proj, view, cx, cy, cz, ax);
            if (!sc.ok || sc.x < 0 || sc.x > 1 || sc.y < 0 || sc.y > 1) continue;
            r.add(vp, sc.x, sc.y);
        }
    }
    ReadNow(r);
}

/// The ribbon through the game's camera, in the window's framebuffer. On the render thread.
void DrawGroundLine(const coachline::Mat4& proj, const coachline::Mat4& view, const coachline::Mat4& model,
                    const coachline::Axes& ax, bool depth_ok) {
    const std::vector<coachline::Vert>& v = g_draw_verts;
    if (v.size() < 4) return;
    if (!g_gl_ext_looked) {
        g_gl_ext_looked  = true;
        if (!g_use_program) g_use_program = reinterpret_cast<glUseProgram_t>(wglGetProcAddress("glUseProgram"));
        g_active_texture = reinterpret_cast<glActiveTexture_t>(wglGetProcAddress("glActiveTexture"));
    }
    // A core-profile context has no fixed-function pipeline to draw this way.
    if (g_profile > 0 && (g_profile & 1) && !(g_profile & 2)) {
        ++g_why[WHY_CORE];
        return;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    // Drawing into an offscreen target would paint the line into a reflection or a post-process
    // input. Only the window itself. (A GL too old to know the query errors, and has no FBOs.)
    GLint fbo = 0;
    glGetIntegerv(kGlDrawFramebufferBinding, &fbo);
    if (glGetError() == GL_NO_ERROR && fbo != 0) {
        ++g_why[WHY_FBO];
        return;
    }
    GLint program = 0, active = GLint(kGlTexture0);
    if (g_use_program) glGetIntegerv(kGlCurrentProgram, &program);
    if (g_active_texture) glGetIntegerv(kGlActiveTexture, &active);
    glGetError();

    const GLenum mode = g_gl.mode;
    g_gl.own          = true;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (g_use_program && program) g_use_program(0);
    if (depth_ok) DepthProbe(proj, coachline::Mul(view, model), ax);
    if (g_dsnap_reset) {
        g_dsnap.reset();
        g_lsnap.reset(g_line_total);
        g_scheck.reset();
        g_snap_on     = false;
        g_dsnap_reset = false;
        ++g_snap_gen;  // the snap started again
    }
    // Without the track's own ground, snap to what the game drew, a few times a second.
    if (depth_ok && g_depth != DEPTH_NONE && !g_line_on_ground) SnapProbe(proj, coachline::Mul(view, model), ax);
    g_orig_mode(GL_PROJECTION);
    glPushMatrix();
    g_orig_loadf(proj.m);
    g_orig_multf(view.m);
    g_orig_mode(GL_MODELVIEW);
    glPushMatrix();
    g_orig_loadf(model.m);
    // Texturing off on the units a game is likely to leave on; GL_ALL_ATTRIB_BITS saved them.
    for (int u = 0; u < (g_active_texture ? 4 : 1); ++u) {
        if (g_active_texture) g_active_texture(kGlTexture0 + GLenum(u));
        glDisable(GL_TEXTURE_2D);
    }
    if (g_active_texture) g_active_texture(kGlTexture0);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);  // it never hides anything
    if (g_depth == DEPTH_NONE || !depth_ok) {
        glDisable(GL_DEPTH_TEST);
    } else {
        // Tested the way the game tests, pulled toward the camera by a z-fighting margin.
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(g_depth == DEPTH_GEQUAL ? GL_GEQUAL : GL_LEQUAL);
        glEnable(GL_POLYGON_OFFSET_FILL);
        // On the track's own ground the ribbon sits 2 cm up and needs only a nudge to win the
        // depth test: painted on the dirt, not floating over it. On centreline heights, more.
        // A slope factor and a constant big enough to beat the terrain mesh's own small
        // departures from the track's height file (a few depth units is ~1 cm at 30 m with this
        // near plane, and the mesh is off by more): the line blinked where they fought.
        glPolygonOffset(g_depth == DEPTH_GEQUAL ? 2.0f : -2.0f, g_depth == DEPTH_GEQUAL ? 30.0f : -30.0f);
    }
    glShadeModel(GL_SMOOTH);  // a colour per vertex, blended across each quad: fades, not seams
    glBegin(GL_TRIANGLE_STRIP);
    // Nothing that isn't a number reaches GL: a vertex with one takes the last good height on
    // its side of the ribbon, or is left out with the rest of the strip from there.
    float last_y[2] = {NAN, NAN};
    for (size_t i = 0; i < v.size(); ++i) {
        const coachline::Vert& p = v[i];
        float y = g_line_on_ground || !g_snap_on ? p.y : p.y + g_lsnap.at(p.m);
        if (!std::isfinite(y)) y = last_y[i & 1];
        if (!std::isfinite(y) || !std::isfinite(p.x) || !std::isfinite(p.z)) break;
        last_y[i & 1] = y;
        // Faded by how far ahead of the rider this row is *now*: `p.s` is from the last rebuild and
        // up to two metres stale, which would step the near fade. Lap metres don't go stale.
        const float ahead = std::isfinite(g_rider_m) ? coachline::WrapAhead(p.m, g_rider_m, g_arc.total) : p.s;
        glColor4f(p.rgba[0], p.rgba[1], p.rgba[2],
                  p.rgba[3] * coachline::Fade(ahead) * coachline::NearFade(ahead, coachline::Look().near_fade));
        float gx, gy, gz;
        ax.apply(p.x, y, p.z, gx, gy, gz);
        glVertex3f(gx, gy, gz);
    }
    glEnd();
    // The pace hints over it (coachpace::Marks): on the same ground, faded the same way.
    // A triangle with anything that isn't a number in it is left out whole.
    if (!g_draw_marks.empty()) {
        glBegin(GL_TRIANGLES);
        for (size_t t = 0; t + 3 <= g_draw_marks.size(); t += 3) {
            float g[3][3];
            bool  ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const coachline::Vert& p = g_draw_marks[t + size_t(k)];
                const float            y = g_line_on_ground ? p.y : p.y + g_dsnap.at(p.s);
                ok = std::isfinite(y) && std::isfinite(p.x) && std::isfinite(p.z);
                if (ok) ax.apply(p.x, y, p.z, g[k][0], g[k][1], g[k][2]);
            }
            if (!ok) continue;
            for (int k = 0; k < 3; ++k) {
                const coachline::Vert& p = g_draw_marks[t + size_t(k)];
                glColor4f(p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]);
                glVertex3f(g[k][0], g[k][1], g[k][2]);
            }
        }
        glEnd();
    }
    const GLenum drew = glGetError();
    g_orig_mode(GL_MODELVIEW);
    glPopMatrix();
    g_orig_mode(GL_PROJECTION);
    glPopMatrix();
    if (g_use_program && program) g_use_program(GLuint(program));
    if (g_active_texture) g_active_texture(GLenum(active));
    glPopAttrib();  // restores the matrix mode, among the rest
    g_gl.mode = mode;
    g_gl.own  = false;
    while (glGetError() != GL_NO_ERROR) {
    }  // nothing of ours may surface as the game's error
    if (drew == GL_NO_ERROR) ++g_gl.n_drawn;
    else {
        ++g_gl.n_draw_errors;
        ++g_why[WHY_GL_ERROR];
    }
}

/// The fixed-function projection the game set up for the window: the last frustum this frame
/// whose aspect matches it, with what was multiplied onto it (the game's constant z flip).
bool WindowProjection(float aspect, coachline::Candidate& out) {
    for (size_t i = g_gl.cands.size(); i-- > 0;)
        if (aspect > 0 && std::fabs(coachline::Aspect(g_gl.cands[i].proj) - aspect) < aspect * 0.08f) {
            out = g_gl.cands[i];
            return true;
        }
    return false;
}

const char* SourceName(int s) {
    switch (s) {
        case 1: return "uniform";
        case 2: return "modelview";
        case 3: return "fallback-onboard";
        default: return "searching";
    }
}

// ---------------------------------------------------------------------------------------
// Jump calls on the line (coachjump.h): the calls are made once a sheet, from its terrain, its
// air and its speed, and drawn as extra quads (coachmark.h) after the ribbon, on the ribbon as
// built. Kept apart from the ribbon's own draw so the two can change independently.
std::vector<coachjump::Call>  g_jump_calls;
uint32_t                      g_jump_ver = 0;
std::vector<coachmark::Quad>  g_marks;  // this frame's, built under the lock, drawn outside it
int                           g_ai    = -1;  // the segment the rider is on this frame, and how far along it
float                         g_afrac = 0;

/// This frame's marks for the calls ahead of the rider. Under g_mu, after the ribbon is built.
void BuildMarks() {
    g_marks.clear();
    // Rebuilt for a new sheet, and once the track's own grid is there to read the ground from.
    const bool     grid = g_extras.grid != nullptr;
    const uint32_t ver  = g_extras.version * 2u + (grid ? 1u : 0u);
    if (g_jump_ver != ver) {
        g_jump_ver           = ver;
        coachjump::Line line = coachjump::MakeLine(g_hud_sheet);
        // A sheet without TRRN still gets its faces from the track's grid (`<track>.ground`).
        if (grid && g_hud_sheet.terrain.empty())
            for (size_t i = 0; i < line.size(); ++i) {
                float y;
                if (g_extras.grid->at(g_hud_sheet.ref[i].x, g_hud_sheet.ref[i].z, y)) line.ground[i] = y;
            }
        g_jump_calls                  = coachjump::Calls(line);
        if (!g_hud_sheet.ref.empty()) Log("jumps", coachjump::Summary(line, g_jump_calls));
    }
    if (!g_hud_set.jumps || g_jump_calls.empty() || g_draw_verts.size() < 4) return;
    if (g_ai < 0 || !std::isfinite(g_rider_m)) return;
    // Every mark is placed by its metre on Coach's line, not by its distance from the rider.
    coachjump::Frame fr;
    fr.rider_m = g_rider_m;
    fr.total   = g_arc.total;
        coachjump::Marks(g_marks, g_jump_calls, coachjump::AheadOf(g_hud_sheet.ref, g_ai, g_afrac, coachline::kAhead + 10.0f),
                     g_draw_verts, true, &fr);
}

/// The marks through the same camera as the ribbon, with the same ground correction. On the
/// render thread, right after DrawGroundLine, with the game's state saved and put back the same way.
void DrawMarks(const coachline::Mat4& proj, const coachline::Mat4& view, const coachline::Mat4& model,
               const coachline::Axes& ax, bool depth_ok) {
    if (g_marks.empty()) return;
    while (glGetError() != GL_NO_ERROR) {
    }
    GLint program = 0, active = GLint(kGlTexture0);
    if (g_use_program) glGetIntegerv(kGlCurrentProgram, &program);
    if (g_active_texture) glGetIntegerv(kGlActiveTexture, &active);
    glGetError();
    const GLenum mode = g_gl.mode;
    g_gl.own          = true;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (g_use_program && program) g_use_program(0);
    g_orig_mode(GL_PROJECTION);
    glPushMatrix();
    g_orig_loadf(proj.m);
    g_orig_multf(view.m);
    g_orig_mode(GL_MODELVIEW);
    glPushMatrix();
    g_orig_loadf(model.m);
    for (int u = 0; u < (g_active_texture ? 4 : 1); ++u) {
        if (g_active_texture) g_active_texture(kGlTexture0 + GLenum(u));
        glDisable(GL_TEXTURE_2D);
    }
    if (g_active_texture) g_active_texture(kGlTexture0);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    if (g_depth == DEPTH_NONE || !depth_ok) {
        glDisable(GL_DEPTH_TEST);
    } else {
        // Tested like the ribbon, pulled a little further so a bar lying on it wins.
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(g_depth == DEPTH_GEQUAL ? GL_GEQUAL : GL_LEQUAL);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(g_depth == DEPTH_GEQUAL ? 3.0f : -3.0f, g_depth == DEPTH_GEQUAL ? 40.0f : -40.0f);
    }
    const bool on_ground = g_line_on_ground;  // as the ribbon: no snap on the track's own ground
    glBegin(GL_QUADS);
    for (const coachmark::Quad& q : g_marks) {
        // As the ribbon: a mark carries the lap metre it lies at, and takes the correction held
        // for that place on the line, which does not slide as the rider moves.
        const float dy = on_ground || (std::isfinite(q.m) && !g_snap_on) ? 0.0f
                         : std::isfinite(q.m)                           ? g_lsnap.at(q.m)
                                                                        : g_dsnap.at(q.s);
        if (!std::isfinite(dy)) continue;
        glColor4f(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3]);
        for (int c = 0; c < 4; ++c) {
            float gx, gy, gz;
            ax.apply(q.p[c][0], q.p[c][1] + dy, q.p[c][2], gx, gy, gz);
            glVertex3f(gx, gy, gz);
        }
    }
    glEnd();
    g_orig_mode(GL_MODELVIEW);
    glPopMatrix();
    g_orig_mode(GL_PROJECTION);
    glPopMatrix();
    if (g_use_program && program) g_use_program(GLuint(program));
    if (g_active_texture) g_active_texture(GLenum(active));
    glPopAttrib();
    g_gl.mode = mode;
    g_gl.own  = false;
    while (glGetError() != GL_NO_ERROR) {
    }
}

BOOL WINAPI hkSwap(HDC hdc) {
    try {
        if (!g_gl.thread) g_gl.thread = GetCurrentThreadId();
        if (GetCurrentThreadId() == g_gl.thread && !g_gl.own) {
            GlFinish();
            ++g_gl.n_frames;
            if (g_gl_ok && !g_sh_tried) {
                std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
                if (lock.owns_lock()) InstallShaderHooks();
            }
            coachline::Mat4 proj, view, model;
            coachline::Axes ax;
            bool            draw = false, depth_ok = true;
            {
                std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
                const ULONGLONG              now = GetTickCount64();
                g_frame_ms                         = now;
                if (lock.owns_lock() && g_have_sample) {
                    g_snap[0] = g_rider.x, g_snap[1] = g_rider_y, g_snap[2] = g_rider.y;
                }
                // Only while riding: in a menu, the pause screen or a loading screen nothing is
                // built, read or drawn (coachline::Riding).
                if (lock.owns_lock() && g_gl_ok && g_hud_set.enabled && g_hud_set.ground && g_practice &&
                    g_have_sample && RidingNow(now)) {
                    if (!g_cam_since) g_cam_since = now;
                    RECT       rc{};
                    float      aspect = 0;
                    const HWND wnd    = WindowFromDC(hdc);
                    if (wnd && GetClientRect(wnd, &rc) && rc.bottom > rc.top)
                        aspect = float(rc.right - rc.left) / float(rc.bottom - rc.top);
                    coachline::Candidate wp;
                    const bool           have_wp = WindowProjection(aspect, wp);

                    // 1. A camera matrix handed to the shaders.
                    const coachline::UniformPick pk =
                        g_sh_ok ? coachline::PickUniform(g_ups, g_rider.x, g_rider_y, g_rider.y, g_ukey.locked())
                                : coachline::UniformPick{};
                    if (g_ukey.vote(pk.key)) {
                        const coachline::Upload& u = g_ups[size_t(pk.index)];
                        Log("ground", "camera locked: program " + std::to_string(u.program) + " location " +
                                          std::to_string(u.location) + (pk.transposed ? " transposed" : "") +
                                          (pk.perspective ? " view-projection" : " view") + ", GL = telemetry " +
                                          coachline::AxesName(coachline::AxesAt(pk.axes)) + ", eye " +
                                          std::to_string(pk.dist) + " m from the bike");
                    }
                    // 2. The view baked into the fixed-function modelview loads.
                    if (g_mv_lock.vote(g_mv_best.ok ? g_mv_best.axes : -1))
                        Log("ground", "camera locked: fixed-function modelview, GL = telemetry " +
                                          coachline::AxesName(coachline::AxesAt(g_mv_best.axes)) + ", eye " +
                                          std::to_string(g_mv_best.dist) + " m from the bike");

                    int source = 0;
                    if (pk.index >= 0 && g_ukey.locked() && DrawMatrices(pk, aspect, proj, view)) {
                        source = 1;
                        model  = coachline::Mat4{};
                        ax     = coachline::AxesAt(pk.axes);
                    } else if (g_mv_lock.locked() >= 0 && g_mv_best.ok && have_wp) {
                        source = 2;
                        proj   = wp.proj;
                        view   = wp.view;
                        model  = g_mv_best.view;
                        ax     = coachline::AxesAt(g_mv_best.axes);
                        float eye[3];
                        coachline::CameraPos(g_mv_best.view, eye[0], eye[1], eye[2]);
                        g_follow.hit(eye, now);
                    } else if (g_mv_lock.locked() >= 0 && g_follow.holding(now)) {
                        // A frame without the camera, inside the hold: still the modelview
                        // camera. Redrawn from the last one for a few frames so the line doesn't
                        // blink, then left out until the camera is back - never drawn from a
                        // different candidate in between.
                        source = 2;
                        if (g_follow.fresh(now) && g_last_cam.ok) {
                            proj = g_last_cam.proj, view = g_last_cam.view, model = g_last_cam.model;
                            ax   = coachline::AxesAt(g_last_cam.axes);
                            ++g_why[WHY_STALE];
                        } else {
                            ++g_why[WHY_HOLD];
                            source = -2;  // held, nothing drawn
                        }
                    } else if (now - g_cam_since > coachline::kFallbackMs &&
                               coachline::OnboardView(g_rider.x, g_rider_y, g_rider.y, g_rider_v[0], g_rider_v[1],
                                                      g_rider_v[2], model)) {
                        // 3. Nothing found for two seconds: the helmet view from the telemetry.
                        source = 3;
                        if (have_wp) {
                            proj = wp.proj;
                        } else {
                            const float f = 1.0f / std::tan(0.5f * 70.0f * 0.017453292f), n = 0.05f, fa = 1000.0f;
                            for (float& x : proj.m) x = 0;
                            proj.m[0] = f / (aspect > 0 ? aspect : 16.0f / 9.0f), proj.m[5] = f;
                            proj.m[10] = (fa + n) / (n - fa), proj.m[11] = -1, proj.m[14] = 2 * fa * n / (n - fa);
                        }
                        view     = coachline::Mat4{};
                        ax       = coachline::AxesAt(coachline::FallbackAxes());
                        depth_ok = false;
                    }
                    if (source > 0 && source != 3 && !(source == 2 && !g_mv_best.ok && pk.index < 0)) {
                        g_last_cam.ok = true, g_last_cam.proj = proj, g_last_cam.view = view, g_last_cam.model = model;
                        // Compared field by field: by name it built 96 strings a frame.
                        for (int k = 0; k < coachline::kNumAxes; ++k) {
                            const coachline::Axes c = coachline::AxesAt(k);
                            if (std::equal(c.src, c.src + 3, ax.src) && std::equal(c.sgn, c.sgn + 3, ax.sgn)) {
                                g_last_cam.axes = k;
                                break;
                            }
                        }
                    }
                    // The camera being followed must stay where it has been relative to the bike: the
                    // crash camera, a replay view or a wrong object drifts off, and is let go.
                    if (source == 2 && g_mv_best.ok && !g_eye.ok(g_mv_best.dist, now)) {
                        Log("ground", "camera moved off the bike (eye " + std::to_string(g_mv_best.dist) + " m, usually " +
                                          std::to_string(g_eye.usual()) + " m); finding it again");
                        g_follow.reset();
                        g_mv_lock.reset();
                        g_eye.reset();
                        g_last_cam.ok = false;
                        ++g_why[WHY_EYE];
                        source = 0;
                    }
                    // Down, getting up, or put back on the track: hidden, and everything built from
                    // the old position is started again - the ribbon (re-anchored at the bike's real
                    // position), the ground snap, the camera being followed. Never a stale camera.
                    if (g_rider_state.generation() != g_seen_gen) {
                        g_seen_gen = g_rider_state.generation();
                        g_follow.reset();
                        g_last_cam.ok = false;
                        g_ribbon.clear();
                        g_dsnap_reset = true;
                        g_eye.reset();
                        g_ground_s = NAN;
                    }
                    if (!g_rider_state.visible()) {
                        ++g_why[WHY_DOWN];
                        source = 0;
                    }
                    // Learn where this camera holds the bike while it draws, and check it each frame
                    // (the loads are filtered by it): after a crash or a reset a relock must be the
                    // same camera, not whichever load has its eye nearest the bike.
                    if (g_rider_state.visible() && g_mv_lock.locked() >= 0) {
                        if (g_mv_best.ok) {
                            g_view_miss = 0;
                            if (source == 2) {
                                const bool was = g_view_ref.known();
                                g_view_ref.learn(coachline::BikeInView(g_mv_best.view, coachline::AxesAt(g_mv_best.axes), g_snap[0],
                                                                       g_snap[1], g_snap[2]));
                                if (!was && g_view_ref.known())
                                    Log("ground", "camera view learnt: the bike is " + std::to_string(g_view_ref.dist()) +
                                                      " m from it; a relock has to hold the bike there");
                            }
                        } else if (g_view_ref.known() && g_mv_best.rejected > 0) {
                            ++g_why[WHY_VIEW];
                            if (!g_view_miss) g_view_miss = now;
                            else if (now - g_view_miss > coachline::kViewGiveUpMs) {
                                // Seconds of loads near the bike and none holding it as before: the
                                // rider changed camera. Start the search over.
                                Log("ground", "no camera held the bike where it was for " +
                                                  std::to_string(coachline::kViewGiveUpMs / 1000) + " s; searching freely");
                                g_view_ref.reset();
                                g_follow.reset();
                                g_last_cam.ok = false;
                                g_view_miss   = 0;
                            }
                        }
                    }
                    const bool held = source == -2;
                    if (held) source = 2;
                    if (source == 1 || source == 2) g_cam_since = now;
                    if (source != g_source) {
                        Log("ground", std::string("camera=") + SourceName(source));
                        g_source = source;
                    }
                    if (g_diag_left > 0 && now - g_diag_ms >= 1000) {
                        g_diag_ms = now;
                        --g_diag_left;
                        LogUniforms(aspect, pk);
                        LogSources(aspect, have_wp ? &wp : nullptr);
                    }
                    // The game's own ground first, then the track's grid file, each once it is known to
                    // line up (or until the check says). A change of ground remakes what was read off it.
                    g_extras.grid = LiveGroundOn() ? &g_live
                                    : g_grid.ready() && (!g_align.result().done || g_align.result().ok) ? &g_grid
                                                                                                         : nullptr;
                    if (g_extras.grid != g_ground_was) {
                        g_ground_was     = g_extras.grid;
                        g_jump_ver       = ~0u;
                        g_pace_grid_lips = false;
                    }
                    g_line_on_ground = g_extras.grid != nullptr || g_extras.terrain_k != 0;
                    if (source && !held) {
                        ++g_u_picked;
                        const float lapM = g_track.ready() ? g_track.length() : 0.0f;
                        const std::vector<coachline::Zone> zones =
                            g_cues.active() && lapM > 0 ? coachline::BrakeZones(g_cues.sheet(), lapM)
                                                        : std::vector<coachline::Zone>();
                        // From where the rider is, on their own ground (Anchor), not from the
                        // sheet's lap position, which drifts from the game's on a long or short lap.
                        coachline::Anchor at;
                        at.x      = g_rider.x;
                        at.z      = g_rider.y;
                        // The ground under the rider: the centreline there plus how far the bike
                        // has ridden above it lately (HeightBias, seconds long), less the bike's
                        // own height. Steady through bumps and jumps, where the bike's own height
                        // would bob; the depth snap then takes it onto what the game drew.
                        float centre = 0;
                        const float target = g_height.samples() > 0 && g_track.height_at_lap(g_pos, centre)
                                                 ? centre + g_height.gap() - coachline::kOriginGuess
                                                 : g_rider_y - coachline::kOriginGuess;
                        if (!std::isfinite(g_ground_s) || std::fabs(target - g_ground_s) > 5.0f) g_ground_s = target;
                        else g_ground_s += (std::max)(-0.02f, (std::min)(0.02f, target - g_ground_s));
                        at.ground = g_ground_s;
                        // With a pace hint showing, the line's colours carry it (coachpace::Recolour).
                        const bool pace = PaceOn() && g_pace_hint.kind != coachpace::NONE;
                        g_pace_ex.grid  = g_extras.grid;  // the same ground under both
                        g_ribbon.update(g_ref.points(), g_track, g_pos, g_height.offset(), zones,
                                        pace ? &g_pace_ex : &g_extras, &at);
                        g_draw_verts = g_ribbon.verts();
                        g_rider_m    = NAN;
                        g_ai         = coachline::AnchorPoint(g_hud_sheet.ref, g_rider.x, g_rider.y, g_pos, g_afrac);
                        if (g_ai >= 0 && size_t(g_ai) < g_arc.m.size() && g_arc.m.size() == g_hud_sheet.ref.size()) {
                            const size_t n = g_hud_sheet.ref.size(), a = size_t(g_ai), b = (a + 1) % n;
                            g_rider_m = g_arc.m[a] + g_afrac * std::hypot(g_hud_sheet.ref[b].x - g_hud_sheet.ref[a].x,
                                                                          g_hud_sheet.ref[b].z - g_hud_sheet.ref[a].z);
                        }
                        g_draw_marks = pace ? coachpace::Marks(g_draw_verts, g_pace_hint, g_pace_phase)
                                            : std::vector<coachline::Vert>();
                        // The gear sign is one of the line's words: off with them (line_text=0).
                        if (GearOn() && g_gear_hint.level > 0 && g_hud_set.look.text) {
                            const std::vector<coachline::Vert> gm = coachgear::Marks(g_draw_verts, g_gear_hint);
                            g_draw_marks.insert(g_draw_marks.end(), gm.begin(), gm.end());
                        }
                        draw         = g_draw_verts.size() >= 4;
                        if (!draw) ++g_why[WHY_NO_VERTS];
                        BuildMarks();
                    }
                }
                // Every 10 s while it is on: where the camera came from, and what was drawn.
                if (lock.owns_lock() && g_gl_ok && g_hud_set.ground && now - g_gl_logged > 10000) {
                    g_gl_logged = now;
                    Log("ground", "frames=" + std::to_string(g_gl.n_frames) +
                                      " uniforms=" + std::to_string(g_ups_total) +
                                      " modelview_loads=" + std::to_string(g_mv_loads) +
                                      " camera=" + SourceName(g_source) +
                                      " picked=" + std::to_string(g_u_picked - g_prev_picked) +
                                      " drawn=" + std::to_string(g_gl.n_drawn - g_prev_drawn) + " (last 10 s)" +
                                      " snap=" + [] {
                                          if (g_line_on_ground) return std::string("off (on the track's own ground)");
                                          char o[160];
                                          float jy, js;
                                          g_ribbon.jitter(jy, js);
                                          std::snprintf(o, sizeof(o), "%s max=%.2f reads=%u used=%u jitter dy=%.3f shift=%.3f",
                                                        g_snap_on ? "on" : "off", double(g_lsnap.largest()), g_dsnap_reads,
                                                        g_dsnap_used, double(jy), double(js));
                                          return std::string(o) + (g_snap_on ? "" : " (" + g_scheck.why() + ")");
                                      }() +
                                      " draw_errors=" + std::to_string(g_gl.n_draw_errors) +
                                      " depth=" + std::to_string(int(g_depth)) +
                                      " height_gap=" + std::to_string(g_height.gap()) +
                                      " offset=" + std::to_string(g_height.offset()) +
                                      " ribbon=" + std::to_string(g_ribbon.verts().size()) + " verts");
                    // Why frames that had a camera drew nothing, since the last line.
                    std::string why;
                    for (int w = 0; w < WHY_COUNT; ++w)
                        if (g_why[w]) why += std::string(" ") + kWhyName[w] + "=" + std::to_string(g_why[w]);
                    Log("ground", "not drawn:" + (why.empty() ? std::string(" none") : why) + " (stale_redraw drew from the last camera)");
                    g_prev_picked = g_u_picked;
                    g_prev_drawn  = g_gl.n_drawn;
                    std::fill(std::begin(g_why), std::end(g_why), 0u);
                }
            }
            if (draw) DrawGroundLine(proj, view, model, ax, depth_ok);
            if (draw) DrawMarks(proj, view, model, ax, depth_ok);
        }
    } catch (...) {
        g_gl.own = false;
    }
    if (GetCurrentThreadId() == g_gl.thread) {
        g_gl.cands.clear();
        g_gl.building = false;
        g_ups_total += g_ups_frame;
        g_ups_frame = 0;
        ClearUploads();
        g_mv_best = coachline::ModelviewPick{};
    }
    return g_orig_swap(hdc);
}

/// Hooks the GL entry points the first time the ground line is asked for. All of them or none:
/// a partial set could leave the draw calling through a null original. One attempt per run.
void InstallGroundHooks() {
    if (g_gl_tried) return;
    g_gl_tried = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) {
        Log("ground", "opengl32 not loaded; no ground line");
        return;
    }
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ground", "MinHook failed to initialise; no ground line");
        return;
    }
    g_gl.cands.reserve(kMaxCandidates);
    g_draw_verts.reserve(256);
    struct H {
        const char* n;
        void*       d;
        void**      o;
    };
    const H hs[] = {{"glMatrixMode", (void*)&hkMode, (void**)&g_orig_mode},
                    {"glLoadMatrixf", (void*)&hkLoadf, (void**)&g_orig_loadf},
                    {"glLoadMatrixd", (void*)&hkLoadd, (void**)&g_orig_loadd},
                    {"glMultMatrixf", (void*)&hkMultf, (void**)&g_orig_multf},
                    {"glMultMatrixd", (void*)&hkMultd, (void**)&g_orig_multd},
                    {"glLoadIdentity", (void*)&hkIdentity, (void**)&g_orig_identity},
                    {"glFrustum", (void*)&hkFrustum, (void**)&g_orig_frustum},
                    {"glOrtho", (void*)&hkOrtho, (void**)&g_orig_ortho},
                    {"glTranslatef", (void*)&hkTranslatef, (void**)&g_orig_translatef},
                    {"glTranslated", (void*)&hkTranslated, (void**)&g_orig_translated},
                    {"glRotatef", (void*)&hkRotatef, (void**)&g_orig_rotatef},
                    {"glRotated", (void*)&hkRotated, (void**)&g_orig_rotated},
                    {"glScalef", (void*)&hkScalef, (void**)&g_orig_scalef},
                    {"glScaled", (void*)&hkScaled, (void**)&g_orig_scaled},
                    {"wglSwapBuffers", (void*)&hkSwap, (void**)&g_orig_swap}};
    std::vector<void*> made;
    for (const H& h : hs) {
        void* t = (void*)GetProcAddress(gl, h.n);
        if (!t || MH_CreateHook(t, h.d, h.o) != MH_OK) {
            Log("ground", std::string("hook failed: ") + h.n + "; no ground line");
            for (void* m : made) MH_RemoveHook(m);
            return;
        }
        made.push_back(t);
    }
    // Enabled together, in one thread-freezing pass, so no frame sees half of them.
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log("ground", "hooks could not be enabled; no ground line");
        for (void* m : made) MH_RemoveHook(m);
        return;
    }
    g_gl_ok = true;
    Log("ground", std::to_string(made.size()) + " GL capture hooks installed (read-only)");
}

/// Takes the hooks out, so nothing in the game calls into this DLL once it is unloaded.
void RemoveGroundHooks() {
    if (!g_gl_tried) return;
    g_gl_ok = false;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
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
        LiveBusyCheck();
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
    RemoveGroundHooks();
    LiveBusy(false);  // a clean exit, not a hang
    Log("mxbcoach", "shutting down");
    LogClose();
}

__declspec(dllexport) void EventInit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event(_pData, _iDataSize);
    g_event = coachcue::ReadEvent(_pData, _iDataSize);
    // How much travel each shock has. Without it there is nothing for the bars to be a
    // proportion of, so they stay away rather than draw a scale nobody knows the size of.
    g_susp_travel[0] = g_susp_travel[1] = 0;
    if (_pData && _iDataSize >= int(kEventSuspMaxTravel + 8)) {
        const auto* eb = static_cast<const uint8_t*>(_pData);
        g_susp_travel[0] = coachcue::F32(eb + kEventSuspMaxTravel);
        g_susp_travel[1] = coachcue::F32(eb + kEventSuspMaxTravel + 4);
    }
    Log("event", coachlog::EventText(g_event.type, g_event.track, g_event.bike, g_event.track_len, g_event.server));
    LoadCues();
    LoadHud();
    g_grid.clear();
    g_grid_said_missing = false;
    g_grid_bad.clear();
    LoadGround(false);
    ResetLiveGround();
    g_rider_state.reset();
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
    g_cue_file.clear();
    g_hud_sheet = coachhud::Sheet{};
    g_grid.clear();
    g_align.reset();
    ResetLiveGround();
    BuildExtras();
    g_hud_file.clear();
    g_ref.load({});
    g_track.clear();
    g_height.reset();
    g_ribbon.clear();
    g_ukey.reset();
    g_mv_lock.reset();
    g_follow.reset();
    g_view_ref.reset();
    g_view_miss = 0;
    g_dsnap_reset = true;
    g_last_cam.ok = false;
    g_cam_since = 0;
    g_depth = DEPTH_LEQUAL;
    g_practice = false;
    ReleaseDevice();
    StopVoice();
}

// _pRaceData: floats, the start/finish line's distance along the centreline first.
__declspec(dllexport) void TrackCenterline(int _iNumSegments, void* _pasSegment, void* _pRaceData) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_centreline(_iNumSegments, _pasSegment, coachrec::kTrackSegmentSize);
    g_track.build(_iNumSegments, _pasSegment, coachrec::kTrackSegmentSize, static_cast<const float*>(_pRaceData));
    g_height.reset();
    g_ribbon.clear();
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
    g_draw_calls  = 0;
    // A new run starts on the bike, not in the dirt: left set, the first cue of the session
    // would be swallowed.
    g_was_crashed = false;
    ReadVoiceSettings(false);
    g_setup = coachhud::SetupName(_pData, _iDataSize);
    g_clock.reset();
    g_stop.reset();
    g_have_sample = false;
    g_cam_since   = 0;
    g_dsnap_reset = true;
    // The bottomed marks belong to the stint, not to the session.
    g_susp[0] = g_susp[1] = g_susp_deep[0] = g_susp_deep[1] = 0;
    g_have_susp = false;
    ReadHudSettings(true);
}

__declspec(dllexport) void RunDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_run_end();
    g_cues.set_practice(false);
    StopVoice();
    // The whole stint in one line, and the one that separates the two worlds: frames=0 means
    // the game never called Draw, so nothing we build could ever have appeared.
    Log("draw", "stint ended: frames=" + std::to_string(g_draw_calls) +
                    " practice=" + (g_practice ? "yes" : "no") +
                    " cues fired=" + std::to_string(g_cues.fires()) +
                    " hud=" + (g_hud_set.enabled ? "on" : "off") + " map=" + (g_hud_set.map ? "on" : "off"));
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
    // The line: a newer sheet is swapped in where every cue is re-armed and the gap restarts.
    LookForCues(true);
    LookForHud(true);
}

__declspec(dllexport) void RunSplit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_split(_pData, _iDataSize);
}

/// RunTelemetry's part under g_mu; hands back what the game's ground needs.
LiveIn RunTelemetryLocked(void* _pData, int _iDataSize, float _fTime, float _fPos) {
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
        //
        // Silenced once, when the crash happens, not for as long as the rider is down. This
        // runs at 50 Hz: while `crashed` stayed true it called waveOutReset fifty times a
        // second, which is a good way to leave the sound device unable to play anything for
        // the rest of the run - and it logged each one, filling the log's 256 KB cap in about
        // a minute and a half, taking with it every line that would have explained the silence.
        if (crashed) {
            if (!g_was_crashed) {
                g_was_crashed = true;
                StopVoice();
            }
        } else if (g_was_crashed) {
            g_was_crashed = false;
        } else if (g_cues.fires() != fired && g_cues.showing()) {
            const coachcue::Cue& c = *g_cues.showing();
            Speak(c);
            Log("cue", "\"" + c.text + "\" at " + std::to_string(int(c.at_m)) + "m");
        }
        g_clock.on_sample(_fTime, _fPos);
        g_stop.on_sample(_fTime, speed);
        const coachhud::Pt here = {coachcue::F32(b + kDataPosX), coachcue::F32(b + kDataPosZ)};
        // Far enough apart to be travel rather than the bike shuffling on the spot: at racing
        // speed that is a few samples, and standing still it simply never updates.
        if (g_have_dir_from) {
            const float dx = here.x - g_dir_from.x, dz = here.y - g_dir_from.y;
            if (dx * dx + dz * dz >= kDirStepM * kDirStepM) {
                g_rider_dir = {dx, dz};
                g_dir_from  = here;
            }
        } else {
            g_dir_from      = here;
            g_have_dir_from = true;
        }
        g_rider       = here;
        g_time        = _fTime;
        g_pos         = _fPos;
        g_rider_y     = coachcue::F32(b + kDataPosY);
        // SPluginsBikeData_t m_fVelocityX/Y/Z follow the position (mxb_api.h).
        if (_iDataSize >= int(kDataVelX + 12))
            for (int k = 0; k < 3; ++k) g_rider_v[k] = coachcue::F32(b + kDataVelX + size_t(k) * 4);
        // Where the ground is, for the line on the track: the bike's height over the centreline,
        // learnt while riding. Not while crashed or crawling, when the bike may be in a ditch.
        float centre_h = 0;
        if (!crashed && speed > 3.0f && g_track.height_at_lap(_fPos, centre_h)) g_height.add(g_rider_y - centre_h);
        g_rider_state.sample(_fTime, crashed, here.x, g_rider_y, here.y);
        // The one-time check that the track's grid and the telemetry share a frame.
        // On the ground only (a wheel touching something): in the air the bike's height says
        // nothing about the ground, and a supercross lap is nearly half air.
        const bool grounded = _iDataSize < int(kDataWheelMat + 8) || coachcue::U32(b + kDataWheelMat) != 0 ||
                              coachcue::U32(b + kDataWheelMat + 4) != 0;
        if (!crashed && speed > 3.0f && g_grid.ready() && g_align.add(g_grid, here.x, g_rider_y, here.y, grounded)) {
            const coachline::Alignment& a = g_align.result();
            char msg[260];
            std::snprintf(msg, sizeof(msg),
                          "trh aligned dx=%.0f dz=%.0f dy=%.2f spread=%.2f (%d samples on the grid, %d off it)%s",
                          double(a.dx), double(a.dz), double(a.dy), double(a.spread), a.on_grid, a.off_grid,
                          a.ok ? "" : " - NOT this track's ground; the line snaps to what the game draws instead");
            Log("ground", msg);
            if (a.ok) g_grid.shift(a.dx, a.dz);
        }
        if (!crashed && speed > 3.0f) LiveGroundAlign(here.x, g_rider_y, here.y, grounded);
        if (crashed || speed <= 3.0f)
            g_ride_since = 0;
        else if (!g_ride_since)
            g_ride_since = GetTickCount64();
        g_have_sample = true;
        g_sample_ms   = GetTickCount64();
        PaceSample(speed, _fTime, crashed);
        // How much of each shock's travel is in use, and the deepest it has been this stint.
        g_have_susp = g_susp_travel[0] > 0 || g_susp_travel[1] > 0;
        for (int i = 0; i < 2; ++i) {
            g_susp[i] = coachhud::SuspUsed(coachcue::F32(b + kDataSuspLength + size_t(i) * 4), g_susp_travel[i]);
            if (g_susp[i] > g_susp_deep[i]) g_susp_deep[i] = g_susp[i];
        }
        GearSample(int(coachcue::U32(b + kDataGear)), speed, _fTime, crashed);
    }
    // Every sample, take back any buffer the device has finished with, so the next cue always
    // finds one free. Reaping only when a cue turned up is what let them run out.
    Drain();
    // voice.ini changed while riding: MXB Coach can be switched to mid-stint.
    if (_fTime >= g_voice.next_check || _fTime + 2.0f < g_voice.next_check) {
        g_voice.next_check = _fTime + 2.0f;
        ReadVoiceSettings(false);
    }
    PollStance(_fTime, _fPos, crashed);
    PollLean(_fTime, _fPos);
    MaybeReloadHudSettings();
    // Only does anything while a sheet is missing: see coachcue::ShouldLook.
    LookForCues(false);
    LookForHud(false);
    if (!g_grid.ready() && GetTickCount64() - g_grid_look_ms >= 1000) LoadGround(true);
    LiveIn live;
    live.on        = g_hud_set.enabled && g_hud_set.ground && g_practice && g_have_sample;
    live.riding_ms = g_ride_since ? GetTickCount64() - g_ride_since : 0;
    return live;
}

__declspec(dllexport) void RunTelemetry(void* _pData, int _iDataSize, float _fTime, float _fPos) {
    // The game's ground outside g_mu: asking the game anything while holding it can stall Draw.
    LiveGroundStep(RunTelemetryLocked(_pData, _iDataSize, _fTime, _fPos));
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
        ++g_draw_calls;
        if (_iState == 0) g_track_draw_ms = GetTickCount64();
        if (_iState == 0 && g_practice) {
            // Before the frame is built, so a part picked up this frame is drawn where the
            // rider has just dragged it to rather than a frame behind.
            //
            // Polled here, not from RunTelemetry where it started: that callback only fires
            // during a live stint, which is the one state where the rider is looking at the
            // track rather than at the HUD. MXBMRP3 polls the mouse from its own render path
            // for the same reason, and their drag works.
            PollDrag();
            if (g_hud_set.ground) InstallGroundHooks();
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
