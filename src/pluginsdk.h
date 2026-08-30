// pluginsdk.h - the PiBoSo plugin ABI, one layout per title.
//
// MX Bikes, GP Bikes and Kart Racing Pro expose the SAME callback table (identical export
// names, identical order - readable in each shipped exe's .rdata) but NOT the same payload
// structs. Each title has its own `GetModDataVersion`, and the structs move with it:
//
//   * RaceTrackPosition  - MX/GP end with m_iCrashed; KRP has no such field (24 B, not 28).
//   * RaceClassification - KRP's header carries an extra m_iSessionSeries, and GP's and
//                          KRP's entries an extra m_fBestSpeed, so m_iNumEntries and
//                          m_iNumLaps sit at different offsets in all three.
//   * EventInit          - a different struct per title, and the only one whose tail we
//                          read (the server name), which makes a wrong prefix silent.
//
// Reading one title's memory with another's struct is exactly the class of bug that took
// GP Bikes down with the wrong reload table, minus the crash: it yields plausible garbage.
// So the structs are transcribed VERBATIM from PiBoSo's published examples (mxb_example.c,
// gpb_example.c, krp_example.c - one per title, all on the respective game's site), the
// sizes are asserted below, and the ingest code reads offsets derived from them with
// `offsetof` rather than any number typed by hand.
//
// Only the callbacks FrostMod actually consumes are here. Fields past the last one we read
// are left off where the struct is only ever read forwards (telemetry), and transcribed in
// full where we read its tail (the event structs).
#pragma once

#include <cstddef>

#include "offsets.h"

// ---------------------------------------------------------------------------
// Shared shapes - identical in all three SDKs, so they need no per-title copy.
// ---------------------------------------------------------------------------
namespace sdk {

// RunTelemetry payload, prefix only. The first six fields are the same in all three
// (int RPM, two temperatures, int gear, fuel, speedometer), so the world position lands at
// the same offset everywhere. Everything after it differs per title and we don't read it.
struct VehicleDataPrefix {
    int   m_iRPM;
    float m_fTemperature1, m_fTemperature2;   // engine/head + water; unused, names differ
    int   m_iGear;
    float m_fFuel, m_fSpeedometer;
    float m_fPosX, m_fPosY, m_fPosZ;          // world position (all we read)
};
static_assert(sizeof(VehicleDataPrefix) == 36, "telemetry prefix moved");
static_assert(offsetof(VehicleDataPrefix, m_fPosX) == 24, "telemetry position moved");

// RaceTrackPosition array element, prefix only. The six fields every title has, in the
// order every title has them; MX and GP append m_iCrashed, which `PluginAbi::tp_crashed`
// locates when the title carries it. Asserted against all three layouts at the bottom of
// this file.
struct TrackPositionPrefix {
    int   m_iRaceNum;
    float m_fPosX, m_fPosY, m_fPosZ;   // metres
    float m_fYaw;                      // angle from north, DEGREES (not radians)
    float m_fTrackPos;                 // 0..1 along the centreline
};
static_assert(sizeof(TrackPositionPrefix) == 24, "track position prefix moved");

// RaceEvent payload. Byte-identical in mxb/gpb/krp_example.c.
struct RaceEvent {
    int   m_iType;                 // -1 = replay; 1 = testing; 2 = race; 4 = challenge
    char  m_szName[100];           // the server / event name
    char  m_szTrackName[100];
    float m_fTrackLength;          // metres
};
static_assert(sizeof(RaceEvent) == 208, "RaceEvent moved");

// RaceAddEntry payload. Byte-identical in all three (the middle strings are named after
// each title's vehicle, but they are the same three 100-byte fields).
struct RaceAddEntry {
    int  m_iRaceNum;
    char m_szName[100], m_szVehicleName[100], m_szVehicleShortName[100], m_szCategory[100];
    int  m_iUnactive, m_iNumberOfGears, m_iMaxRPM;
};
static_assert(sizeof(RaceAddEntry) == 416, "RaceAddEntry moved");

} // namespace sdk

