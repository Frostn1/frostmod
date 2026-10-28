// shaderup.h - upgraded material shading for MX Bikes' own GLSL (shaders=1, off by default).
//
// WHAT THE GAME DOES. MX Bikes builds every material from a fixed set of GLSL 1.10 sources
// that live as plain strings in mxbikes.exe. One routine (0x22B100) makes each program:
// glCreateShaderObjectARB, glShaderSourceARB(shader, 1, &src, NULL), glCompileShaderARB,
// attach, glLinkProgramARB, and then asks glGetUniformLocationARB for every uniform and keeps
// the answers. It never checks a compile or link status. The lighting is the fixed-function
// model written out by hand: Phong specular `pow(max(dot(reflect(L, N), E), 0), shininess)`
// with no N.L term and no normalisation, the normal map decoded with `(c - 0.5) * 2.0` and
// normalised, the cubemap reflection weighted by `RefFactorMin + (RefFactor - RefFactorMin) *
// pow(1 - |E.N|, FresExp)`, and the projected shadow texture read with one tap.
//
// WHAT THIS CHANGES. A source the game hands glShaderSourceARB is hashed (FNV-1a 64 over the
// exact bytes). Only a hash in kKnown[] is touched; anything else - another build, a mod,
// another title - goes through as it came. A known source gets a short preamble (#defines
// plus the helpers it needs) and a few statements rewritten in place:
//
//   kBump     `BumpNorm = normalize(BumpNorm);`  ->  fm_bump(): xy scaled by shaderbump=,
//             renormalised, and a zero-length texel (a black mip) stays a flat normal
//             instead of becoming NaN.
//   kSpec     the Phong line -> fm_spec(): Blinn-Phong with the half vector, exponent 4x the
//             material's Phong shininess (the usual match for the lobe width), the (n+8)/8
//             energy normalisation taken relative to n = 64 so a mid-shiny material keeps its
//             brightness, capped at 1.5x, times N.L so no highlight shows on the side facing
//             away from the sun. Strength shaderspec=. Above 0.5 the term rolls off on a soft
//             shoulder toward 0.95, so a tight, shiny highlight fades out instead of clipping
//             to a flat white disc with a hard edge.
//   kFresnel  the reflection weight -> fm_fresnel(): the same RefFactorMin/RefFactor range,
//             with the curve moved from the artist's pow(1-c, FresExp) toward Schlick's
//             (1-c)^5 by shaderfresnel= (default 80; 100 = pure Schlick). Head-on surfaces
//             reflect less and grazing ones still reach RefFactor, so edges read brighter
//             than the middle.
//   kShadow   every `texture2D(shadowMap, ` -> fm_shadow(): four rotated-grid taps around
//             the point, shadersoft= percent of one texel at 2048 apart. Three extra
//             fetches, nothing else.
//   kTone     the last `gl_FragColor = color;` -> a soft shoulder above 0.8 so the brighter
//             normalised highlights roll off instead of clipping. The pipeline is LDR
//             (RGBA8, no exposure stage), so this is a highlight shoulder, not a tonemapper,
//             and it is off unless shadertone=1.
//
// Every rewrite is an exact-text match; kKnown[] says which ones each source must take, and a
// source that takes anything else is refused whole. No game shader text is stored here: the
// table is hashes, and the matched fragments are single statements.
//
// No Win32 or GL here, so tests/shaderup_test.cpp runs anywhere. The hooks, the live A/B
// relink and the colour grade pass are in frostmod.cpp (SHADERS section).
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace shaderup {

// ---- the hash ------------------------------------------------------------------------------
inline uint64_t Fnv1a64(const char* s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)s[i]; h *= 0x100000001b3ull; }
    return h;
}
inline uint64_t Hash(const std::string& s) { return Fnv1a64(s.data(), s.size()); }

// ---- the rules -----------------------------------------------------------------------------
enum Rule : uint32_t {
    kBump    = 1u << 0,
    kSpec    = 1u << 1,
    kFresnel = 1u << 2,
    kShadow  = 1u << 3,
    kTone    = 1u << 4,
};
constexpr uint32_t kAllRules = kBump | kSpec | kFresnel | kShadow | kTone;

