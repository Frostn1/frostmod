// shaderlab - offline checks for src/shaderup.h against the game's own shader strings.
//
//   shaderlab --table  <mxbikes.exe>             print src/shaderup_table.inc
//   shaderlab --check  <mxbikes.exe> <out dir>   compile + link every source, original and
//                                                upgraded; replay the live A/B relink; render
//                                                original vs upgraded on a test sphere
//   shaderlab --dll    <dir> <mxbikes.exe> early|late
//                                                load the real frostmod.dll from <dir> (with a
//                                                frostmod_radar.cfg saying shaders=1) into this
//                                                process, before or after the "game" builds its
//                                                programs, and check every program comes out
//                                                upgraded with its uniforms intact
//
// It only READS the exe you point it at (the installed one is fine: the shader sources are
// plain strings in it) and keeps nothing from it. The GL work runs in a legacy (compatibility)
// context on a window that is created hidden and never shown, rendering into an FBO, so it
// puts nothing on the screen.
//
// Build: cmake -B build -A x64 -DFROSTMOD_SHADERLAB=ON && cmake --build build --config Release
// --target shaderlab.
#include <windows.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/shaderup.h"
#include "../../src/shaderswitch.h"

// ---- GL entry points (ARB shader objects, like the game) ------------------------------------
#define GLF(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; static PFN_##name name = nullptr;
GLF(GLuint, glCreateShaderObjectARB, (GLenum))
GLF(GLuint, glCreateProgramObjectARB, (void))
GLF(void, glShaderSourceARB, (GLuint, GLsizei, const char* const*, const GLint*))
GLF(void, glCompileShaderARB, (GLuint))
GLF(void, glAttachObjectARB, (GLuint, GLuint))
GLF(void, glLinkProgramARB, (GLuint))
GLF(void, glUseProgramObjectARB, (GLuint))
GLF(void, glDeleteObjectARB, (GLuint))
GLF(void, glGetObjectParameterivARB, (GLuint, GLenum, GLint*))
GLF(void, glGetInfoLogARB, (GLuint, GLsizei, GLsizei*, char*))
GLF(GLint, glGetUniformLocationARB, (GLuint, const char*))
GLF(void, glGetActiveUniformARB, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*))
GLF(GLint, glGetAttribLocationARB, (GLuint, const char*))
GLF(void, glUniform1iARB, (GLint, GLint))
GLF(void, glUniform1fARB, (GLint, GLfloat))
GLF(void, glUniform2fARB, (GLint, GLfloat, GLfloat))
GLF(void, glUniform3fARB, (GLint, GLfloat, GLfloat, GLfloat))
GLF(void, glUniform4fARB, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glUniformMatrix4fvARB, (GLint, GLsizei, GLboolean, const GLfloat*))
GLF(void, glUniformMatrix3fvARB, (GLint, GLsizei, GLboolean, const GLfloat*))
GLF(void, glVertexAttrib4fARB, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glActiveTextureARB, (GLenum))
GLF(void, glMultiTexCoord2fARB, (GLenum, GLfloat, GLfloat))
GLF(void, glGenFramebuffersEXT, (GLsizei, GLuint*))
GLF(void, glBindFramebufferEXT, (GLenum, GLuint))
GLF(void, glFramebufferTexture2DEXT, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(void, glGenRenderbuffersEXT, (GLsizei, GLuint*))
GLF(void, glBindRenderbufferEXT, (GLenum, GLuint))
GLF(void, glRenderbufferStorageEXT, (GLenum, GLenum, GLsizei, GLsizei))
GLF(void, glFramebufferRenderbufferEXT, (GLenum, GLenum, GLenum, GLuint))
GLF(GLenum, glCheckFramebufferStatusEXT, (GLenum))

static bool LoadGl() {
#define L(name) name = (PFN_##name)wglGetProcAddress(#name); if (!name) { std::printf("missing %s\n", #name); return false; }
    L(glCreateShaderObjectARB) L(glCreateProgramObjectARB) L(glShaderSourceARB) L(glCompileShaderARB)
    L(glAttachObjectARB) L(glLinkProgramARB) L(glUseProgramObjectARB) L(glDeleteObjectARB)
    L(glGetObjectParameterivARB) L(glGetInfoLogARB) L(glGetUniformLocationARB) L(glGetActiveUniformARB)
    L(glGetAttribLocationARB) L(glUniform1iARB) L(glUniform1fARB) L(glUniform2fARB) L(glUniform3fARB)
    L(glUniform4fARB) L(glUniformMatrix4fvARB) L(glUniformMatrix3fvARB) L(glVertexAttrib4fARB)
    L(glActiveTextureARB) L(glMultiTexCoord2fARB) L(glGenFramebuffersEXT) L(glBindFramebufferEXT)
    L(glFramebufferTexture2DEXT) L(glGenRenderbuffersEXT) L(glBindRenderbufferEXT)
    L(glRenderbufferStorageEXT) L(glFramebufferRenderbufferEXT) L(glCheckFramebufferStatusEXT)
#undef L
    return true;
}

constexpr GLenum kVS = 0x8B31, kFS = 0x8B30;
constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kActiveUniforms = 0x8B86;

// ---- the sources ----------------------------------------------------------------------------
struct Src {
    size_t      offset;
    std::string text;
    bool        frag;
};

static std::vector<Src> Extract(const char* exe) {
    std::vector<Src> out;
    FILE* f = nullptr;
    if (fopen_s(&f, exe, "rb") || !f) return out;
    std::vector<char> d;
    fseek(f, 0, SEEK_END); d.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f); fclose(f);
    std::set<size_t> seen;
    const std::string key = "void main";
    for (size_t p = 0; p + key.size() < d.size(); ++p) {
        if (memcmp(&d[p], key.data(), key.size()) != 0) continue;
        size_t s = p; while (s > 0 && d[s - 1] != 0) --s;
        size_t e = p; while (e < d.size() && d[e] != 0) ++e;
        if (!seen.insert(s).second) continue;
        Src x; x.offset = s; x.text.assign(&d[s], e - s);
        x.frag = x.text.find("gl_FragColor") != std::string::npos ||
                 x.text.find("gl_FragData") != std::string::npos;
        out.push_back(x);
    }
    return out;
}

