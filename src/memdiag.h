// Where the game's system RAM goes: the bookkeeping behind memdiag=1 (frostmod_radar.cfg).
//
// MX Bikes renders with OpenGL, and a GL driver on Windows keeps a system-RAM copy of what
// it is handed (textures, buffer objects). A stock game was seen at 9 GB. This tallies what
// the game uploads, per texture name and level, so a log can say how much of that is live
// texture / buffer data (and how much of it is RGBA8 that compression would shrink) against
// the process's own PrivateUsage. It is a diagnostic and changes nothing the game does.
//
// Pure C++ with no GL or Win32 headers, so the arithmetic and the accounting run in CI
// (tests/memdiag_test.cpp). Not thread-safe by itself: the caller holds one lock around it.
// The GL hooks that feed it live in frostmod.cpp (MEMDIAG section).
#pragma once

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace memdiag {

// The GL enums this needs, spelled out so the header stands without glext.h.
enum : uint32_t {
    kAlpha = 0x1906, kRgb = 0x1907, kRgba = 0x1908, kLuminance = 0x1909, kLuminanceAlpha = 0x190A,
    kRed = 0x1903, kDepthComponent = 0x1902, kBgr = 0x80E0, kBgra = 0x80E1,
    kAlpha8 = 0x803C, kLuminance8 = 0x8040, kLuminance8Alpha8 = 0x8045, kIntensity8 = 0x804B,
    kRgb8 = 0x8051, kRgb10A2 = 0x8059, kRgba8 = 0x8058, kRgba16 = 0x805B, kRgb5A1 = 0x8057,
    kRgba4 = 0x8056, kRgb565 = 0x8D62,
    kR8 = 0x8229, kRg8 = 0x822B, kR16F = 0x822D, kR32F = 0x822E, kRg16F = 0x822F,
    kRgba32F = 0x8814, kRgb32F = 0x8815, kRgba16F = 0x881A, kRgb16F = 0x881B,
    kR11G11B10F = 0x8C3A, kSrgb8 = 0x8C41, kSrgb8Alpha8 = 0x8C43,
    kDepth16 = 0x81A5, kDepth24 = 0x81A6, kDepth32 = 0x81A7, kDepth24Stencil8 = 0x88F0,
    kCompressedRgb = 0x84ED, kCompressedRgba = 0x84EE,
    kDxt1Rgb = 0x83F0, kDxt1Rgba = 0x83F1, kDxt3 = 0x83F2, kDxt5 = 0x83F3,
    kSrgbDxt1 = 0x8C4C, kSrgbDxt1A = 0x8C4D, kSrgbDxt3 = 0x8C4E, kSrgbDxt5 = 0x8C4F,
    kRgtc1 = 0x8DBB, kRgtc2 = 0x8DBD,
    // pixel transfer types (glTexSubImage2D)
    kUByte = 0x1401, kByte = 0x1400, kUShort = 0x1403, kShort = 0x1402, kUInt = 0x1405,
    kInt = 0x1404, kFloat = 0x1406, kHalf = 0x140B,
    // texture targets
    kTex2D = 0x0DE1, kCubePosX = 0x8515, kCubeNegZ = 0x851A,
    kProxy2D = 0x8064, kProxyCube = 0x851B,
};

/// What a log line groups a texture under.
enum Class : int {
    CLS_RGBA8 = 0,      // 4 bytes a texel, uncompressed: the one compression would shrink 4-8x
    CLS_RGB8,           // 3 channels, which drivers store padded to 4
    CLS_COMPRESSED,     // handed over already compressed (glCompressedTexImage2D)
    CLS_DRIVER_COMP,    // a compressed internal format given raw texels: the driver compresses
    CLS_SMALL,          // 1-2 bytes a texel (luminance, alpha, R8, RG8)
    CLS_FLOAT,          // half / float render targets
    CLS_DEPTH,
    CLS_OTHER,
    CLS_COUNT
};
inline const char* ClassName(int c) {
    static const char* n[CLS_COUNT] = {"RGBA8", "RGB8", "DXT", "drvDXT", "L/A8", "float", "depth", "other"};
    return (c >= 0 && c < CLS_COUNT) ? n[c] : "?";
}

/// Block size in bytes for a 4x4-block compressed format, 0 for anything else.
inline uint32_t BlockBytes(uint32_t fmt) {
    switch (fmt) {
    case kDxt1Rgb: case kDxt1Rgba: case kSrgbDxt1: case kSrgbDxt1A: case kRgtc1: return 8;
    case kDxt3: case kDxt5: case kSrgbDxt3: case kSrgbDxt5: case kRgtc2: return 16;
    default: return 0;
    }
}

/// The class of an internal format, and what the driver is likely to keep per texel
/// (in eighths of a byte, so DXT1's half byte is exact). The bytes are an estimate for
/// glTexImage2D; glCompressedTexImage2D passes its exact imageSize instead.
struct FormatInfo { int cls; uint32_t eighths; };
inline FormatInfo Describe(uint32_t fmt) {
    switch (fmt) {
    case 4: case kRgba: case kRgba8: case kBgra: case kSrgb8Alpha8: case kRgb10A2:
    case kR11G11B10F: case kRg16F: case kR32F:
        return {fmt == kR11G11B10F || fmt == kRg16F || fmt == kR32F ? CLS_FLOAT : CLS_RGBA8, 32};
    case 3: case kRgb: case kRgb8: case kBgr: case kSrgb8:
        return {CLS_RGB8, 32};
    case kRgb565: case kRgb5A1: case kRgba4:
        return {CLS_OTHER, 16};
    case 1: case kAlpha: case kLuminance: case kAlpha8: case kLuminance8: case kIntensity8:
    case kR8: case kRed:
        return {CLS_SMALL, 8};
    case 2: case kLuminanceAlpha: case kLuminance8Alpha8: case kRg8: case kR16F:
        return {fmt == kR16F ? CLS_FLOAT : CLS_SMALL, 16};
    case kRgba16F: case kRgb16F: case kRgba16: return {CLS_FLOAT, 64};
    case kRgba32F: case kRgb32F:              return {CLS_FLOAT, 128};
    case kDepthComponent: case kDepth16: case kDepth24: case kDepth32: case kDepth24Stencil8:
        return {CLS_DEPTH, fmt == kDepth16 ? 16u : 32u};
    case kCompressedRgb:  return {CLS_DRIVER_COMP, 4};   // usually DXT1
    case kCompressedRgba: return {CLS_DRIVER_COMP, 8};   // usually DXT5
    default:
        if (uint32_t b = BlockBytes(fmt)) return {CLS_DRIVER_COMP, b == 8 ? 4u : 8u};
        return {CLS_OTHER, 32};
    }
}

/// Bytes a level of `fmt` at w x h takes as glTexImage2D allocates it.
inline uint64_t LevelBytes(uint32_t fmt, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return 0;
    if (uint32_t b = BlockBytes(fmt))
        return (uint64_t)((w + 3) / 4) * (uint64_t)((h + 3) / 4) * b;
    return (uint64_t)w * (uint64_t)h * Describe(fmt).eighths / 8;
}

/// Bytes a glTexSubImage2D hands over, from its client format and type.
inline uint64_t TransferBytes(uint32_t format, uint32_t type, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) return 0;
    uint32_t comps;
    switch (format) {
    case kRgba: case kBgra: comps = 4; break;
    case kRgb: case kBgr: comps = 3; break;
    case kLuminanceAlpha: comps = 2; break;
    default: comps = 1; break;
    }
    uint32_t px;   // bytes a pixel
    switch (type) {
    case kUByte: case kByte: px = comps; break;
    case kUShort: case kShort: case kHalf: px = comps * 2; break;
    case kUInt: case kInt: case kFloat: px = comps * 4; break;
    default: px = 4; break;   // the packed types (8_8_8_8_REV, 5_6_5, ...) are one word a pixel
    }
    return (uint64_t)w * (uint64_t)h * px;
}