// ---- the settings (frostmod_radar.cfg; integers, like every other key there) -----------------
struct Config {
    bool on        = false;   // shaders=1: the master switch. Off by default.
    int  bump      = 100;     // shaderbump=   normal-map strength, percent (0..300)
    int  spec      = 100;     // shaderspec=   specular strength, percent (0..300)
    // shaderfresnel= artist curve -> Schlick, percent (0..100). 80, not 100: at 100 the
    // artist's FresExp drops out of the program, the driver hands its location to another
    // uniform, and the live A/B can no longer relink those materials (it then leaves them as
    // the game linked them). 80 keeps every uniform alive and is within a hair of Schlick.
    int  fresnel   = 80;
    int  soft      = 100;     // shadersoft=   shadow tap spread, percent of a 2048 texel (0..400); 0 = one tap
    bool tone      = false;   // shadertone=1  highlight shoulder
    bool grade     = false;   // grade=1       colour grade in the final pass (needs shaders=1)
    int  bright    = 100;     // gradebright=  percent (50..150)
    int  contrast  = 100;     // gradecontrast= percent (50..150)
    int  sat       = 100;     // gradesat=     percent (0..200)
};

inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The rules a config asks for. A rule turned all the way down (fresnel=0, soft=0) is dropped
// rather than applied as a no-op, so that part stays the game's own code.
inline uint32_t EnabledRules(const Config& c) {
    // Specular is the reason the feature exists, and the normal rewrite (renormalise + the
    // zero-length guard) is wanted even at 100%, so both are always on with shaders=1.
    uint32_t r = kSpec | kBump;
    if (c.fresnel > 0) r |= kFresnel;
    if (c.soft > 0) r |= kShadow;
    if (c.tone) r |= kTone;
    return r;
}

// One settings line. Returns true if it was one of ours (consumed), false otherwise.
inline bool ParseCfgLine(const char* line, Config& c) {
    if (!line) return false;
    struct K { const char* k; int lo, hi; int* i; bool* b; };
    const K keys[] = {
        {"shaders=",       0, 1,   nullptr,     &c.on},
        {"shaderbump=",    0, 300, &c.bump,     nullptr},
        {"shaderspec=",    0, 300, &c.spec,     nullptr},
        {"shaderfresnel=", 0, 100, &c.fresnel,  nullptr},
        {"shadersoft=",    0, 400, &c.soft,     nullptr},
        {"shadertone=",    0, 1,   nullptr,     &c.tone},
        {"gradebright=",   50, 150, &c.bright,  nullptr},
        {"gradecontrast=", 50, 150, &c.contrast, nullptr},
        {"gradesat=",      0, 200, &c.sat,      nullptr},
        {"grade=",         0, 1,   nullptr,     &c.grade},
    };
    for (const K& k : keys) {
        const size_t n = std::strlen(k.k);
        if (std::strncmp(line, k.k, n) != 0) continue;
        const char* v = line + n;
        char* end = nullptr;
        const long x = std::strtol(v, &end, 10);
        if (end == v) return true;            // ours, but not a number: keep the default
        if (k.b) *k.b = (x == 1);             // only exactly 1 turns a switch on
        else *k.i = Clamp((int)x, k.lo, k.hi);
        return true;
    }
    return false;
}

// The lines to save: only what differs from the default, so a file without them is the default.
inline std::string CfgLines(const Config& c) {
    const Config d;
    std::string s;
    char b[48];
    auto add = [&](const char* k, int v) { std::snprintf(b, sizeof(b), "%s%d\n", k, v); s += b; };
    if (c.on)                     add("shaders=", 1);
    if (c.bump != d.bump)         add("shaderbump=", c.bump);
    if (c.spec != d.spec)         add("shaderspec=", c.spec);
    if (c.fresnel != d.fresnel)   add("shaderfresnel=", c.fresnel);
    if (c.soft != d.soft)         add("shadersoft=", c.soft);
    if (c.tone)                   add("shadertone=", 1);
    if (c.grade)                  add("grade=", 1);
    if (c.bright != d.bright)     add("gradebright=", c.bright);
    if (c.contrast != d.contrast) add("gradecontrast=", c.contrast);
    if (c.sat != d.sat)           add("gradesat=", c.sat);
    return s;
}