static std::string Label(uint32_t r) {
    std::string s;
    auto add = [&](uint32_t b, const char* n) { if (r & b) { if (!s.empty()) s += "+"; s += n; } };
    add(shaderup::kBump, "bump"); add(shaderup::kSpec, "spec"); add(shaderup::kFresnel, "fresnel");
    add(shaderup::kShadow, "shadow"); add(shaderup::kTone, "tone");
    return s;
}

static int Table(const char* exe) {
    auto srcs = Extract(exe);
    if (srcs.empty()) { std::fprintf(stderr, "no shader strings in %s\n", exe); return 1; }
    std::printf("// Generated by tools/shaderlab (shaderlab --table <mxbikes.exe>). One row per fragment\n"
                "// shader the upgrade rewrites: FNV-1a 64 of the exact source, its length, the rules it\n"
                "// must take. No shader text.\n");
    int rows = 0;
    std::set<uint64_t> done;   // the exe holds a few sources twice
    for (const Src& s : srcs) {
        if (!s.frag || !done.insert(shaderup::Hash(s.text)).second) continue;
        const uint32_t can = shaderup::Probe(s.text);
        if (!(can & shaderup::kSpec)) continue;   // only the lit materials
        std::printf("{0x%016llxull, %zu, 0x%02x, \"%s\"},\n", (unsigned long long)shaderup::Hash(s.text),
                    s.text.size(), can, Label(can).c_str());
        ++rows;
    }
    std::fprintf(stderr, "%d rows from %zu sources\n", rows, srcs.size());
    return 0;
}

// ---- GL helpers -----------------------------------------------------------------------------
static std::string InfoLog(GLuint o) {
    char b[4096] = {0}; GLsizei n = 0;
    glGetInfoLogARB(o, sizeof(b) - 1, &n, b);
    return std::string(b, n);
}
static GLuint Compile(GLenum type, const std::string& src, std::string* log) {
    GLuint s = glCreateShaderObjectARB(type);
    const char* p = src.c_str();
    glShaderSourceARB(s, 1, &p, nullptr);
    glCompileShaderARB(s);
    GLint ok = 0; glGetObjectParameterivARB(s, kCompileStatus, &ok);
    if (!ok) { if (log) *log = InfoLog(s); glDeleteObjectARB(s); return 0; }
    return s;
}
static GLuint Link(GLuint vs, GLuint fs, std::string* log) {
    GLuint p = glCreateProgramObjectARB();
    if (vs) glAttachObjectARB(p, vs);
    glAttachObjectARB(p, fs);
    glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, kLinkStatus, &ok);
    if (!ok) { if (log) *log = InfoLog(p); glDeleteObjectARB(p); return 0; }
    return p;
}
static std::vector<shaderup::Loc> Locations(GLuint p) {
    std::vector<shaderup::Loc> v;
    GLint n = 0; glGetObjectParameterivARB(p, kActiveUniforms, &n);
    for (GLint i = 0; i < n; ++i) {
        char name[256]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniformARB(p, i, sizeof(name), &len, &size, &type, name);
        std::string s(name, len);
        if (s.compare(0, 3, "gl_") == 0) continue;
        if (s.size() > 3 && s.compare(s.size() - 3, 3, "[0]") == 0) s.resize(s.size() - 3);
        v.push_back({s, glGetUniformLocationARB(p, s.c_str())});
    }
    return v;
}

static std::set<std::string> Varyings(const std::string& s) {
    std::set<std::string> v;
    size_t p = 0;
    while ((p = s.find("varying ", p)) != std::string::npos) {
        p += 8;
        size_t a = s.find(' ', p); size_t b = s.find(';', a);
        if (a == std::string::npos || b == std::string::npos) break;
        v.insert(s.substr(a + 1, b - a - 1));
    }
    return v;
}