/// Which max-dimension bucket a texture falls in: <=64, 128, 256, 512, 1024, 2048, 4096+.
constexpr int kSizeBuckets = 7;
inline int SizeBucket(int32_t maxDim) {
    int b = 0;
    for (int32_t lim = 64; b < kSizeBuckets - 1 && maxDim > lim; lim *= 2) ++b;
    return b;
}
inline const char* SizeBucketName(int b) {
    static const char* n[kSizeBuckets] = {"<=64", "128", "256", "512", "1024", "2048", ">=4096"};
    return (b >= 0 && b < kSizeBuckets) ? n[b] : "?";
}

struct Bucket { int64_t count = 0; int64_t bytes = 0; };
struct FmtRow { uint32_t fmt = 0; int64_t count = 0; int64_t bytes = 0; };

/// Everything one snapshot reports. Plain values, copied out under the caller's lock.
struct Snapshot {
    Bucket   cls[CLS_COUNT];        // live textures by class (count = textures whose level 0 is that class)
    Bucket   size[kSizeBuckets];    // live textures by level-0 max dimension
    Bucket   texLive;               // all live textures
    Bucket   bufLive;               // all live buffer objects
    uint64_t texUploaded = 0;       // cumulative glTexImage2D + glCompressedTexImage2D bytes
    uint64_t subUploaded = 0;       // cumulative glTexSubImage2D bytes
    uint64_t bufUploaded = 0;       // cumulative glBufferData bytes
    uint64_t texFreed = 0;          // bytes released by glDeleteTextures / respecification
    uint64_t bufFreed = 0;
    uint64_t unboundUploads = 0;    // uploads to texture 0 / a target we do not track
    uint64_t untrackedDeletes = 0;  // deletes of names we never saw upload (loaded before the hooks)
    std::vector<FmtRow> fmts;       // live bytes by raw internal format, biggest first
};

