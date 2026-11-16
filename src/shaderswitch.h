// shaderswitch.h - relinking a live GL program without the game noticing (shaders=1 A/B).
//
// The game links each program once, asks glGetUniformLocationARB for every uniform, keeps the
// answers, and sets most uniforms once. Switching a program's fragment shader while the game
// runs therefore means: read every active uniform's value, pin every vertex attribute to the
// location it has, re-source + compile + relink, check every uniform still sits where the game
// believes it does (shaderup::LocationsCompatible), and write the values back. Anything that
// fails puts the old sources back and relinks again.
//
// Shared by frostmod.cpp (SHADERS section) and tools/shaderlab, which runs it on every game
// shader in a real GL context and compares the pictures, so the code that ships is the code
// that was checked. Needs a current GL context; no game knowledge.
#pragma once
#include <windows.h>
#include <GL/gl.h>

#include <string>
#include <vector>

#include "shaderup.h"

namespace shaderswitch {

// The ARB shader-object entry points it needs. GLhandleARB is an unsigned int on Windows.
struct Api {
    void(APIENTRY* source)(GLuint, GLsizei, const char* const*, const GLint*) = nullptr;
    void(APIENTRY* compile)(GLuint) = nullptr;
    void(APIENTRY* link)(GLuint) = nullptr;
    void(APIENTRY* param)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* infoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
    void(APIENTRY* attached)(GLuint, GLsizei, GLsizei*, GLuint*) = nullptr;
    void(APIENTRY* activeU)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*) = nullptr;
    void(APIENTRY* activeA)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*) = nullptr;
    GLint(APIENTRY* uLoc)(GLuint, const char*) = nullptr;
    GLint(APIENTRY* aLoc)(GLuint, const char*) = nullptr;
    void(APIENTRY* bindAttr)(GLuint, GLuint, const char*) = nullptr;
    void(APIENTRY* getUf)(GLuint, GLint, GLfloat*) = nullptr;
    void(APIENTRY* getUi)(GLuint, GLint, GLint*) = nullptr;
    void(APIENTRY* use)(GLuint) = nullptr;
    GLuint(APIENTRY* handle)(GLenum) = nullptr;
    using Ufv  = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
    using Uiv  = void(APIENTRY*)(GLint, GLsizei, const GLint*);
    using Umat = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
    Ufv  ufv[4] = {};
    Uiv  uiv[4] = {};
    Umat umat[3] = {};   // 2x2, 3x3, 4x4

    // Fill from a resolver (wglGetProcAddress, or a trampoline-aware one). True if all present.
    template <class F> bool Load(F proc) {
        source   = (decltype(source))proc("glShaderSourceARB");
        compile  = (decltype(compile))proc("glCompileShaderARB");
        link     = (decltype(link))proc("glLinkProgramARB");
        param    = (decltype(param))proc("glGetObjectParameterivARB");
        infoLog  = (decltype(infoLog))proc("glGetInfoLogARB");
        attached = (decltype(attached))proc("glGetAttachedObjectsARB");
        activeU  = (decltype(activeU))proc("glGetActiveUniformARB");
        activeA  = (decltype(activeA))proc("glGetActiveAttribARB");
        uLoc     = (decltype(uLoc))proc("glGetUniformLocationARB");
        aLoc     = (decltype(aLoc))proc("glGetAttribLocationARB");
        bindAttr = (decltype(bindAttr))proc("glBindAttribLocationARB");
        getUf    = (decltype(getUf))proc("glGetUniformfvARB");
        getUi    = (decltype(getUi))proc("glGetUniformivARB");
        use      = (decltype(use))proc("glUseProgramObjectARB");
        handle   = (decltype(handle))proc("glGetHandleARB");
        const char* fv[4] = {"glUniform1fvARB", "glUniform2fvARB", "glUniform3fvARB", "glUniform4fvARB"};
        const char* iv[4] = {"glUniform1ivARB", "glUniform2ivARB", "glUniform3ivARB", "glUniform4ivARB"};
        const char* mv[3] = {"glUniformMatrix2fvARB", "glUniformMatrix3fvARB", "glUniformMatrix4fvARB"};
        bool ok = source && compile && link && param && infoLog && attached && activeU && activeA &&
                  uLoc && aLoc && bindAttr && getUf && getUi && use && handle;
        for (int i = 0; i < 4; ++i) {
            ufv[i] = (Ufv)proc(fv[i]);
            uiv[i] = (Uiv)proc(iv[i]);
            ok = ok && ufv[i] && uiv[i];
        }
        for (int i = 0; i < 3; ++i) { umat[i] = (Umat)proc(mv[i]); ok = ok && umat[i]; }
        return ok;
    }
};

constexpr GLenum kProgramObject  = 0x8B40;   // GL_PROGRAM_OBJECT_ARB
constexpr GLenum kObjectType     = 0x8B4E;   // GL_OBJECT_TYPE_ARB
constexpr GLenum kCompileStatus  = 0x8B81;
constexpr GLenum kLinkStatus     = 0x8B82;
constexpr GLenum kActiveUniforms = 0x8B86;
constexpr GLenum kActiveAttribs  = 0x8B89;

inline std::string InfoLog(const Api& gl, GLuint o) {
    char b[600] = {0}; GLsizei n = 0;
    gl.infoLog(o, sizeof(b) - 1, &n, b);
    std::string s(b, n > 0 ? (size_t)n : 0);
    for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
    return s;
}
inline bool Status(const Api& gl, GLuint o, GLenum what) {
    GLint ok = 0; gl.param(o, what, &ok); return ok != 0;
}
inline bool IsProgram(const Api& gl, GLuint p) {
    GLint t = 0; gl.param(p, kObjectType, &t); return t == (GLint)kProgramObject;
}