// ---- textures -------------------------------------------------------------------------------
static GLuint Tex2D(int w, int h, const std::vector<uint8_t>& rgba, bool mip = true) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP */, mip ? GL_TRUE : GL_FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return t;
}
static uint8_t U8(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return (uint8_t)(x * 255.0f + 0.5f); }

static GLuint MakeDecal() {
    const int n = 256; std::vector<uint8_t> d(n * n * 4);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const bool stripe = ((y / 32) % 4) == 0;
        // dark blue plastic with white stripes: dark enough for highlights and reflections to read
        float r = stripe ? 0.90f : 0.07f, g = stripe ? 0.90f : 0.11f, b = stripe ? 0.88f : 0.22f;
        uint8_t* p = &d[(y * n + x) * 4];
        p[0] = U8(r); p[1] = U8(g); p[2] = U8(b); p[3] = 255;
    }
    return Tex2D(n, n, d);
}
// Tangent-space normal map from a height field of rivets and grooves; alpha = specular mask.
static GLuint MakeBump() {
    const int n = 256; std::vector<float> h(n * n);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        float fx = (x % 32) - 16.0f, fy = (y % 32) - 16.0f;
        float r2 = fx * fx + fy * fy;
        float rivet = r2 < 49.0f ? std::sqrt(49.0f - r2) / 7.0f : 0.0f;
        float groove = (x % 64) < 3 ? -0.6f : 0.0f;
        float grain = 0.08f * std::sin(x * 0.9f) * std::sin(y * 1.3f);
        h[y * n + x] = rivet + groove + grain;
    }
    std::vector<uint8_t> d(n * n * 4);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        float dx = h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n];
        float dy = h[((y + 1) % n) * n + x] - h[((y + n - 1) % n) * n + x];
        float nx = -dx * 1.5f, ny = -dy * 1.5f, nz = 1.0f;
        float l = std::sqrt(nx * nx + ny * ny + nz * nz); nx /= l; ny /= l; nz /= l;
        uint8_t* p = &d[(y * n + x) * 4];
        p[0] = U8(nx * 0.5f + 0.5f); p[1] = U8(ny * 0.5f + 0.5f); p[2] = U8(nz * 0.5f + 0.5f);
        p[3] = ((y / 32) % 4) == 0 ? 90 : 255;
    }
    return Tex2D(n, n, d);
}
// The projected shadow texture: white = shadowed (the shaders take 1 - it). Low resolution and
// hard edged, the way an aliased shadow pass looks.
static GLuint MakeShadow() {
    const int n = 1024; std::vector<uint8_t> d(n * n * 4);   // a plausible in-game size
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
        const float t = u * 6.0f + v * 1.3f;
        const bool shadowed = t - std::floor(t) < 0.4f;   // diagonal bars: stair-stepped edges
        const uint8_t s = shadowed ? 165 : 0;
        uint8_t* p = &d[(y * n + x) * 4]; p[0] = p[1] = p[2] = s; p[3] = 255;
    }
    GLuint t = Tex2D(n, n, d, false);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    return t;
}
static GLuint MakeSolid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> d(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) { d[i * 4] = r; d[i * 4 + 1] = g; d[i * 4 + 2] = b; d[i * 4 + 3] = a; }
    return Tex2D(4, 4, d, false);
}
static GLuint MakeCube() {
    const int n = 64; GLuint t; glGenTextures(1, &t);
    glBindTexture(0x8513 /* GL_TEXTURE_CUBE_MAP */, t);
    glTexParameteri(0x8513, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(0x8513, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (int face = 0; face < 6; ++face) {
        std::vector<uint8_t> d(n * n * 4);
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            float a = 2.0f * (x + 0.5f) / n - 1.0f, b = 2.0f * (y + 0.5f) / n - 1.0f;
            float dx, dy, dz;   // the direction of this texel (GL cube face conventions)
            switch (face) {
            case 0: dx = 1; dy = -b; dz = -a; break; case 1: dx = -1; dy = -b; dz = a; break;
            case 2: dx = a; dy = 1; dz = b; break;   case 3: dx = a; dy = -1; dz = -b; break;
            case 4: dx = a; dy = -b; dz = 1; break;  default: dx = -a; dy = -b; dz = -1; break;
            }
            float l = std::sqrt(dx * dx + dy * dy + dz * dz); dy /= l;
            float r, g, bl;
            if (dy > 0) { r = 0.55f - 0.3f * dy; g = 0.70f - 0.25f * dy; bl = 0.95f; }
            else { r = 0.35f; g = 0.28f; bl = 0.20f; }
            if (dy > -0.05f && dy < 0.25f && std::fmod(std::fabs(std::atan2(dz, dx)) * 6.0f, 1.0f) < 0.35f) {
                r = 0.12f; g = 0.2f; bl = 0.1f;   // trees on the horizon
            }
            uint8_t* p = &d[(y * n + x) * 4]; p[0] = U8(r); p[1] = U8(g); p[2] = U8(bl); p[3] = 255;
        }
        glTexImage2D(0x8515 + face, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.data());
    }
    return t;
}

// ---- PNG (stored deflate; no zlib) ----------------------------------------------------------
static uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return c;
}
static void Be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i))); }
static void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    Be32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    Be32(out, Crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
static bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), &rgb[(size_t)y * w * 3], &rgb[(size_t)y * w * 3] + w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw.size();) {
        size_t n = raw.size() - p; if (n > 65535) n = 65535;
        z.push_back(p + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n); p += n;
    }
    Be32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ih; Be32(ih, w); Be32(ih, h); ih.push_back(8); ih.push_back(2); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    Chunk(out, "IHDR", ih); Chunk(out, "IDAT", z); Chunk(out, "IEND", {});
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
    fwrite(out.data(), 1, out.size(), f); fclose(f); return true;
}

