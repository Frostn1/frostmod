// others.h - the other riders, for MXB Coach's recorder.
//
// The Run* callbacks carry only the local rider. The Race* callbacks carry everyone: who is in
// the event (RaceAddEntry / RaceRemoveEntry), where each bike is (RaceTrackPosition, called
// often), and every rider's lap and split times (RaceLap / RaceSplit). The recorder writes
// them into the stint's .mxbc file so the app can put the rider next to the one just ahead,
// section by section, and see the lines others take through each corner.
//
// Which entry is the local rider: the plugin API never says. The recorder flags the
// RaceTrackPosition entry closest to its own latest telemetry position, within kLocalRadius,
// the way FrostMod's radar finds "me".
//
// No Win32 here, so tests/others_test.cpp runs anywhere. mxbcoach.cpp feeds it from the
// callbacks and writes the records only while a stint is recording, like SAMPLE.
//
// Records in the .mxbc file (tags in coachrec.h; little-endian):
//   ENTRY       152 bytes, for every rider in the event when a stint opens, then whenever
//               RaceAddEntry or RaceRemoveEntry arrives during it
//      0   i32  race number
//      4   u8   1 riding, 0 left the event
//      5   u8[3] zero
//      8   char[64] name              NUL-padded, cut to 63
//      72  char[40] bike short name   NUL-padded, cut to 39
//      112 char[40] category          NUL-padded, cut to 39
//   POSITIONS   8 + 20 per bike, at most 10 a second of track time
//      0   f32  track time s          of the latest SAMPLE
//      4   u16  bikes
//      6   u16  bytes per bike (20); a reader steps by this
//      then per bike:
//      0   u16  race number
//      2   u8   flags: bit 0 crashed, bit 1 the local rider
//      3   u8   zero
//      4   f32  track position 0..1
//      8   f32  x m
//      12  f32  y m (height)
//      16  f32  z m
//   RACE_LAP    f32 track time s, then raw SPluginsRaceLap_t (32: session, race number, lap,
//               invalid, lap time ms, split ms[2], best)
//   RACE_SPLIT  f32 track time s, then raw SPluginsRaceSplit_t (20: session, race number,
//               lap, split, split time ms)
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "pluginsdk.h"

namespace others {

constexpr size_t kEntrySize   = 152;
constexpr size_t kNameLen     = 64;
constexpr size_t kBikeLen     = 40;
constexpr size_t kCategoryLen = 40;

constexpr size_t kPosHeader = 8;
constexpr size_t kPosBike   = 20;
constexpr int    kMaxBikes  = 128;

constexpr uint8_t kCrashed = 1;
constexpr uint8_t kLocal   = 2;

// 10 a second. A hair under 0.1 s so 50 Hz sample times don't alias to every 6th sample.
constexpr float kPosInterval = 0.099f;
// The local rider's entry sits within a sample's travel of its own telemetry.
constexpr float kLocalRadius = 3.0f;

using Entry = std::array<uint8_t, kEntrySize>;

inline int32_t EntryRaceNum(const Entry& e) {
    int32_t n;
    std::memcpy(&n, &e[0], 4);
    return n;
}

// Up to `len - 1` bytes of a game string, stopping at its NUL.
inline void PutText(uint8_t* at, const char* s, size_t max_src, size_t len) {
    size_t n = 0;
    while (n < max_src && n < len - 1 && s[n]) ++n;
    std::memcpy(at, s, n);
}

/// The ENTRY payload for a RaceAddEntry payload. False if it is too short to hold one.
inline bool EntryFromAdd(const void* data, int size, Entry& out) {
    if (!data || size < int(sizeof(sdk::RaceAddEntry))) return false;
    sdk::RaceAddEntry a;
    std::memcpy(&a, data, sizeof(a));
    out.fill(0);
    std::memcpy(&out[0], &a.m_iRaceNum, 4);
    out[4] = a.m_iUnactive ? 0 : 1;
    PutText(&out[8], a.m_szName, sizeof(a.m_szName), kNameLen);
    PutText(&out[72], a.m_szVehicleShortName, sizeof(a.m_szVehicleShortName), kBikeLen);
    PutText(&out[112], a.m_szCategory, sizeof(a.m_szCategory), kCategoryLen);
    return true;
}

/// The local rider's world position from a RunTelemetry payload.
inline bool OwnPosition(const void* data, int size, float& x, float& y, float& z) {
    if (!data || size < int(sizeof(sdk::VehicleDataPrefix))) return false;
    sdk::VehicleDataPrefix p;
    std::memcpy(&p, data, sizeof(p));
    x = p.m_fPosX;
    y = p.m_fPosY;
    z = p.m_fPosZ;
    return true;
}

/// The POSITIONS payload for a RaceTrackPosition array. `me` is the local rider's position,
/// or null when it isn't known. False when the stride is shorter than MX Bikes' struct.
inline bool PositionsPayload(float time, int n, const void* arr, int elem, const float* me,
                             std::vector<uint8_t>& out) {
    using TP = sdk_mxb::SPluginsRaceTrackPosition_t;
    out.clear();
    if (!arr || elem < int(sizeof(TP)) || n < 0) return false;
    n = (std::min)(n, kMaxBikes);

    int   local = -1;
    float best  = kLocalRadius * kLocalRadius;
    for (int i = 0; me && i < n; ++i) {
        TP t;
        std::memcpy(&t, static_cast<const uint8_t*>(arr) + size_t(i) * size_t(elem), sizeof(t));
        const float dx = t.m_fPosX - me[0], dy = t.m_fPosY - me[1], dz = t.m_fPosZ - me[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) {
            best  = d2;
            local = i;
        }
    }

    out.resize(kPosHeader);
    std::memcpy(&out[0], &time, 4);
    uint16_t count = 0;
    for (int i = 0; i < n; ++i) {
        TP t;
        std::memcpy(&t, static_cast<const uint8_t*>(arr) + size_t(i) * size_t(elem), sizeof(t));
        if (t.m_iRaceNum < 0 || t.m_iRaceNum > 0xFFFF) continue;
        uint8_t b[kPosBike] = {0};
        const uint16_t num  = uint16_t(t.m_iRaceNum);
        std::memcpy(&b[0], &num, 2);
        b[2] = uint8_t((t.m_iCrashed ? kCrashed : 0) | (i == local ? kLocal : 0));
        std::memcpy(&b[4], &t.m_fTrackPos, 4);
        std::memcpy(&b[8], &t.m_fPosX, 4);
        std::memcpy(&b[12], &t.m_fPosY, 4);
        std::memcpy(&b[16], &t.m_fPosZ, 4);
        out.insert(out.end(), b, b + kPosBike);
        ++count;
    }
    const uint16_t stride = uint16_t(kPosBike);
    std::memcpy(&out[4], &count, 2);
    std::memcpy(&out[6], &stride, 2);
    return true;
}

/// f32 time then the game's payload, if it holds at least `min_size` bytes.
inline bool TimedPayload(float time, const void* data, int size, size_t min_size, std::vector<uint8_t>& out) {
    out.clear();
    if (!data || size < 0 || size_t(size) < min_size) return false;
    out.resize(4 + size_t(size));
    std::memcpy(&out[0], &time, 4);
    std::memcpy(&out[4], data, size_t(size));
    return true;
}

/// Who is in the event, and what the local rider's latest sample said. The roster outlives
/// stints: RaceAddEntry arrives when the event starts, often before the rider goes out.
class Tracker {
public:
    /// A new race event, or the event closed.
    void clear_roster() { roster_.clear(); }

