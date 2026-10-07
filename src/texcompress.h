// texcompress=1 (frostmod_radar.cfg): store the game's big uncompressed textures as DXT.
//
// MX Bikes hands the GL driver most of its track and bike textures as raw 8-bit RGB/RGBA,
// and the NVIDIA driver keeps a system-RAM copy of each (memdiag=1 shows it: a stock track
// is gigabytes of RGBA8). Asking for a DXT internal format on the same glTexImage2D call
// makes the driver compress on upload, so that copy is 4x (DXT5) or 8x (DXT1) smaller.
//
// This header is the decision only - which uploads are swapped, to what, and what a later
// level or sub-image update of a swapped texture must do - pure C++ with no GL or Win32 so
// it runs in CI (tests/texcompress_test.cpp). The GL hooks live in frostmod.cpp (MEMDIAG
// section). Not thread-safe by itself: the caller holds one lock around a Table.
#pragma once

#include <cstdint>
#include <unordered_map>

namespace texcompress {

enum : uint32_t {
    kTex2D = 0x0DE1,
    kRgb = 0x1907, kRgba = 0x1908, kBgr = 0x80E0, kBgra = 0x80E1,
    kRgb8 = 0x8051, kRgba8 = 0x8058,
    kUByte = 0x1401,
    kDxt1Rgb = 0x83F0, kDxt5 = 0x83F3,
};

/// Why an upload was left as the game gave it (or 0 = swapped). Counted in the log.
enum Skip : int {
    SK_NONE = 0,
    SK_TARGET,      // not GL_TEXTURE_2D (cube maps, proxies, rectangles)
    SK_FORMAT,      // not 8-bit RGB/RGBA (alpha/luminance, float, depth, already compressed)
    SK_NODATA,      // allocated with no pixels: a render target or a texture filled later
    SK_SMALL,       // under the size threshold
    SK_NPOT,        // not a power of two on a side: screen-sized UI art, video frames
    SK_CALLER,      // uploaded by a plugin (HUD fonts and the like), not by mxbikes.exe
    SK_SUBIMAGE,    // this texture name was updated with glTexSubImage2D before
    SK_BORDER,      // a texture border
    SK_COUNT
};
inline const char* SkipName(int s) {
    static const char* n[SK_COUNT] = {"swapped", "target", "format", "nodata", "small", "npot",
                                      "plugin", "subimage", "border"};
    return (s >= 0 && s < SK_COUNT) ? n[s] : "?";
}

inline bool IsPow2(int32_t v) { return v > 0 && (v & (v - 1)) == 0; }

/// An uncompressed 8-bit internal format this replaces, and whether it has alpha.
inline bool Replaceable(uint32_t ifmt, bool* alpha) {
    switch (ifmt) {
    case 3: case kRgb: case kRgb8:   *alpha = false; return true;
    case 4: case kRgba: case kRgba8: *alpha = true;  return true;
    default: return false;
    }
}
inline bool ClientFormatOk(uint32_t format, uint32_t type) {
    return type == kUByte && (format == kRgb || format == kRgba || format == kBgr || format == kBgra);
}

/// True when every alpha byte of a tightly packed 4-byte-a-pixel level is 255, so DXT1
/// (8:1) loses nothing DXT5 (4:1) would keep. `alphaOffset` is 3 for RGBA and BGRA.
inline bool AllOpaque(const uint8_t* px, int32_t w, int32_t h) {
    const uint64_t n = (uint64_t)w * (uint64_t)h;
    for (uint64_t i = 0; i < n; ++i)
        if (px[i * 4 + 3] != 0xFF) return false;
    return true;
}

struct Request {
    uint32_t target, ifmt, format, type;
    int32_t  level, w, h, border;
    bool     hasPixels;      // pixels non-null (or a bound unpack buffer)
    bool     fromGame;       // the call came from mxbikes.exe
    bool     tightRgba;      // client data is 4 bytes a pixel and tightly packed, so it can be scanned
    const uint8_t* px;       // the pixels when tightRgba, else null
};

struct Decision { uint32_t ifmt; int skip; };

/// What one texture name has been made into. A swapped texture keeps its compressed format
/// for every level so its mip chain stays one format (a mixed chain is incomplete and draws
/// as nothing). `levels` is a bit per level this replaced, for undoing it.
struct Entry {
    uint32_t dxt = 0;        // 0: not swapped
    uint32_t origIfmt = 0;
    uint32_t levels = 0;
    int32_t  w0 = 0, h0 = 0;
    bool     subbed = false; // had a glTexSubImage2D: never swapped again
};

class Table {
public:
    explicit Table(int32_t minDim = 256) : minDim_(minDim) {}
    void SetMinDim(int32_t d) { minDim_ = d < 4 ? 4 : d; }
    int32_t MinDim() const { return minDim_; }