// ---- the scene ------------------------------------------------------------------------------
struct Scene { GLuint decal, bump, shadow, cube, white, gloss; };

static void SetUniforms(GLuint p, const Scene& sc, float lx, float ly, float lz) {
    GLint n = 0; glGetObjectParameterivARB(p, kActiveUniforms, &n);
    for (GLint i = 0; i < n; ++i) {
        char nm[256]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniformARB(p, i, sizeof(nm), &len, &size, &type, nm);
        std::string s(nm, len);
        if (s.compare(0, 3, "gl_") == 0) continue;
        const GLint loc = glGetUniformLocationARB(p, s.c_str());
        if (type == 0x8B5E || type == 0x8B60) {   // sampler2D / samplerCube
            int unit = 6;
            if (s == "tex" || s == "baseImage") unit = 0;
            else if (s == "bumpMap") unit = 1;
            else if (s == "shadowMap") unit = 2;
            else if (s == "cubeMap") unit = 3;
            else if (s == "RefMap" || s == "glossMap") unit = 4;
            glUniform1iARB(loc, unit);
        } else if (s == "repetitions" || s == "bumpRepetitions") {
            if (type == 0x8B50) glUniform2fARB(loc, 2.0f, 2.0f); else glUniform1fARB(loc, 2.0f);
        } else if (s == "RefFactor") glUniform1fARB(loc, 0.6f);
        else if (s == "RefFactorMin") glUniform1fARB(loc, 0.05f);
        else if (s == "FresExp") glUniform1fARB(loc, 1.5f);
        else if (s == "LightPosition") glUniform3fARB(loc, lx, ly, lz);
        else if (s == "TexCoordProj") {
            // object xz -> shadow uv, q in .z = 1 (the shaders divide by .z)
            const float m[16] = {0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0.5f, 0.5f, 1, 1};
            glUniformMatrix4fvARB(loc, 1, GL_FALSE, m);
        } else if (s == "ObjToCubeSpace") {
            const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            glUniformMatrix4fvARB(loc, 1, GL_FALSE, m);
        } else if (type == 0x1406) glUniform1fARB(loc, 0.0f);
    }
    (void)sc;
}

static void BindScene(const Scene& sc) {
    const GLuint units[7] = {sc.decal, sc.bump, sc.shadow, 0, sc.gloss, sc.white, sc.white};
    for (int u = 0; u < 7; ++u) {
        glActiveTextureARB(0x84C0 + u);
        if (u == 3) glBindTexture(0x8513, sc.cube); else glBindTexture(GL_TEXTURE_2D, units[u]);
    }
    glActiveTextureARB(0x84C0);
}

// A UV sphere in immediate mode: normal, uv, tangent (attribute), position.
static void DrawSphere(GLint tangentLoc) {
    const int R = 64, C = 128;
    const float PI = 3.14159265f;
    glBegin(GL_TRIANGLES);
    auto vtx = [&](int i, int j) {
        float th = PI * i / R, ph = 2 * PI * j / C;
        float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
        float tx = -std::sin(ph), tz = std::cos(ph);
        glNormal3f(x, y, z);
        glMultiTexCoord2fARB(0x84C0, 4.0f * j / C, 2.0f * i / R);
        if (tangentLoc >= 0) glVertexAttrib4fARB(tangentLoc, tx, 0, tz, 1.0f);
        glVertex3f(x, y, z);
    };
    for (int i = 0; i < R; ++i) for (int j = 0; j < C; ++j) {
        vtx(i, j); vtx(i + 1, j); vtx(i + 1, j + 1);
        vtx(i, j); vtx(i + 1, j + 1); vtx(i, j + 1);
    }
    glEnd();
}