// ---- the known sources ---------------------------------------------------------------------
// One row per fragment shader in mxbikes.exe that takes at least one rule: the hash and length
// of the exact source the game passes, and the rules it must take (all of them, or the source
// is refused). Made by tools/shaderlab (`shaderlab --table <mxbikes.exe>`), from the same
// Patch() below, so the table and the rewrite cannot disagree.
struct Known {
    uint64_t    hash;
    uint32_t    len;
    uint32_t    rules;
    const char* label;
};
// clang-format off
inline const Known* KnownTable(size_t* count) {
    static const Known k[] = {
#include "shaderup_table.inc"
    };
    *count = sizeof(k) / sizeof(k[0]);
    return k;
}
// clang-format on

inline const Known* Find(uint64_t hash, size_t len) {
    size_t n = 0;
    const Known* k = KnownTable(&n);
    for (size_t i = 0; i < n; ++i)
        if (k[i].hash == hash && k[i].len == len) return &k[i];
    return nullptr;
}

// ---- the rewrite ---------------------------------------------------------------------------
// "1.25" from 125, without printf's locale (a game that set a comma locale would otherwise get
// "1,25" in GLSL).
inline std::string Fixed(int percent) {
    const bool neg = percent < 0;
    const int a = neg ? -percent : percent;
    std::string s = neg ? "-" : "";
    s += std::to_string(a / 100);
    s += '.';
    const int f = a % 100;
    s += (char)('0' + f / 10);
    s += (char)('0' + f % 10);
    return s;
}

// The helpers, one per rule, written for GLSL 1.10 (samplers as `in` parameters are legal there).
inline const char* HelperText(Rule r) {
    switch (r) {
    case kBump:
        return "vec3 fm_bump(vec3 n){n.xy*=FM_BUMP;float l=dot(n,n);"
               "return l>1e-8?n*inversesqrt(l):vec3(0.0,0.0,1.0);}\n";
    case kSpec:
        // L: to the light. E: eye -> surface (the game's EyeDir), so the view vector is -E.
        return "float fm_spec(vec3 L,vec3 N,vec3 E,float sh){"
               "vec3 H=normalize(L-normalize(E));"
               "float n=clamp(sh*4.0,1.0,2048.0);"
               "float k=min((n+8.0)/72.0,1.5);"
               "float s=pow(max(dot(N,H),0.0),n)*k*max(dot(N,L),0.0)*FM_SPEC;"
               "return s<0.5?s:0.5+0.45*(1.0-exp((0.5-s)/0.45));}\n";
    case kFresnel:
        return "float fm_fresnel(float f0,float f90,float e,float edn){"
               "float c=1.0-clamp(abs(edn),0.0,1.0);"
               "float c2=c*c;"
               "return f0+(f90-f0)*mix(pow(c,e),c2*c2*c,FM_FRESNEL);}\n";
    case kShadow:
        return "vec4 fm_shadow(sampler2D m,vec2 uv){"
               "vec2 o=vec2(FM_SOFT/2048.0);"
               "return 0.25*(texture2D(m,uv+o*vec2(-0.4,-1.2))+texture2D(m,uv+o*vec2(1.2,-0.4))"
               "+texture2D(m,uv+o*vec2(0.4,1.2))+texture2D(m,uv+o*vec2(-1.2,0.4)));}\n";
    case kTone:
        return "vec4 fm_tone(vec4 c){vec3 x=c.rgb;"
               "vec3 o=max(x-0.8,0.0);"
               "return vec4(min(x,0.8)+0.2*(1.0-exp(-o*5.0)),c.a);}\n";
    }
    return "";
}

// Replace every `from` with `to`; returns how many.
inline int ReplaceAll(std::string& s, const char* from, const char* to) {
    const size_t fl = std::strlen(from), tl = std::strlen(to);
    int n = 0;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + tl)) {
        s.replace(p, fl, to);
        ++n;
    }
    return n;
}

