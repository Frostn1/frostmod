// mxbcoach.cpp - MXB Coach's recorder, a PiBoSo plugin (mxbcoach.dlo) for MX Bikes.
//
// The game loads it from its plugins\ folder and hands it the rider's own telemetry. It
// writes one .mxbc file per stint on track into <save path>\mxbcoach\sessions\, which the
// MXB Coach app reads afterwards. In practice it also shows the live cues the app writes to
// <save path>\mxbcoach\cues\, one short line at a time through the game's Draw callback. It
// hooks nothing: everything arrives through the published callbacks, and the rules live in
// coachrec.h and coachcue.h.
//
// MX Bikes only for now. GP Bikes and Kart Racing Pro send different telemetry structs.
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "coachcue.h"
#include "coachrec.h"
#include "offsets.h"

namespace {

std::mutex         g_mu;
coachrec::Recorder g_rec;
coachcue::Player   g_cues;
coachcue::Event    g_event;
std::string        g_base;  // <save path>\mxbcoach\

// SPluginsBikeData_t: speedometer m/s, and the crashed flag.
constexpr size_t kDataSpeed   = 20;
constexpr size_t kDataCrashed = 136;

// PiBoSo draw items, as in mxb_example.c. The game reads them after Draw() returns, so they
// live in these statics.
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
SPluginQuad_t   g_quad[1];
SPluginString_t g_text[1];

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

// The app's sheet for this track and bike, else for the track, if it was made for this track.
void LoadCues() {
    for (const std::string& name : coachcue::SheetNames(g_event)) {
        std::vector<uint8_t> b = ReadFile(g_base + "cues\\" + name);
        coachcue::Sheet s;
        if (!b.empty() && coachcue::Parse(b.data(), b.size(), s) && coachcue::Fits(s, g_event)) {
            g_cues.load(std::move(s));
            return;
        }
    }
    g_cues.clear();
}

unsigned long CueColour(uint8_t kind) {
    switch (kind) {
        case coachcue::BRAKE: return 0xFF4B4BFFul;       // red
        case coachcue::THROTTLE: return 0xFF5CD65Cul;    // green
        case coachcue::OFF_BRAKES: return 0xFF5CD6FFul;  // amber
        default: return 0xFFFFFFFFul;                    // white
    }
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
    dir += "mxbcoach\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_base = dir;
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
}

__declspec(dllexport) void EventInit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event(_pData, _iDataSize);
    g_event = coachcue::ReadEvent(_pData, _iDataSize);
    LoadCues();
    // Before any stint, only a testing event is practice; a race event waits for its session.
    g_cues.set_practice(g_event.type == 1);
}

__declspec(dllexport) void EventDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_event_end();
    g_cues.clear();
}

__declspec(dllexport) void TrackCenterline(int _iNumSegments, void* _pasSegment, void*) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_centreline(_iNumSegments, _pasSegment, coachrec::kTrackSegmentSize);
}

__declspec(dllexport) void RunInit(void* _pData, int _iDataSize) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_run_init(_pData, _iDataSize, Stamp().c_str());
    g_cues.set_practice(coachcue::Practice(g_event.type, coachcue::ReadSession(_pData, _iDataSize)));
}

__declspec(dllexport) void RunDeinit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rec.on_run_end();
    g_cues.set_practice(false);
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
    if (_pData && _iDataSize >= int(kDataCrashed + 4)) {
        const auto* b = static_cast<const uint8_t*>(_pData);
        g_cues.on_sample(_fPos, coachcue::F32(b + kDataSpeed), _fTime, coachcue::U32(b + kDataCrashed) != 0);
    }
}

// Each frame on track, spectating or in a replay: the cue showing, if any, as one line of text
// on a dark backing near the top of the screen. Only while riding.
__declspec(dllexport) void Draw(int _iState, int* _piNumQuads, void** _ppQuad, int* _piNumString,
                                void** _ppString) {
    int quads = 0, strings = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const coachcue::Cue* c = _iState == 0 ? g_cues.showing() : nullptr;
        if (c) {
            const float x0 = 0.35f, x1 = 0.65f, y0 = 0.14f, y1 = 0.21f;
            SPluginQuad_t& q = g_quad[0];
            q.m_aafPos[0][0] = x0; q.m_aafPos[0][1] = y0;
            q.m_aafPos[1][0] = x0; q.m_aafPos[1][1] = y1;
            q.m_aafPos[2][0] = x1; q.m_aafPos[2][1] = y1;
            q.m_aafPos[3][0] = x1; q.m_aafPos[3][1] = y0;
            q.m_iSprite = 0;
            q.m_ulColor = 0xA0000000ul;
            SPluginString_t& s = g_text[0];
            strncpy_s(s.m_szString, c->text.c_str(), _TRUNCATE);
            s.m_afPos[0] = 0.5f;
            s.m_afPos[1] = 0.155f;
            s.m_iFont = 1;
            s.m_fSize = 0.045f;
            s.m_iJustify = 1;
            s.m_ulColor = CueColour(c->kind);
            quads = strings = 1;
        }
    }
    if (_piNumQuads) *_piNumQuads = quads;
    if (_ppQuad) *_ppQuad = g_quad;
    if (_piNumString) *_piNumString = strings;
    if (_ppString) *_ppString = g_text;
}

}  // extern "C"