// ---------------------------------------------------------------------------
// MX Bikes  (mxb_example.c, data version 8)
// ---------------------------------------------------------------------------
namespace sdk_mxb {

struct SPluginsRaceTrackPosition_t {
    int   m_iRaceNum;
    float m_fPosX, m_fPosY, m_fPosZ;   // metres
    float m_fYaw;                      // angle from north, DEGREES (not radians)
    float m_fTrackPos;                 // 0..1 along the centreline
    int   m_iCrashed;
};
static_assert(sizeof(SPluginsRaceTrackPosition_t) == 28, "MX track position moved");

struct SPluginsRaceClassification_t {
    int m_iSession, m_iSessionState, m_iSessionTime, m_iNumEntries;
};
struct SPluginsRaceClassificationEntry_t {
    int m_iRaceNum, m_iState, m_iBestLap, m_iBestLapNum, m_iNumLaps;
    int m_iGap, m_iGapLaps, m_iPenalty, m_iPit;
};
static_assert(sizeof(SPluginsRaceClassification_t) == 16, "MX classification header moved");
static_assert(sizeof(SPluginsRaceClassificationEntry_t) == 36, "MX classification entry moved");

// EventInit payload. We read fields at the *end* of this one, so every preceding field must
// match the build exactly - a wrong size anywhere ahead of m_szServerName silently yields
// garbage rather than an error.
//
// NOTE the last three fields are NOT in the published mxb_example.c, which still describes
// the pre-server-name struct: data version 8 appended them, and the field-tested proof is
// that FrostMod has been reading correct server names out of this offset in production
// since v0.12. Expect the same lag for the other two titles' examples.
struct SPluginsBikeEvent_t {
    char  m_szRiderName[100];
    char  m_szBikeID[100];
    char  m_szBikeName[100];
    int   m_iNumberOfGears;
    int   m_iMaxRPM;
    int   m_iLimiter;
    int   m_iShiftRPM;
    float m_fEngineOptTemperature;
    float m_afEngineTemperatureAlarm[2];
    float m_fMaxFuel;
    float m_afSuspMaxTravel[2];
    float m_fSteerLock;
    char  m_szCategory[100];
    char  m_szTrackID[100];
    char  m_szTrackName[100];
    float m_fTrackLength;
    int   m_iType;                   // 1 = testing; 2 = race; 4 = straight rhythm
    char  m_szServerName[64];        // the server we joined
    int   m_iServerType;
    char  m_szGUID[100];             // OUR OWN guid - the local player only
};
static_assert(sizeof(SPluginsBikeEvent_t) == 820, "MX event moved");

} // namespace sdk_mxb

// ---------------------------------------------------------------------------
// GP Bikes  (gpb_example.c, data version 12)
// ---------------------------------------------------------------------------
namespace sdk_gpb {

// Same as MX's, m_iCrashed included.
struct SPluginsRaceTrackPosition_t {
    int   m_iRaceNum;
    float m_fPosX, m_fPosY, m_fPosZ;
    float m_fYaw;
    float m_fTrackPos;
    int   m_iCrashed;
};
static_assert(sizeof(SPluginsRaceTrackPosition_t) == 28, "GP track position moved");

// Header matches MX's; the entry carries m_fBestSpeed, which pushes m_iNumLaps back 4.
struct SPluginsRaceClassification_t {
    int m_iSession, m_iSessionState, m_iSessionTime, m_iNumEntries;
};
struct SPluginsRaceClassificationEntry_t {
    int   m_iRaceNum, m_iState, m_iBestLap;
    float m_fBestSpeed;
    int   m_iBestLapNum, m_iNumLaps;
    int   m_iGap, m_iGapLaps, m_iPenalty, m_iPit;
};
static_assert(sizeof(SPluginsRaceClassification_t) == 16, "GP classification header moved");
static_assert(sizeof(SPluginsRaceClassificationEntry_t) == 40, "GP classification entry moved");

// As published: no server name, no GUID. Whether GP's shipped data version 12 appends a
// tail the way MX's 8 does is unknown - EventInit logs the size it was handed, and a size
// past this one is the evidence that it does.
struct SPluginsBikeEvent_t {
    char  m_szRiderName[100];
    char  m_szBikeID[100];
    char  m_szBikeName[100];
    int   m_iNumberOfGears;
    int   m_iMaxRPM;
    int   m_iLimiter;
    int   m_iShiftRPM;
    float m_fEngineOptTemperature;
    float m_afEngineTemperatureAlarm[2];
    float m_fMaxFuel;
    float m_afSuspMaxTravel[2];
    float m_fSteerLock;
    char  m_szCategory[100];
    char  m_szTrackID[100];
    char  m_szTrackName[100];
    float m_fTrackLength;
    int   m_iType;
};
static_assert(sizeof(SPluginsBikeEvent_t) == 652, "GP event moved");

} // namespace sdk_gpb