class Tracker {
public:
    /// glTexImage2D / glCompressedTexImage2D on texture `id`. `face` is 0..5 (0 for 2D);
    /// `bytes` is what the level now takes. A level given again replaces what it held.
    void TexLevel(uint32_t id, int face, int level, uint32_t fmt, int32_t w, int32_t h, uint64_t bytes,
                  bool precompressed) {
        texUploaded_ += bytes;
        if (id == 0 || face < 0 || face > 5 || level < 0 || level > 31) { ++unbound_; return; }
        Tex& t = tex_[id];
        const bool fresh = t.levels.empty();
        if (fresh) ++texCount_;
        const int cls = precompressed ? (int)CLS_COMPRESSED : Describe(fmt).cls;
        const uint16_t key = (uint16_t)(face * 32 + level);
        Unfile(t);
        bool found = false;
        for (Level& l : t.levels)
            if (l.key == key) {   // the level is given again: what it held is released
                texFreed_ += l.bytes;
                AddFmt(l.fmt, l.key == 0 ? -1 : 0, -(int64_t)l.bytes);
                l = Level{key, fmt, cls, bytes};
                found = true;
                break;
            }
        if (!found) t.levels.push_back(Level{key, fmt, cls, bytes});
        AddFmt(fmt, key == 0 ? 1 : 0, (int64_t)bytes);
        // The texture is filed under its level 0 (any face); a texture given a smaller
        // level first is filed under that until its level 0 arrives.
        if (level == 0 || fresh) { t.cls = cls; t.fmt = fmt; }
        if (level == 0) t.maxDim = w > h ? w : h;
        else if (fresh) { const int64_t d = (int64_t)(w > h ? w : h) << (level > 16 ? 16 : level); t.maxDim = (int32_t)(d > 65536 ? 65536 : d); }
        File(t);
    }

    void DeleteTexture(uint32_t id) {
        if (id == 0) return;
        auto it = tex_.find(id);
        if (it == tex_.end()) { ++untrackedDeletes_; return; }
        Tex& t = it->second;
        Unfile(t);
        for (const Level& l : t.levels) {
            texFreed_ += l.bytes;
            AddFmt(l.fmt, l.key == 0 ? -1 : 0, -(int64_t)l.bytes);
        }
        --texCount_;
        tex_.erase(it);
    }

    void SubImage(uint64_t bytes) { subUploaded_ += bytes; }