// setU=false draws with whatever uniforms the program holds (the game sets most of them once);
// tangent=-2 asks for the attribute's location, otherwise it is the one the "game" cached.
static void RenderPanel(GLuint prog, const Scene& sc, float shininess, std::vector<uint8_t>& rgb, int W, int H,
                        float zoom = 1.0f, bool setU = true, GLint tangent = -2) {
    glViewport(0, 0, W, H);
    glClearColor(0.18f, 0.19f, 0.21f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); const double fr = 0.55 / zoom; glFrustum(-fr, fr, -fr, fr, 1.5, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -3.2f); glRotatef(18, 1, 0, 0); glRotatef(-25, 0, 1, 0);
    const GLfloat amb[4] = {0.32f, 0.33f, 0.36f, 1}, dif[4] = {0.85f, 0.82f, 0.76f, 1}, spe[4] = {1, 1, 1, 1};
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb); glLightfv(GL_LIGHT0, GL_DIFFUSE, dif); glLightfv(GL_LIGHT0, GL_SPECULAR, spe);
    const GLfloat mamb[4] = {1, 1, 1, 1}, mdif[4] = {1, 1, 1, 1}, mspe[4] = {0.7f, 0.7f, 0.7f, 1};
    glMaterialfv(GL_FRONT, GL_AMBIENT, mamb); glMaterialfv(GL_FRONT, GL_DIFFUSE, mdif);
    glMaterialfv(GL_FRONT, GL_SPECULAR, mspe); glMaterialf(GL_FRONT, GL_SHININESS, shininess);
    glFogf(GL_FOG_DENSITY, 0.0f);
    const GLfloat fogc[4] = {0.5f, 0.5f, 0.5f, 1}; glFogfv(GL_FOG_COLOR, fogc);
    glUseProgramObjectARB(prog);
    // eye-space direction to the sun (upper left, slightly behind the camera)
    float lx = -0.45f, ly = 0.65f, lz = 0.6f; float l = std::sqrt(lx * lx + ly * ly + lz * lz);
    if (setU) SetUniforms(prog, sc, lx / l, ly / l, lz / l);
    BindScene(sc);
    DrawSphere(tangent == -2 ? glGetAttribLocationARB(prog, "Tangent") : tangent);
    glUseProgramObjectARB(0);
    glFinish();
    rgb.resize((size_t)W * H * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    // flip to top-down for the PNG
    for (int y = 0; y < H / 2; ++y)
        for (int x = 0; x < W * 3; ++x) std::swap(rgb[(size_t)y * W * 3 + x], rgb[(size_t)(H - 1 - y) * W * 3 + x]);
}

static void Blit(std::vector<uint8_t>& dst, int DW, int ox, int oy, const std::vector<uint8_t>& src, int W, int H) {
    for (int y = 0; y < H; ++y) memcpy(&dst[((size_t)(oy + y) * DW + ox) * 3], &src[(size_t)y * W * 3], (size_t)W * 3);
}