// The active uniforms and where they sit (arrays by base name; gl_ state skipped).
inline std::vector<shaderup::Loc> Locations(const Api& gl, GLuint p) {
    std::vector<shaderup::Loc> v;
    GLint n = 0; gl.param(p, kActiveUniforms, &n);
    for (GLint i = 0; i < n && i < 256; ++i) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        gl.activeU(p, (GLuint)i, sizeof(name), &len, &size, &type, name);
        std::string s(name, len > 0 ? (size_t)len : 0);
        if (s.compare(0, 3, "gl_") == 0) continue;
        if (s.size() > 3 && s.compare(s.size() - 3, 3, "[0]") == 0) s.resize(s.size() - 3);
        v.push_back({s, gl.uLoc(p, s.c_str())});
    }
    return v;
}

inline std::vector<GLuint> Attached(const Api& gl, GLuint p) {
    GLuint objs[8] = {}; GLsizei n = 0;
    gl.attached(p, 8, &n, objs);
    return std::vector<GLuint>(objs, objs + (n > 0 ? (n > 8 ? 8 : n) : 0));
}

inline void SetSource(const Api& gl, GLuint sh, const std::string& text) {
    const char* p = text.c_str();
    gl.source(sh, 1, &p, nullptr);
}

// One uniform element's value.
struct Saved { GLint loc; shaderup::UType t; GLfloat f[16]; GLint i[16]; };

inline std::vector<Saved> SaveUniforms(const Api& gl, GLuint p) {
    std::vector<Saved> v;
    GLint n = 0; gl.param(p, kActiveUniforms, &n);
    for (GLint k = 0; k < n && k < 256; ++k) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        gl.activeU(p, (GLuint)k, sizeof(name), &len, &size, &type, name);
        std::string s(name, len > 0 ? (size_t)len : 0);
        shaderup::UType t;
        if (s.compare(0, 3, "gl_") == 0 || !shaderup::UniformType(type, &t)) continue;
        if (s.size() > 3 && s.compare(s.size() - 3, 3, "[0]") == 0) s.resize(s.size() - 3);
        for (GLint e = 0; e < size && e < 64; ++e) {
            const std::string el = size > 1 ? s + "[" + std::to_string(e) + "]" : s;
            Saved sv{}; sv.t = t; sv.loc = gl.uLoc(p, el.c_str());
            if (sv.loc < 0) continue;
            if (t.isInt) gl.getUi(p, sv.loc, sv.i); else gl.getUf(p, sv.loc, sv.f);
            v.push_back(sv);
        }
    }
    return v;
}

// Write the values back (the program is made current for it, then the caller's is restored).
inline void RestoreUniforms(const Api& gl, GLuint p, const std::vector<Saved>& v) {
    if (v.empty()) return;
    const GLuint prev = gl.handle(kProgramObject);
    gl.use(p);
    for (const Saved& s : v) {
        if (s.t.isMatrix) gl.umat[s.t.comps == 4 ? 0 : (s.t.comps == 9 ? 1 : 2)](s.loc, 1, GL_FALSE, s.f);
        else if (s.t.isInt) gl.uiv[s.t.comps - 1](s.loc, 1, s.i);
        else gl.ufv[s.t.comps - 1](s.loc, 1, s.f);
    }
    gl.use(prev);
}

// Pin every active attribute to where it is now, for the next link.
inline void PinAttributes(const Api& gl, GLuint p) {
    GLint na = 0; gl.param(p, kActiveAttribs, &na);
    for (GLint k = 0; k < na && k < 32; ++k) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum t = 0;
        gl.activeA(p, (GLuint)k, sizeof(name), &len, &size, &t, name);
        std::string s(name, len > 0 ? (size_t)len : 0);
        if (s.compare(0, 3, "gl_") == 0) continue;
        const GLint loc = gl.aLoc(p, s.c_str());
        if (loc >= 0) gl.bindAttr(p, (GLuint)loc, s.c_str());
    }
}

struct Change {
    GLuint      shader;
    std::string to;     // the text to switch to
    std::string from;   // the text it holds now (put back on failure)
};

enum class Outcome { Switched, Refused, RefusedUnclean };

// Switch `p` to the sources in `ch`, keeping the game's locations `game`. On anything but
// success the old sources are back and the program relinked as it was. Uniform values are
// kept either way. `why` says what failed.
inline Outcome Relink(const Api& gl, GLuint p, const std::vector<Change>& ch,
                      const std::vector<shaderup::Loc>& game, std::string* why) {
    const auto values = SaveUniforms(gl, p);
    PinAttributes(gl, p);
    bool ok = true;
    std::string w;
    for (const Change& c : ch) {
        SetSource(gl, c.shader, c.to);
        gl.compile(c.shader);
        if (!Status(gl, c.shader, kCompileStatus)) { ok = false; w = "compile: " + InfoLog(gl, c.shader); break; }
    }
    if (ok) {
        gl.link(p);
        if (!Status(gl, p, kLinkStatus)) { ok = false; w = "link: " + InfoLog(gl, p); }
        else if (!shaderup::LocationsCompatible(game, Locations(gl, p), &w)) { ok = false; w = "uniforms: " + w; }
    }
    Outcome out = Outcome::Switched;
    if (!ok) {
        for (const Change& c : ch) { SetSource(gl, c.shader, c.from); gl.compile(c.shader); }
        gl.link(p);
        std::string w2;
        const bool back = Status(gl, p, kLinkStatus) && shaderup::LocationsCompatible(game, Locations(gl, p), &w2);
        out = back ? Outcome::Refused : Outcome::RefusedUnclean;
    }
    RestoreUniforms(gl, p, values);
    if (why) *why = w;
    return out;
}

}  // namespace shaderswitch