    void BufferData(uint32_t id, int64_t size) {
        if (size < 0) size = 0;
        bufUploaded_ += (uint64_t)size;
        if (id == 0) { ++unbound_; return; }
        auto it = buf_.find(id);
        if (it != buf_.end()) { bufFreed_ += (uint64_t)it->second; bufBytes_ -= it->second; it->second = size; }
        else { buf_.emplace(id, size); }
        bufBytes_ += size;
    }

    void DeleteBuffer(uint32_t id) {
        auto it = buf_.find(id);
        if (it == buf_.end()) return;
        bufFreed_ += (uint64_t)it->second;
        bufBytes_ -= it->second;
        buf_.erase(it);
    }

    Snapshot Snap() const {
        Snapshot s;
        for (int i = 0; i < CLS_COUNT; ++i) s.cls[i] = cls_[i];
        for (int i = 0; i < kSizeBuckets; ++i) s.size[i] = size_[i];
        s.texLive = {texCount_, texBytes_};
        s.bufLive = {(int64_t)buf_.size(), bufBytes_};
        s.texUploaded = texUploaded_; s.subUploaded = subUploaded_; s.bufUploaded = bufUploaded_;
        s.texFreed = texFreed_; s.bufFreed = bufFreed_;
        s.unboundUploads = unbound_; s.untrackedDeletes = untrackedDeletes_;
        for (const auto& f : fmt_) if (f.second.count || f.second.bytes) s.fmts.push_back({f.first, f.second.count, f.second.bytes});
        // biggest first; a handful of rows, so insertion sort
        for (size_t i = 1; i < s.fmts.size(); ++i)
            for (size_t j = i; j > 0 && s.fmts[j].bytes > s.fmts[j - 1].bytes; --j) std::swap(s.fmts[j], s.fmts[j - 1]);
        return s;
    }

private:
    struct Level { uint16_t key; uint32_t fmt; int cls; uint64_t bytes; };
    struct Tex { std::vector<Level> levels; int32_t maxDim = 0; int cls = CLS_OTHER; uint32_t fmt = 0; };

    uint64_t Total(const Tex& t) const { uint64_t b = 0; for (const Level& l : t.levels) b += l.bytes; return b; }
    // Take a texture out of the class / size / total tallies, and put it back.
    void Unfile(const Tex& t) {
        const int64_t b = (int64_t)Total(t);
        if (t.levels.empty()) return;
        cls_[t.cls].count--; cls_[t.cls].bytes -= b;
        const int sb = SizeBucket(t.maxDim);
        size_[sb].count--; size_[sb].bytes -= b;
        texBytes_ -= b;
    }
    void File(const Tex& t) {
        const int64_t b = (int64_t)Total(t);
        cls_[t.cls].count++; cls_[t.cls].bytes += b;
        const int sb = SizeBucket(t.maxDim);
        size_[sb].count++; size_[sb].bytes += b;
        texBytes_ += b;
    }
    // fmt_[f].count: textures whose face-0 level 0 is f; .bytes: live bytes of levels in f.
    void AddFmt(uint32_t fmt, int64_t dCount, int64_t dBytes) {
        Bucket& f = fmt_[fmt];
        f.count += dCount; f.bytes += dBytes;
    }

    std::unordered_map<uint32_t, Tex>     tex_;
    std::unordered_map<uint32_t, int64_t> buf_;
    std::unordered_map<uint32_t, Bucket>  fmt_;   // keyed by raw internal format
    Bucket   cls_[CLS_COUNT];
    Bucket   size_[kSizeBuckets];
    int64_t  texCount_ = 0, texBytes_ = 0, bufBytes_ = 0;
    uint64_t texUploaded_ = 0, subUploaded_ = 0, bufUploaded_ = 0, texFreed_ = 0, bufFreed_ = 0;
    uint64_t unbound_ = 0, untrackedDeletes_ = 0;
};

inline double MB(int64_t b)  { return (double)b / (1024.0 * 1024.0); }
inline double MBu(uint64_t b) { return (double)b / (1024.0 * 1024.0); }