static int Check(const char* exe, const std::string& outDir) {
    auto srcs = Extract(exe);
    if (srcs.empty()) { std::printf("no shader strings in %s\n", exe); return 1; }

    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "shaderlab";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "shaderlab", "shaderlab", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    // never ShowWindow: the window only exists to own a pixel format
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);   // legacy context = compatibility profile, like the game
    wglMakeCurrent(dc, rc);
    std::printf("GL: %s | %s | GLSL %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION),
                (const char*)glGetString(0x8B8C));
    if (!LoadGl()) return 1;
    shaderswitch::Api api;
    if (!api.Load([](const char* n) { return (void*)wglGetProcAddress(n); })) { std::printf("missing ARB entry points\n"); return 1; }

    // An FBO to draw into (the window is never shown, so its own pixels are not ours).
    GLuint fbo, color, depth; const int W = 420, H = 420;
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(0x8D40, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2DEXT(0x8D40, 0x8CE0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffersEXT(1, &depth); glBindRenderbufferEXT(0x8D41, depth);
    glRenderbufferStorageEXT(0x8D41, 0x81A6 /* GL_DEPTH_COMPONENT24 */, W, H);
    glFramebufferRenderbufferEXT(0x8D40, 0x8D00, 0x8D41, depth);
    if (glCheckFramebufferStatusEXT(0x8D40) != 0x8CD5) { std::printf("FBO incomplete\n"); return 1; }
    Scene sc{MakeDecal(), MakeBump(), MakeShadow(), MakeCube(), MakeSolid(255, 255, 255, 255), MakeSolid(255, 255, 255, 255)};

    shaderup::Config defCfg; defCfg.on = true;
    shaderup::Config maxCfg = defCfg; maxCfg.tone = true; maxCfg.bump = 300; maxCfg.spec = 300; maxCfg.soft = 400;

    // 1. Every source compiles as shipped; every known one compiles and links upgraded.
    int compiled = 0, failedOrig = 0, known = 0, upOk = 0, upFail = 0, unknownLit = 0;
    int relinkOk = 0, relinkBad = 0, linkPairs = 0;
    size_t tableN = 0; const shaderup::Known* table = shaderup::KnownTable(&tableN);
    std::set<uint64_t> matched;
    struct Pair { size_t fs, vs; uint32_t rules; };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < srcs.size(); ++i) {
        const Src& s = srcs[i];
        std::string log;
        GLuint o = Compile(s.frag ? kFS : kVS, s.text, &log);
        if (o) { ++compiled; glDeleteObjectARB(o); }
        else { ++failedOrig; std::printf("original #%zu does not compile here: %s\n", i, log.c_str()); }
        if (!s.frag) continue;
        const shaderup::Known* k = shaderup::Find(shaderup::Hash(s.text), s.text.size());
        if (!k) {
            if (shaderup::Probe(s.text) & shaderup::kSpec) { ++unknownLit; std::printf("lit source #%zu not in the table\n", i); }
            continue;
        }
        ++known; matched.insert(k->hash);
        // its vertex shader: the nearest one before it that feeds all its varyings
        auto fv = Varyings(s.text);
        size_t vs = SIZE_MAX;
        for (size_t j = i; j-- > 0;) {
            if (srcs[j].frag) continue;
            auto vv = Varyings(srcs[j].text);
            bool all = true; for (auto& x : fv) all = all && vv.count(x);
            if (all) { vs = j; break; }
        }
        for (const shaderup::Config* c : {&defCfg, &maxCfg}) {
            auto r = shaderup::Upgrade(s.text, *k, *c);
            if (!r.ok) { ++upFail; std::printf("upgrade #%zu refused: %s\n", i, r.why); continue; }
            GLuint f = Compile(kFS, r.out, &log);
            if (!f) { ++upFail; std::printf("upgraded #%zu (%s) does not compile: %s\n", i, k->label, log.c_str()); continue; }
            if (vs != SIZE_MAX) {
                GLuint v = Compile(kVS, srcs[vs].text, nullptr);
                GLuint p = Link(v, f, &log);
                if (!p) { ++upFail; std::printf("upgraded #%zu does not link: %s\n", i, log.c_str()); }
                else glDeleteObjectARB(p);
                glDeleteObjectARB(v);
            }
            glDeleteObjectARB(f);
            ++upOk;
        }
        if (vs == SIZE_MAX) { std::printf("no vertex shader found for #%zu\n", i); continue; }
        pairs.push_back({i, vs, k->rules});

        // 2. The live A/B, replayed with the code the DLL runs (src/shaderswitch.h): the "game"
        // links the original, keeps its locations and its Tangent slot, sets its uniforms once
        // and draws. Then the program is switched on and drawn again WITHOUT touching a
        // uniform - it must match a freshly built upgraded program pixel for pixel - and
        // switched off again - it must match the first picture pixel for pixel.
        {
            ++linkPairs;
            const int AW = 160, AH = 160;
            GLuint v = Compile(kVS, srcs[vs].text, nullptr);
            GLuint f = Compile(kFS, s.text, nullptr);
            GLuint p = Link(v, f, nullptr);
            if (!p) { std::printf("original pair #%zu/#%zu does not link\n", i, vs); continue; }
            const auto game = shaderswitch::Locations(api, p);
            const GLint tangent = glGetAttribLocationARB(p, "Tangent");
            auto up = shaderup::Upgrade(s.text, *k, defCfg);
            std::vector<uint8_t> a, b, c, ref;
            RenderPanel(p, sc, 32, a, AW, AH);                                 // sets the uniforms once
            std::string why;
            auto o1 = shaderswitch::Relink(api, p, {{f, up.out, s.text}}, game, &why);
            if (o1 == shaderswitch::Outcome::Switched) RenderPanel(p, sc, 32, b, AW, AH, 1.0f, false, tangent);
            auto o2 = o1 == shaderswitch::Outcome::Switched
                          ? shaderswitch::Relink(api, p, {{f, s.text, up.out}}, game, &why) : o1;
            if (o2 == shaderswitch::Outcome::Switched) RenderPanel(p, sc, 32, c, AW, AH, 1.0f, false, tangent);
            GLuint fu = Compile(kFS, up.out, nullptr), pu = Link(v, fu, nullptr);
            RenderPanel(pu, sc, 32, ref, AW, AH);
            const bool good = o1 == shaderswitch::Outcome::Switched && o2 == shaderswitch::Outcome::Switched &&
                              b == ref && c == a && a != ref;
            if (good) ++relinkOk;
            else {
                ++relinkBad;
                std::printf("live A/B #%zu (%s): on=%d off=%d on==fresh:%d off==original:%d %s\n", i, k->label, (int)o1,
                            (int)o2, b == ref, c == a, why.c_str());
            }
            glDeleteObjectARB(pu); glDeleteObjectARB(fu);
            glDeleteObjectARB(p); glDeleteObjectARB(f); glDeleteObjectARB(v);
        }
    }
    // the grade pass
    {
        std::string log;
        GLuint g = Compile(kFS, shaderup::GradeFragmentSource(), &log);
        GLuint p = g ? Link(0, g, &log) : 0;
        std::printf("grade pass: %s %s\n", p ? "compiles and links" : "FAILS", log.c_str());
        if (!p) ++upFail;
        if (p) glDeleteObjectARB(p);
        if (g) glDeleteObjectARB(g);
    }
    int stale = 0;
    for (size_t i = 0; i < tableN; ++i) if (!matched.count(table[i].hash)) { ++stale; std::printf("table row %s not in this exe\n", table[i].label); }
    std::printf("\nsources: %zu, compile as shipped: %d (failed %d)\n", srcs.size(), compiled, failedOrig);
    std::printf("known fragment shaders: %d sources match the %zu table rows (stale rows %d, lit sources missing from table %d)\n",
                known, tableN, stale, unknownLit);
    std::printf("upgraded compiles+links: %d ok, %d failed (default and max settings)\n", upOk, upFail);
    std::printf("live A/B (switch on, off; uniforms kept, pixels match): %d of %d\n", relinkOk, linkPairs);

    // 3. Pictures.
    if (!outDir.empty()) {
        struct Shot { const char* name; uint32_t need, avoid; };
        const Shot shots[] = {
            {"bump_spec_shadow", shaderup::kBump | shaderup::kSpec | shaderup::kShadow, shaderup::kFresnel},
            {"bump_spec_reflection", shaderup::kBump | shaderup::kSpec | shaderup::kFresnel, 0},
            {"spec_no_bump", shaderup::kSpec, shaderup::kBump},
        };
        const float shin[4] = {8, 24, 64, 128};
        for (const Shot& shot : shots) {
            const Pair* pick = nullptr;
            for (const Pair& p : pairs) if ((p.rules & shot.need) == shot.need && !(p.rules & shot.avoid)) { pick = &p; break; }
            if (!pick) { std::printf("no source for %s\n", shot.name); continue; }
            const Src& fs = srcs[pick->fs];
            const shaderup::Known* k = shaderup::Find(shaderup::Hash(fs.text), fs.text.size());
            auto up = shaderup::Upgrade(fs.text, *k, defCfg);
            GLuint v = Compile(kVS, srcs[pick->vs].text, nullptr);
            GLuint fo = Compile(kFS, fs.text, nullptr), fu = Compile(kFS, up.out, nullptr);
            GLuint po = Link(v, fo, nullptr), pu = Link(v, fu, nullptr);
            // rows: shininess 8 / 24 / 64 / 128; columns: original | upgraded
            const int DW = W * 2, DH = H * 4;
            std::vector<uint8_t> sheet((size_t)DW * DH * 3), panel;
            for (int r = 0; r < 4; ++r) {
                RenderPanel(po, sc, shin[r], panel, W, H); Blit(sheet, DW, 0, r * H, panel, W, H);
                RenderPanel(pu, sc, shin[r], panel, W, H); Blit(sheet, DW, W, r * H, panel, W, H);
            }
            const std::string path = outDir + "/" + shot.name + ".png";
            if (&shot == &shots[0]) {   // the shadow edge up close: one tap vs four
                const int CW = W * 2;
                std::vector<uint8_t> close((size_t)CW * H * 3);
                Scene flat = sc; flat.decal = sc.white;   // a plain white surface shows the edge best
                RenderPanel(po, flat, 24, panel, W, H, 8.0f); Blit(close, CW, 0, 0, panel, W, H);
                RenderPanel(pu, flat, 24, panel, W, H, 8.0f); Blit(close, CW, W, 0, panel, W, H);
                WritePng(outDir + "/shadow_closeup.png", CW, H, close);
            }
            std::printf("%s: source %s (%s) -> %s\n", shot.name, k->label, WritePng(path, DW, DH, sheet) ? "ok" : "WRITE FAILED", path.c_str());
            glDeleteObjectARB(po); glDeleteObjectARB(pu); glDeleteObjectARB(fo); glDeleteObjectARB(fu); glDeleteObjectARB(v);
        }
        glBindFramebufferEXT(0x8D40, 0);
    }
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); ReleaseDC(w, dc); DestroyWindow(w);
    return (failedOrig || upFail || unknownLit || stale || relinkBad) ? 2 : 0;
}