// The Phong line, with whatever normal and eye expression the shader uses.
// `specular = pow( max( dot(reflect(LightDirNorm, N), E) , 0.0), gl_FrontMaterial.shininess );`
inline int RewriteSpec(std::string& s) {
    static const char kHead[] = "specular = pow( max( dot(reflect(LightDirNorm, ";
    static const char kTail[] = ") , 0.0), gl_FrontMaterial.shininess );";
    int n = 0;
    size_t p = 0;
    while ((p = s.find(kHead, p)) != std::string::npos) {
        const size_t a = p + sizeof(kHead) - 1;
        const size_t comma = s.find("), ", a);          // reflect( , N) closes after the normal
        const size_t tail = s.find(kTail, a);
        if (comma == std::string::npos || tail == std::string::npos || comma > tail) return -1;
        const std::string normal = s.substr(a, comma - a);
        const std::string eye = s.substr(comma + 3, tail - (comma + 3));
        // The normal is one identifier; the eye is an identifier or normalize(identifier).
        for (char ch : normal)
            if (!(isalnum((unsigned char)ch) || ch == '_')) return -1;
        if (normal.empty() || eye.empty() || eye.find(';') != std::string::npos) return -1;
        int depth = 0;
        for (char ch : eye) {
            depth += ch == '(' ? 1 : (ch == ')' ? -1 : 0);
            if (depth < 0) return -1;
        }
        if (depth != 0) return -1;
        const std::string repl = "specular = fm_spec(LightDirNorm, " + normal + ", " + eye +
                                 ", gl_FrontMaterial.shininess);";
        s.replace(p, tail + sizeof(kTail) - 1 - p, repl);
        p += repl.size();
        ++n;
    }
    return n;
}

// The last `gl_FragColor = color;` (the only one, in every source that has kTone).
inline int RewriteTone(std::string& s) {
    static const char kFrom[] = "gl_FragColor = color;";
    const size_t p = s.rfind(kFrom);
    if (p == std::string::npos) return 0;
    s.replace(p, sizeof(kFrom) - 1, "gl_FragColor = fm_tone(color);");
    return 1;
}

// Which rules this source can take, and the result of taking `want` of them.
struct Result {
    bool        ok = false;
    uint32_t    applied = 0;   // the rules that matched
    std::string out;           // the upgraded source (empty unless ok)
    const char* why = "";      // why not, when !ok
};

// Rewrite `src` with the rules in `want` (and only those), the strengths from `c`.
// ok only if every rule in `want` matched at least once, the source starts with a #version
// line, and no rule was half-applied.
inline Result Apply(const std::string& src, uint32_t want, const Config& c) {
    Result r;
    if (want == 0) { r.why = "no rules asked for"; return r; }
    if (src.compare(0, 9, "#version ") != 0) { r.why = "no #version line"; return r; }
    const size_t eol = src.find('\n');
    if (eol == std::string::npos) { r.why = "no newline after #version"; return r; }
    if (src.find("fm_") != std::string::npos) { r.why = "already upgraded"; return r; }

    std::string body = src.substr(eol + 1);
    uint32_t got = 0;
    if (want & kBump) {
        if (ReplaceAll(body, "BumpNorm = normalize(BumpNorm);", "BumpNorm = fm_bump(BumpNorm);") > 0)
            got |= kBump;
    }
    if (want & kSpec) {
        const int n = RewriteSpec(body);
        if (n < 0) { r.why = "a specular line did not parse"; return r; }
        if (n > 0) got |= kSpec;
    }
    if (want & kFresnel) {
        if (ReplaceAll(body, "RefFactorMin + (RefFactor - RefFactorMin) * pow((1.0 - abs(edn)), FresExp)",
                       "fm_fresnel(RefFactorMin, RefFactor, FresExp, edn)") > 0)
            got |= kFresnel;
    }
    if (want & kShadow) {
        if (ReplaceAll(body, "texture2D(shadowMap, ", "fm_shadow(shadowMap, ") > 0) got |= kShadow;
    }
    if (want & kTone) {
        if (RewriteTone(body) > 0) got |= kTone;
    }
    r.applied = got;
    if (got != want) { r.why = "a rule did not match"; return r; }

    std::string pre = src.substr(0, eol + 1);
    pre += "#define FM_BUMP " + Fixed(c.bump) + "\n";
    pre += "#define FM_SPEC " + Fixed(c.spec) + "\n";
    pre += "#define FM_FRESNEL " + Fixed(c.fresnel) + "\n";
    pre += "#define FM_SOFT " + Fixed(c.soft) + "\n";
    for (Rule rule : {kBump, kSpec, kFresnel, kShadow, kTone})
        if (got & rule) pre += HelperText(rule);
    r.out = pre + body;
    r.ok = true;
    return r;
}