// ---------------------------------------------------------------------------
// Kart Racing Pro  (krp_example.c, data version 6)
// ---------------------------------------------------------------------------
namespace sdk_krp {

// No m_iCrashed: karts don't crash out the way a bike does, and the field simply isn't in
// the struct. 24 bytes, so a guard written against MX's 28 rejects the whole array.
struct SPluginsRaceTrackPosition_t {
    int   m_iRaceNum;
    float m_fPosX, m_fPosY, m_fPosZ;
    float m_fYaw;                      // angle from north, degrees
    float m_fTrackPos;                 // 0..1 along the centreline
};
static_assert(sizeof(SPluginsRaceTrackPosition_t) == 24, "KRP track position moved");

// Both the header and the entry are wider than MX's: m_iSessionSeries in the header (karts
// run heats), m_fBestSpeed in the entry.
struct SPluginsRaceClassification_t {
    int m_iSession, m_iSessionSeries, m_iSessionState, m_iSessionTime, m_iNumEntries;
};
struct SPluginsRaceClassificationEntry_t {
    int   m_iRaceNum, m_iState, m_iBestLap;
    float m_fBestSpeed;
    int   m_iBestLapNum, m_iNumLaps;
    int   m_iGap, m_iGapLaps, m_iPenalty, m_iPit;
};
static_assert(sizeof(SPluginsRaceClassification_t) == 20, "KRP classification header moved");
static_assert(sizeof(SPluginsRaceClassificationEntry_t) == 40, "KRP classification entry moved");

// A different shape from the bike titles', not a superset: the drive type and engine
// cooling are kart-specific, there is a dash name where the bikes have suspension travel,
// and - as published - there is no server name and no GUID at all. See EventInit: the size
// the callback reports is what settles whether the shipped build appends them.
struct SPluginsKartEvent_t {
    char  m_szDriverName[100];
    char  m_szKartID[100];
    char  m_szKartName[100];
    int   m_iDriveType;              // 0 = direct; 1 = clutch; 2 = shifter
    int   m_iNumberOfGears;
    int   m_iMaxRPM;
    int   m_iLimiter;
    int   m_iShiftRPM;
    int   m_iEngineCooling;          // 0 = aircooled; 1 = watercooled
    float m_fEngineOptTemperature;
    float m_afEngineTemperatureAlarm[2];
    float m_fMaxFuel;
    char  m_szCategory[100];
    char  m_szDash[100];
    char  m_szTrackID[100];
    char  m_szTrackName[100];
    float m_fTrackLength;
    int   m_iType;                   // 1 = testing; 2 = race; 4 = challenge
};
static_assert(sizeof(SPluginsKartEvent_t) == 748, "KRP event moved");

} // namespace sdk_krp

// ---------------------------------------------------------------------------
// What the ingest code actually needs: sizes and field offsets, per title.
//
// Every number is `sizeof`/`offsetof` over the structs above, so the descriptors cannot
// drift from the transcriptions - and a field that a title does not have is -1, not 0,
// because 0 is a valid offset (it is where the driver name lives).
// ---------------------------------------------------------------------------
struct PluginAbi {
    /// Which published example this was transcribed from. Logged once, so a log tells you
    /// which layout produced the numbers in it.
    const char* sdk;

    // RaceTrackPosition array element.
    int tp_size;
    int tp_crashed;        // -1 when the title has no crashed flag

    // RaceClassification header + array element.
    int cls_hdr_size, cls_num_entries;
    int cls_entry_size, cls_entry_num_laps;

    // EventInit payload.
    int ev_size;           // as transcribed; the callback may report MORE (see below)
    int ev_rider, ev_track;
    int ev_server, ev_guid;  // -1 = this title's published struct has no such field
};

inline constexpr PluginAbi kAbiMxb = {
    "mxb_example.c",
    (int)sizeof(sdk_mxb::SPluginsRaceTrackPosition_t),
    (int)offsetof(sdk_mxb::SPluginsRaceTrackPosition_t, m_iCrashed),
    (int)sizeof(sdk_mxb::SPluginsRaceClassification_t),
    (int)offsetof(sdk_mxb::SPluginsRaceClassification_t, m_iNumEntries),
    (int)sizeof(sdk_mxb::SPluginsRaceClassificationEntry_t),
    (int)offsetof(sdk_mxb::SPluginsRaceClassificationEntry_t, m_iNumLaps),
    (int)sizeof(sdk_mxb::SPluginsBikeEvent_t),
    (int)offsetof(sdk_mxb::SPluginsBikeEvent_t, m_szRiderName),
    (int)offsetof(sdk_mxb::SPluginsBikeEvent_t, m_szTrackID),
    (int)offsetof(sdk_mxb::SPluginsBikeEvent_t, m_szServerName),
    (int)offsetof(sdk_mxb::SPluginsBikeEvent_t, m_szGUID),
};

