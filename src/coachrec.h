// coachrec.h - MXB Coach's lap recorder: the .mxbc file format and the rules for when a
// session file opens, what goes in it, and when it closes.
//
// The recorder stores the game's own payloads byte for byte rather than picking fields out of
// them. The app decodes them with the published layouts, so a field we did not think to read
// today is already on disk for the coaching rule that wants it tomorrow, and a game build that
// appends fields costs nothing: every record carries its own length.
//
// No Win32 here, so tests/coachrec_test.cpp runs anywhere. mxbcoach.cpp is the thin plugin
// glue that feeds these calls from the game's callbacks.
//
// File layout (little-endian, as written by x64):
//   "MXBC"  u32 format version
//   then records:  u8 tag, u8[3] zero, u32 payload length, payload
//
//   EVENT       raw SPluginsBikeEvent_t (EventInit)          - once, first
//   CENTRELINE  i32 segment count, i32 segment size, segments - once, if the game sent it
//   SESSION     raw SPluginsBikeSession_t (RunInit)
//   SAMPLE      f32 track time s, f32 lap position 0..1, raw SPluginsBikeData_t
//   LAP         raw SPluginsBikeLap_t
//   SPLIT       raw SPluginsBikeSplit_t
//   START/STOP  no payload - simulation resumed / paused
//   END         no payload - the bike left the track; absent if the game died
//   STANCE_BIND the rider's Sit bind and how far to trust it - once, after SESSION (stance.h)
//   STANCE      f32 time, f32 position, u8 stand/sit/unknown - on each change (stance.h)
//
// A reader skips a tag it doesn't know by its length, so new tags keep the format version.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coachrec {

constexpr char     kMagic[4]      = {'M', 'X', 'B', 'C'};
constexpr uint32_t kFormatVersion = 1;

// 1 = 50 Hz in PiBoSo's Startup() convention: 0.6 m between samples at 30 m/s, enough to
// place a brake point, at half the disk of 100 Hz.
constexpr int kTelemetryRate = 1;

// Flush at least this often, so a game crash loses seconds rather than the session.
constexpr int kFlushEverySamples = 100;

// SPluginsTrackSegment_t: type, length, radius, angle, start[2], height. The callback hands
// an array without saying its stride, so the published size is all there is to go on.
constexpr int kTrackSegmentSize = 28;

enum Tag : uint8_t {
    EVENT      = 1,
    SESSION    = 2,
    CENTRELINE = 3,
    SAMPLE     = 4,
    LAP        = 5,
    SPLIT      = 6,
    START      = 7,
    STOP       = 8,
    END        = 9,
    STANCE_BIND = 10,
    STANCE      = 11,
};

/// Appends records to one file.
class Writer {
public:
    ~Writer() { close(); }

    bool open(const char* path) {
        close();
        f_ = std::fopen(path, "wb");
        if (!f_) return false;
        std::fwrite(kMagic, 1, 4, f_);
        write_u32(kFormatVersion);
        return true;
    }

    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }

    bool is_open() const { return f_ != nullptr; }

    /// One record whose payload is `a` followed by `b`.
    void record(Tag tag, const void* a = nullptr, uint32_t alen = 0,
                const void* b = nullptr, uint32_t blen = 0) {
        if (!f_) return;
        const uint8_t head[4] = {tag, 0, 0, 0};
        std::fwrite(head, 1, 4, f_);
        write_u32(alen + blen);
        if (alen) std::fwrite(a, 1, alen, f_);
        if (blen) std::fwrite(b, 1, blen, f_);
    }

    void flush() {
        if (f_) std::fflush(f_);
    }

private:
    void write_u32(uint32_t v) { std::fwrite(&v, 4, 1, f_); }

    std::FILE* f_ = nullptr;
};

/// The session rules. One file per stint on track: RunInit opens it, RunDeinit closes it.
class Recorder {
public:
    /// Folder the session files go in, with a trailing separator.
    void set_dir(std::string dir) { dir_ = std::move(dir); }
    const std::string& dir() const { return dir_; }

    /// Kept until the next stint opens a file: the event arrives once, before any of them.
    void on_event(const void* data, int size) {
        keep(event_, data, size);
        centreline_.clear();
    }

    void on_event_end() {
        on_run_end();
        event_.clear();
        centreline_.clear();
    }

    void on_centreline(int count, const void* segments, int segment_size) {
        if (count <= 0 || !segments || segment_size <= 0) return;
        centreline_.resize(8 + size_t(count) * size_t(segment_size));
        std::memcpy(centreline_.data(), &count, 4);
        std::memcpy(centreline_.data() + 4, &segment_size, 4);
        std::memcpy(centreline_.data() + 8, segments, size_t(count) * size_t(segment_size));
    }

    /// Opens `<dir><stamp>.mxbc`. Returns false if the file could not be created, in which
    /// case the stint is simply not recorded.
    bool on_run_init(const void* session, int size, const char* stamp) {
        on_run_end();
        path_ = dir_ + stamp + ".mxbc";
        if (!w_.open(path_.c_str())) return false;
        w_.record(EVENT, event_.data(), uint32_t(event_.size()));
        if (!centreline_.empty())
            w_.record(CENTRELINE, centreline_.data(), uint32_t(centreline_.size()));
        w_.record(SESSION, session, clamp(size));
        w_.flush();
        since_flush_ = 0;
        return true;
    }

    void on_run_end() {
        if (!w_.is_open()) return;
        w_.record(END);
        w_.close();
    }

    void on_start() { w_.record(START); }

    void on_stop() {
        w_.record(STOP);
        w_.flush();
    }

    void on_lap(const void* lap, int size) {
        w_.record(LAP, lap, clamp(size));
        w_.flush();
        since_flush_ = 0;
    }

    void on_split(const void* split, int size) { w_.record(SPLIT, split, clamp(size)); }

    void on_sample(const void* data, int size, float time, float pos) {
        if (!w_.is_open() || !data || size <= 0) return;
        float head[2] = {time, pos};
        w_.record(SAMPLE, head, sizeof(head), data, uint32_t(size));
        if (++since_flush_ >= kFlushEverySamples) {
            w_.flush();
            since_flush_ = 0;
        }
    }

    /// A record another module builds, such as stance. Dropped outside a stint.
    void record(Tag tag, const void* payload, uint32_t size) { w_.record(tag, payload, size); }

    bool recording() const { return w_.is_open(); }
    const std::string& path() const { return path_; }

private:
    static uint32_t clamp(int size) { return size > 0 ? uint32_t(size) : 0; }

    static void keep(std::vector<uint8_t>& into, const void* data, int size) {
        into.clear();
        if (data && size > 0)
            into.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + size);
    }

    Writer               w_;
    std::string          dir_, path_;
    std::vector<uint8_t> event_, centreline_;
    int                  since_flush_ = 0;
};

}  // namespace coachrec