    /// The internal format to use for glTexImage2D on texture `id` (0 = unknown binding).
    Decision Choose(uint32_t id, const Request& r) {
        if (r.target != kTex2D) return {r.ifmt, SK_TARGET};
        if (id == 0) return {r.ifmt, SK_TARGET};
        if (r.border != 0) return {r.ifmt, SK_BORDER};
        bool alpha = false;
        auto it = tex_.find(id);
        Entry* e = it == tex_.end() ? nullptr : &it->second;
        if (e && e->subbed) return {r.ifmt, SK_SUBIMAGE};
        if (r.level > 0) {
            // A lower level follows level 0's decision, whatever its size.
            if (e && e->dxt && r.level < 32 && Replaceable(r.ifmt, &alpha)) {
                e->levels |= 1u << r.level;
                return {e->dxt, SK_NONE};
            }
            return {r.ifmt, e && e->dxt ? SK_FORMAT : SK_SMALL};
        }
        // Level 0 (re)specified: a fresh decision for the whole texture.
        if (e) { e->dxt = 0; e->levels = 0; }
        if (!Replaceable(r.ifmt, &alpha) || !ClientFormatOk(r.format, r.type)) return {r.ifmt, SK_FORMAT};
        if (!r.fromGame) return {r.ifmt, SK_CALLER};
        if (!r.hasPixels) return {r.ifmt, SK_NODATA};
        if (r.w < minDim_ || r.h < minDim_) return {r.ifmt, SK_SMALL};
        if (!IsPow2(r.w) || !IsPow2(r.h)) return {r.ifmt, SK_NPOT};
        if (r.format == kRgb || r.format == kBgr) alpha = false;   // no alpha in the data: opaque
        else if (alpha && r.tightRgba && r.px && AllOpaque(r.px, r.w, r.h)) alpha = false;
        Entry& n = tex_[id];
        n.dxt = alpha ? kDxt5 : kDxt1Rgb;
        n.origIfmt = r.ifmt;
        n.levels = 1;
        n.w0 = r.w; n.h0 = r.h;
        return {n.dxt, SK_NONE};
    }

    /// The driver refused the swapped upload: take it back (the caller re-uploads as given).
    void Revert(uint32_t id) {
        auto it = tex_.find(id);
        if (it != tex_.end()) { it->second.dxt = 0; it->second.levels = 0; }
    }

    /// glTexSubImage2D on `id`. Returns the entry to undo when it was swapped (the caller
    /// re-specifies those levels uncompressed before the update), else null. Either way the
    /// name is marked so it is never swapped again while it lives.
    const Entry* SubImage(uint32_t id) {
        if (id == 0) return nullptr;
        Entry& e = tex_[id];
        const bool was = e.dxt != 0;
        e.subbed = true;
        if (!was) return nullptr;
        undo_ = e;
        e.dxt = 0; e.levels = 0;
        return &undo_;
    }

    void Delete(uint32_t id) { tex_.erase(id); }
    const Entry* Find(uint32_t id) const { auto it = tex_.find(id); return it == tex_.end() ? nullptr : &it->second; }

private:
    int32_t minDim_;
    std::unordered_map<uint32_t, Entry> tex_;
    Entry undo_;
};

/// Bytes of a level of `w` x `h` as uncompressed 4-byte texels, and as `dxt`.
inline uint64_t RawBytes(int32_t w, int32_t h) { return (uint64_t)w * (uint64_t)h * 4; }
inline uint64_t DxtBytes(uint32_t dxt, int32_t w, int32_t h) {
    return (uint64_t)((w + 3) / 4) * (uint64_t)((h + 3) / 4) * (dxt == kDxt5 ? 16u : 8u);
}

}  // namespace texcompress