// Which rules a source could take at all (for the table generator and the tests).
inline uint32_t Probe(const std::string& src) {
    Config c;
    uint32_t can = 0;
    for (Rule rule : {kBump, kSpec, kFresnel, kShadow, kTone}) {
        Result r = Apply(src, rule, c);
        if (r.ok) can |= rule;
    }
    // The shoulder only goes where the brighter highlight comes from.
    if (!(can & kSpec)) can &= ~(uint32_t)kTone;
    return can;
}

// The upgrade for a source the game is about to compile. `k` is its kKnown[] row. The rules
// taken are the row's, less the ones this config turns off.
inline Result Upgrade(const std::string& src, const Known& k, const Config& c) {
    const uint32_t want = k.rules & EnabledRules(c);
    if (src.size() != k.len || Hash(src) != k.hash) {
        Result r; r.why = "hash mismatch"; return r;
    }
    return Apply(src, want, c);
}

// ---- the live A/B: does a relinked program still answer to the locations the game kept? ------
// The game asks for every uniform's location once, after its own link, and never again. A
// relink (to switch the upgrade on or off while riding) may only go ahead if every uniform the
// new link has sits where the game believes it does, and no location the game writes to now
// belongs to a different uniform. A uniform the new link dropped (an unused one) is fine: the
// game's writes to its old location then go nowhere, as long as nobody else lives there.
struct Loc {
    std::string name;
    int         loc;
};
inline bool LocationsCompatible(const std::vector<Loc>& game, const std::vector<Loc>& now,
                                std::string* why = nullptr) {
    for (const Loc& n : now) {
        bool found = false;
        for (const Loc& g : game) {
            if (g.name == n.name) {
                found = true;
                if (g.loc != n.loc) {
                    if (why) *why = n.name + " moved";
                    return false;
                }
            } else if (g.loc == n.loc && g.loc >= 0) {
                if (why) *why = n.name + " took " + g.name + "'s location";
                return false;
            }
        }
        (void)found;   // a uniform the game never saw is fine if it collides with nothing
    }
    return true;
}

// GL uniform types the relink snapshot can carry: component count, int or float, matrix.
struct UType {
    unsigned type;
    int      comps;
    bool     isInt;
    bool     isMatrix;
};
inline bool UniformType(unsigned t, UType* out) {
    static const UType k[] = {
        {0x1406, 1, false, false},  {0x8B50, 2, false, false}, {0x8B51, 3, false, false},
        {0x8B52, 4, false, false},  {0x1404, 1, true, false},  {0x8B53, 2, true, false},
        {0x8B54, 3, true, false},   {0x8B55, 4, true, false},  {0x8B56, 1, true, false},
        {0x8B57, 2, true, false},   {0x8B58, 3, true, false},  {0x8B59, 4, true, false},
        {0x8B5A, 4, false, true},   {0x8B5B, 9, false, true},  {0x8B5C, 16, false, true},
        {0x8B5D, 1, true, false},   {0x8B5E, 1, true, false},  {0x8B5F, 1, true, false},
        {0x8B60, 1, true, false},   {0x8B61, 1, true, false},  {0x8B62, 1, true, false},
    };
    for (const UType& u : k)
        if (u.type == t) { if (out) *out = u; return true; }
    return false;
}

// ---- the colour grade (final pass, drawn by FrostMod after the game's frame) ------------------
inline const char* GradeFragmentSource() {
    return "#version 110\n"
           "uniform sampler2D fm_frame;\n"
           "uniform vec3 fm_grade;\n"   // brightness, contrast, saturation
           "void main(){\n"
           "vec3 c=texture2D(fm_frame,gl_TexCoord[0].st).rgb*fm_grade.x;\n"
           "c=(c-0.5)*fm_grade.y+0.5;\n"
           "float l=dot(c,vec3(0.2126,0.7152,0.0722));\n"
           "c=mix(vec3(l),c,fm_grade.z);\n"
           "gl_FragColor=vec4(clamp(c,0.0,1.0),1.0);\n"
           "}\n";
}
inline bool GradeIsIdentity(const Config& c) {
    return c.bright == 100 && c.contrast == 100 && c.sat == 100;
}

}  // namespace shaderup