/// A name for the internal formats the log is likely to show; nullptr for the rest.
inline const char* FormatName(uint32_t f) {
    switch (f) {
    case 3: return "3"; case 4: return "4";
    case kRgb: return "GL_RGB"; case kRgba: return "GL_RGBA"; case kRgb8: return "RGB8";
    case kRgba8: return "RGBA8"; case kBgra: return "BGRA"; case kAlpha: return "ALPHA";
    case kLuminance: return "LUMINANCE"; case kLuminanceAlpha: return "LUM_ALPHA";
    case kAlpha8: return "ALPHA8"; case kLuminance8: return "LUM8"; case kR8: return "R8";
    case kRg8: return "RG8"; case kRgba16F: return "RGBA16F"; case kRgb16F: return "RGB16F";
    case kRgba32F: return "RGBA32F"; case kDepthComponent: return "DEPTH";
    case kDepth24: return "DEPTH24"; case kDepth24Stencil8: return "D24S8"; case kDepth16: return "DEPTH16";
    case kDxt1Rgb: return "DXT1"; case kDxt1Rgba: return "DXT1A"; case kDxt3: return "DXT3";
    case kDxt5: return "DXT5"; case kCompressedRgb: return "COMPRESSED_RGB";
    case kCompressedRgba: return "COMPRESSED_RGBA"; case kSrgb8Alpha8: return "SRGB8_A8";
    default: return nullptr;
    }
}

/// The summary line: live textures by class, buffers, cumulative traffic. `out` gets at
/// most `n` bytes, always terminated.
inline void FormatSummary(const Snapshot& s, char* out, size_t n) {
    if (!n) return;
    int k = std::snprintf(out, n, "tex live=%lld (%.1f MB) [", (long long)s.texLive.count, MB(s.texLive.bytes));
    for (int c = 0; c < CLS_COUNT && k > 0 && (size_t)k < n; ++c) {
        if (!s.cls[c].count && !s.cls[c].bytes) continue;
        k += std::snprintf(out + k, n - k, "%s%s %lld/%.1fMB", out[k - 1] == '[' ? "" : " ",
                           ClassName(c), (long long)s.cls[c].count, MB(s.cls[c].bytes));
    }
    if (k > 0 && (size_t)k < n)
        std::snprintf(out + k, n - k,
                      "] buf live=%lld (%.1f MB) | uploaded tex=%.1f MB sub=%.1f MB buf=%.1f MB | "
                      "freed tex=%.1f MB buf=%.1f MB | untracked deletes=%llu unbound uploads=%llu",
                      (long long)s.bufLive.count, MB(s.bufLive.bytes), MBu(s.texUploaded),
                      MBu(s.subUploaded), MBu(s.bufUploaded), MBu(s.texFreed), MBu(s.bufFreed),
                      (unsigned long long)s.untrackedDeletes, (unsigned long long)s.unboundUploads);
}

/// The detail line: live bytes by size bucket, then by raw internal format (top `maxFmts`).
inline void FormatDetail(const Snapshot& s, char* out, size_t n, size_t maxFmts = 10) {
    if (!n) return;
    int k = std::snprintf(out, n, "by size:");
    for (int b = 0; b < kSizeBuckets && k > 0 && (size_t)k < n; ++b)
        if (s.size[b].count)
            k += std::snprintf(out + k, n - k, " %s %lld/%.1fMB", SizeBucketName(b),
                               (long long)s.size[b].count, MB(s.size[b].bytes));
    if (k > 0 && (size_t)k < n) k += std::snprintf(out + k, n - k, " | by format:");
    for (size_t i = 0; i < s.fmts.size() && i < maxFmts && k > 0 && (size_t)k < n; ++i) {
        const char* nm = FormatName(s.fmts[i].fmt);
        if (nm) k += std::snprintf(out + k, n - k, " %s %lld/%.1fMB", nm, (long long)s.fmts[i].count, MB(s.fmts[i].bytes));
        else    k += std::snprintf(out + k, n - k, " 0x%X %lld/%.1fMB", s.fmts[i].fmt, (long long)s.fmts[i].count, MB(s.fmts[i].bytes));
    }
}

}  // namespace memdiag
