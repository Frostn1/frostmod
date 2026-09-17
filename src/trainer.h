// MX Bikes trainer records (.trn), and the garbage two of their fields carry.
//
// THE DEFECT, from the game's own code. Two functions write a trainer record, and they do not
// agree. The trainer-manager UI saver (0x1400E3BF0) copies three names into the record: the
// bike at +0x48, the series at +0x68 and the tyre at +0x88. The saver that runs when a session
// ends (0x140021010) copies the bike and then jumps straight to +0xA8 — the two `strcpy` loops
// for the series and the tyre are simply not there.
//
// The record it writes is a raw stack local (`sub rsp,0x7C8`, record at `rsp+0x40`) and the
// function contains no `memset` at all. So everything from the end of the bike name to +0xA7 —
// the name's own padding and both missing fields — is whatever was on the stack, written to
// disk as if it were text.
//
// WHAT IT COSTS. At the next track load the reader compares the series field against "" and,
// finding it non-empty, calls the series lookup (0x140004350). That returns 1 without writing
// its out-parameter when the name doesn't match, and the caller at 0x14002086F **never tests
// the return value**: it sign-extends an uninitialised stack dword, multiplies by 0x234, adds
// the array base, and hands three pointers into that wild address to `sprintf` as `%s`. The
// CRT's bounded `strnlen` (limit 0x7FFFFFFE) then walks until it leaves mapped memory.
//
// That is 39% of all MX Bikes crash reports in the largest public sample (17,231 of 43,841),
// and whether a given file faults depends on the loading process's memory layout, which is why
// the same trainer loads on one PC and kills the game on another.
//
// WHAT WE DO. Nothing clever: make the two fields say what the game itself writes when it has
// nothing to put there. The legacy loader zeroes both (0x140020384, 0x14002038B), so an empty
// series is a value the game already handles — it takes the "empty" branch and never calls the
// lookup at all.
//
// A field is only cleared when it is not a string: no NUL inside its own 0x20 bytes, or bytes
// that are not printable text. A trainer saved through the UI carries real values here and
// keeps them. Clearing those would be trading one silent wrong behaviour for another.
#pragma once

#include <cstddef>
#include <cstdint>

namespace trainer {

/// The record the game reads and writes, content version 6. Not the file size: the GHS layer
/// puts a 26-byte header in front of it, which is why the bike name sits at file offset 0x62.
constexpr size_t kRecordSize = 0x57C;
/// Every name field in the record is this wide, NUL included.
constexpr size_t kNameLen = 0x20;
constexpr size_t kBikeAt   = 0x48;
constexpr size_t kSeriesAt = 0x68;
constexpr size_t kTyreAt   = 0x88;
/// What the GHS layer is asked for when it carries one of these.
constexpr int kVersion = 6;

/// Whether `n` bytes read as a name the game could have written: NUL-terminated inside the
/// field, and printable up to that NUL. An empty field passes — that is the game's own "none".
inline bool LooksLikeName(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 0) return true;
        // Anything outside plain printable ASCII is not a name the game wrote. A stack address
        // read as text is almost always caught right here.
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    }
    return false;  // ran to the end of the field with no terminator
}

/// Clear a name field that isn't one. Returns true when it had to.
inline bool ClearIfNotName(uint8_t* p, size_t n) {
    if (LooksLikeName(p, n)) return false;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
    return true;
}

/// Make a trainer record safe to load, in place. Returns how many fields had to be cleared.
///
/// `size` is checked rather than trusted: the legacy 0x53C layout reaches the same reader and
/// the game already zeroes these fields on that path, so it is left alone.
inline int Sanitise(void* record, size_t size) {
    if (!record || size != kRecordSize) return 0;
    uint8_t* r = static_cast<uint8_t*>(record);
    int fixed = 0;
    // The bike name is what the record is found by, so it is never cleared — only forced to
    // end inside its own field, since the reader compares it against the bike list.
    if (!LooksLikeName(r + kBikeAt, kNameLen)) {
        r[kBikeAt + kNameLen - 1] = 0;
        ++fixed;
    }
    if (ClearIfNotName(r + kSeriesAt, kNameLen)) ++fixed;
    if (ClearIfNotName(r + kTyreAt, kNameLen)) ++fixed;
    return fixed;
}

}  // namespace trainer