    /// A stint opened: no sample yet, and the first POSITIONS is due at once.
    void on_stint() {
        have_time_ = have_me_ = false;
        have_last_ = false;
    }

    void on_sample(float time, const void* data, int size) {
        time_      = time;
        have_time_ = true;
        have_me_   = OwnPosition(data, size, me_[0], me_[1], me_[2]);
    }

    /// Updates the roster; `out` is the ENTRY to write.
    bool added(const void* data, int size, Entry& out) {
        if (!EntryFromAdd(data, size, out)) return false;
        roster_[EntryRaceNum(out)] = out;
        return true;
    }

    /// Drops the rider; `out` is their last ENTRY, marked as left.
    bool removed(const void* data, int size, Entry& out) {
        if (!data || size < 4) return false;
        int32_t num;
        std::memcpy(&num, data, 4);
        auto it = roster_.find(num);
        if (it != roster_.end()) {
            out = it->second;
            roster_.erase(it);
        } else {
            out.fill(0);
            std::memcpy(&out[0], &num, 4);
        }
        out[4] = 0;
        return true;
    }

    /// The POSITIONS record, when one is due: after the stint's first sample, and 10 a second.
    bool positions(int n, const void* arr, int elem, std::vector<uint8_t>& out) {
        out.clear();
        if (!have_time_) return false;
        if (have_last_ && time_ >= last_ && time_ - last_ < kPosInterval) return false;
        if (!PositionsPayload(time_, n, arr, elem, have_me_ ? me_ : nullptr, out)) return false;
        last_      = time_;
        have_last_ = true;
        return true;
    }

    bool lap(const void* data, int size, std::vector<uint8_t>& out) const {
        return have_time_ && TimedPayload(time_, data, size, sizeof(sdk_mxb::SPluginsRaceLap_t), out);
    }

    bool split(const void* data, int size, std::vector<uint8_t>& out) const {
        return have_time_ && TimedPayload(time_, data, size, sizeof(sdk_mxb::SPluginsRaceSplit_t), out);
    }

    const std::map<int32_t, Entry>& roster() const { return roster_; }

private:
    std::map<int32_t, Entry> roster_;
    float                    time_ = 0, last_ = 0;
    float                    me_[3] = {0, 0, 0};
    bool                     have_time_ = false, have_me_ = false, have_last_ = false;
};

}  // namespace others