inline constexpr PluginAbi kAbiGpb = {
    "gpb_example.c",
    (int)sizeof(sdk_gpb::SPluginsRaceTrackPosition_t),
    (int)offsetof(sdk_gpb::SPluginsRaceTrackPosition_t, m_iCrashed),
    (int)sizeof(sdk_gpb::SPluginsRaceClassification_t),
    (int)offsetof(sdk_gpb::SPluginsRaceClassification_t, m_iNumEntries),
    (int)sizeof(sdk_gpb::SPluginsRaceClassificationEntry_t),
    (int)offsetof(sdk_gpb::SPluginsRaceClassificationEntry_t, m_iNumLaps),
    (int)sizeof(sdk_gpb::SPluginsBikeEvent_t),
    (int)offsetof(sdk_gpb::SPluginsBikeEvent_t, m_szRiderName),
    (int)offsetof(sdk_gpb::SPluginsBikeEvent_t, m_szTrackID),
    -1, -1,   // no server name / GUID in the published struct
};

inline constexpr PluginAbi kAbiKrp = {
    "krp_example.c",
    (int)sizeof(sdk_krp::SPluginsRaceTrackPosition_t),
    -1,       // no crashed flag
    (int)sizeof(sdk_krp::SPluginsRaceClassification_t),
    (int)offsetof(sdk_krp::SPluginsRaceClassification_t, m_iNumEntries),
    (int)sizeof(sdk_krp::SPluginsRaceClassificationEntry_t),
    (int)offsetof(sdk_krp::SPluginsRaceClassificationEntry_t, m_iNumLaps),
    (int)sizeof(sdk_krp::SPluginsKartEvent_t),
    (int)offsetof(sdk_krp::SPluginsKartEvent_t, m_szDriverName),
    (int)offsetof(sdk_krp::SPluginsKartEvent_t, m_szTrackID),
    -1, -1,   // no server name / GUID in the published struct
};

// The shared prefix must line up with every title's own struct, or the radar reads one
// title's yaw out of another's padding. Cheap to assert, so assert it rather than trusting
// three transcriptions to stay in step.
#define FROSTMOD_TP_PREFIX_MATCHES(NS, T)                                                  \
    static_assert(offsetof(NS::T, m_iRaceNum) == offsetof(sdk::TrackPositionPrefix, m_iRaceNum) && \
                  offsetof(NS::T, m_fPosX)    == offsetof(sdk::TrackPositionPrefix, m_fPosX)    && \
                  offsetof(NS::T, m_fYaw)     == offsetof(sdk::TrackPositionPrefix, m_fYaw)     && \
                  offsetof(NS::T, m_fTrackPos)== offsetof(sdk::TrackPositionPrefix, m_fTrackPos),  \
                  #NS " track position disagrees with the shared prefix")
FROSTMOD_TP_PREFIX_MATCHES(sdk_mxb, SPluginsRaceTrackPosition_t);
FROSTMOD_TP_PREFIX_MATCHES(sdk_gpb, SPluginsRaceTrackPosition_t);
FROSTMOD_TP_PREFIX_MATCHES(sdk_krp, SPluginsRaceTrackPosition_t);
#undef FROSTMOD_TP_PREFIX_MATCHES

// m_iRaceNum leads every classification entry in all three, which is why the ingest reads
// it straight off the element pointer and only needs an offset for m_iNumLaps.
static_assert(offsetof(sdk_mxb::SPluginsRaceClassificationEntry_t, m_iRaceNum) == 0 &&
              offsetof(sdk_gpb::SPluginsRaceClassificationEntry_t, m_iRaceNum) == 0 &&
              offsetof(sdk_krp::SPluginsRaceClassificationEntry_t, m_iRaceNum) == 0,
              "a classification entry no longer starts with the race number");

/// The layout to read this title's callbacks with. Defaults to MX Bikes' for an
/// unrecognised host, matching what DetectGame() assumes.
inline constexpr const PluginAbi* PluginAbiFor(const GameOffsets* g) {
    return g == &GAME_KRP ? &kAbiKrp
         : g == &GAME_GPB ? &kAbiGpb
                          : &kAbiMxb;
}