// ---- the real frostmod.dll, in this process ----------------------------------------------------
// FrostMod assumes MX Bikes' offsets in a process it does not know and byte-checks every RVA it
// would patch. This much zeroed image keeps those reads inside our own module, where they fail
// the checks and nothing is written.
static char g_fakeImage[24 << 20];

static bool LogHas(const std::string& log, const char* what, int waitMs) {
    for (int t = 0; t <= waitMs; t += 100) {
        FILE* f = nullptr;
        if (!fopen_s(&f, log.c_str(), "rb") && f) {
            std::string s; char b[4096]; size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
            fclose(f);
            if (s.find(what) != std::string::npos) return true;
        }
        Sleep(100);
    }
    return false;
}

// shaderlab --dll <dir with frostmod.dll and frostmod_radar.cfg (shaders=1)> <mxbikes.exe> early|late
//   early: the dll is loaded before the GL context exists, like an injected or plugin start;
//          programs built after it must come out upgraded at the source.
//   late:  programs are built and drawn first, then the dll is loaded; the first swap must find
//          and switch them, keeping every uniform the "game" set.
// Either way every program, drawn without setting a uniform again, must match a fresh upgraded
// build pixel for pixel. Read the dll's log in that folder for the [shaders] lines.
static int DllTest(const std::string& dir, const char* exe, bool late) {
    g_fakeImage[0] = 1;   // keep the array (and so the image size)
    auto srcs = Extract(exe);
    const std::string dll = dir + "\\frostmod.dll", log = dir + "\\frostmod.log";
    DeleteFileA(log.c_str());
    HMODULE fm = nullptr;
    auto loadDll = [&]() -> bool {
        fm = LoadLibraryA(dll.c_str());
        if (!fm) { std::printf("LoadLibrary failed (%lu)\n", GetLastError()); return false; }
        if (!LogHas(log, "[shaders] ON", 30000)) { std::printf("no [shaders] ON line in %s\n", log.c_str()); return false; }
        return true;
    };
    if (!late && !loadDll()) return 1;

    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "shaderlab";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "shaderlab", "shaderlab", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    if (!LoadGl()) return 1;   // the "game" resolves its entry points (through the dll's hook, early)

    GLuint fbo, color, depth; const int W = 160, H = 160;
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(0x8D40, fbo);
    glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2DEXT(0x8D40, 0x8CE0, GL_TEXTURE_2D, color, 0);
    glGenRenderbuffersEXT(1, &depth); glBindRenderbufferEXT(0x8D41, depth);
    glRenderbufferStorageEXT(0x8D41, 0x81A6, W, H);
    glFramebufferRenderbufferEXT(0x8D40, 0x8D00, 0x8D41, depth);
    Scene sc{MakeDecal(), MakeBump(), MakeShadow(), MakeCube(), MakeSolid(255, 255, 255, 255), MakeSolid(255, 255, 255, 255)};

    // The "game": every known fragment shader with its vertex shader, built the game's way.
    struct G { size_t fs; GLuint p; GLint tangent; std::vector<uint8_t> before; std::string up; };
    std::vector<G> progs;
    std::set<uint64_t> seen;
    shaderup::Config cfg; cfg.on = true;
    for (size_t i = 0; i < srcs.size(); ++i) {
        const Src& s = srcs[i];
        if (!s.frag) continue;
        const shaderup::Known* k = shaderup::Find(shaderup::Hash(s.text), s.text.size());
        if (!k || !seen.insert(k->hash).second) continue;
        auto fv = Varyings(s.text);
        size_t vs = SIZE_MAX;
        for (size_t j = i; j-- > 0;) {
            if (srcs[j].frag) continue;
            auto vv = Varyings(srcs[j].text);
            bool all = true; for (auto& x : fv) all = all && vv.count(x);
            if (all) { vs = j; break; }
        }
        if (vs == SIZE_MAX) continue;
        G g; g.fs = i; g.up = shaderup::Upgrade(s.text, *k, cfg).out;
        g.p = Link(Compile(kVS, srcs[vs].text, nullptr), Compile(kFS, s.text, nullptr), nullptr);
        if (!g.p) { std::printf("program for #%zu did not link\n", i); return 1; }
        g.tangent = glGetAttribLocationARB(g.p, "Tangent");
        RenderPanel(g.p, sc, 32, g.before, W, H);   // the game sets its uniforms once
        progs.push_back(g);
    }
    if (late && !loadDll()) return 1;

    // First frame through the dll's swap hooks.
    glBindFramebufferEXT(0x8D40, 0);
    SwapBuffers(dc);
    glBindFramebufferEXT(0x8D40, fbo);
    LogHas(log, "[shaders] first frame", 5000);

    int good = 0, bad = 0;
    for (G& g : progs) {
        std::vector<uint8_t> after, ref;
        RenderPanel(g.p, sc, 32, after, W, H, 1.0f, false, g.tangent);
        // a fresh upgraded build; its source hash is not in the table, so the dll lets it be
        GLuint v2 = 0;
        GLuint objs[4] = {}; GLsizei n = 0;
        auto getAttached = (void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLuint*))wglGetProcAddress("glGetAttachedObjectsARB");
        getAttached(g.p, 4, &n, objs);
        for (GLsizei a = 0; a < n; ++a) { GLint t = 0; glGetObjectParameterivARB(objs[a], 0x8B4F, &t); if (t == (GLint)kVS) v2 = objs[a]; }
        GLuint pu = Link(v2, Compile(kFS, g.up, nullptr), nullptr);
        RenderPanel(pu, sc, 32, ref, W, H);
        // early: upgraded from the start; late: switched by the first frame
        const bool ok = after == ref && (late ? after != g.before : g.before == ref);
        ok ? ++good : ++bad;
        if (!ok) std::printf("#%zu: after==upgraded %d, before==upgraded %d\n", g.fs, after == ref, g.before == ref);
    }
    std::printf("%s start: %d of %zu programs drawn upgraded with the game's uniforms kept\n", late ? "late" : "early", good,
                progs.size());
    glBindFramebufferEXT(0x8D40, 0);
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); ReleaseDC(w, dc); DestroyWindow(w);
    // Leave the dll loaded: the process ends here, the way a game does.
    return bad ? 2 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--table") == 0) return Table(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--check") == 0) return Check(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 5 && std::strcmp(argv[1], "--dll") == 0) {
        const int r = DllTest(argv[2], argv[3], std::strcmp(argv[4], "late") == 0);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), (UINT)r);   // FrostMod's threads do not get a say
    }
    std::printf("usage: shaderlab --table <mxbikes.exe> | --check <mxbikes.exe> [out dir] |\n"
                "                 --dll <dir> <mxbikes.exe> early|late\n");
    return 1;
}
