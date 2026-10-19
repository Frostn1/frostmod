// srvpage_gl.h - the in-game Servers page: the hooks, the input and the GL draw.
// Included once, from frostmod.cpp, after the texture hooks (hkMdTexImage2D) and before the
// swap hook that calls SrvFrame. The pure parts (font, sprites, servers.json, layout) are in
// srvpage.h.
//
// How the page knows the main menu is up. The game has no hook for "which screen is this", so
// the override main.mnu gives our button an idle sprite of its own, frostsrv1.tga: 16 x 16, every
// pixel the colour kMarker with alpha 0, so it draws nothing. Its upload is spotted in
// glTexImage2D (that size, every pixel that colour) and its texture name kept; a frame in which
// the game binds that name is a frame with the main menu on screen. Nothing else in the game is
// 16 x 16 in that colour.
//
// Input is polled (GetAsyncKeyState, GetCursorPos) like the rest of FrostMod's panels. Nothing is
// swallowed: on the main menu the right of the screen has nothing of the game's to click, and
// the game's own column stays live - pick Testing and the page closes with the menu.
//
// Join restarts the game into the server: the game reads -directconnect only at startup (the
// flag MXB App joins with). FrostMod writes the address to frostmod_join.txt and closes the
// game; MXB App sees the game close, checks the address and starts it again into the server.
// FrostMod starts no process of its own. Joining in-process is later work (bus command 0x389,
// offsets.h).
//
// Cost: two pointer compares per glBindTexture, a few calls a frame on the main menu, and the
// page's quads only while it is open. The font and sprites are read from ui.pkz on first open.

namespace srvgl {

constexpr uint8_t kMarker[3] = {0xF1, 0x05, 0x7E};  // the marker sprite's colour, R G B

std::atomic<bool>     g_on{false};       // the override main.mnu with our button is in place
std::atomic<GLuint>   g_marker{0};       // the marker sprite's texture name, once seen
std::atomic<uint64_t> g_marker_ms{0};     // when it was last bound (GetTickCount64)
unsigned              g_frame = 1;

typedef void(APIENTRY* BindTexture_t)(GLenum, GLuint);
BindTexture_t g_origBind = nullptr;

void APIENTRY hkBindTexture(GLenum target, GLuint tex) {
    const GLuint m = g_marker.load(std::memory_order_relaxed);
    if (m && tex == m) g_marker_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_origBind(target, tex);
}

}  // namespace srvgl

/// From hkMdTexImage2D, after the upload: is this the marker sprite?
static void SrvOnTexImage(GLenum target, GLint level, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* px) {
    using namespace srvgl;
    if (!g_on.load(std::memory_order_relaxed) || target != GL_TEXTURE_2D || level != 0 || w != 16 || h != 16 || !px ||
        type != GL_UNSIGNED_BYTE)
        return;
    const int step = (format == GL_RGBA || format == 0x80E1 /*GL_BGRA*/) ? 4 : (format == GL_RGB || format == 0x80E0) ? 3 : 0;
    if (!step) return;
    const bool     bgr = format == 0x80E1 || format == 0x80E0;
    const uint8_t* p   = static_cast<const uint8_t*>(px);
    for (int i = 0; i < 256; ++i, p += step) {
        const uint8_t r = bgr ? p[2] : p[0], b = bgr ? p[0] : p[2];
        if (r != kMarker[0] || p[1] != kMarker[1] || b != kMarker[2]) return;
    }
    GLint id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
    g_marker.store(GLuint(id));
    Log("[servers] main menu marker sprite is texture %d", id);
}

namespace srvgl {

// ---------------------------------------------------------------------------------------
// Page state (render thread only)

struct Tex {
    GLuint id = 0;
    int    w = 0, h = 0;
};
struct FontTex {
    srvpage::Font f;
    Tex           t;
};

bool                         g_loaded = false, g_load_failed = false;
FontTex                      g_main, g_data;
Tex                          g_hover, g_pick;  // main2.tga, main3.tga
std::vector<srvpage::Server> g_list;
std::string                  g_list_note;
bool                         g_open = false;
int                          g_top = 0, g_sel = -1, g_gone = 0;
bool                         g_down = false;  // the left button, last frame
uint64_t                     g_last_click_ms = 0;
int                          g_last_click_row = -1;
std::string                  g_status;
uint64_t                     g_status_until = 0;

std::string GameDir() {
    char p[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    if (char* s = strrchr(p, '\\')) *s = 0;
    return p;
}

void Status(const std::string& s, uint64_t ms = 4000) { g_status = s, g_status_until = GetTickCount64() + ms; }

Tex Upload(int w, int h, GLenum ifmt, GLenum fmt, const void* px) {
    Tex t;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
    glPopClientAttrib();
    t.w = w, t.h = h;
    return t;
}

bool LoadFont(const std::string& pkz, const char* name, FontTex& out) {
    if (!srvpage::ReadFnt(srvpage::ReadPkz(pkz, name), out.f) || !out.f.ok()) return false;
    out.t = Upload(out.f.aw, out.f.ah, 0x803C /*GL_ALPHA8*/, GL_ALPHA, out.f.alpha.data());
    out.f.alpha.clear();
    out.f.alpha.shrink_to_fit();
    return true;
}

bool LoadSprite(const std::string& pkz, const char* name, Tex& out) {
    srvpage::Image im;
    if (!srvpage::ReadTga(srvpage::ReadPkz(pkz, name), im)) return false;
    out = Upload(im.w, im.h, GL_RGBA8, 0x80E1 /*GL_BGRA*/, im.bgra.data());
    return true;
}

/// The game's font and sprites, out of its own ui.pkz. Once.
void LoadArt() {
    if (g_loaded || g_load_failed) return;
    const std::string pkz = GameDir() + "\\ui.pkz";
    const bool ok = LoadFont(pkz, "ui/main.fnt", g_main) && LoadFont(pkz, "ui/data.fnt", g_data) &&
                    LoadSprite(pkz, "ui/main2.tga", g_hover) && LoadSprite(pkz, "ui/main3.tga", g_pick);
    if (ok) g_loaded = true;
    else g_load_failed = true;
    Log("[servers] the game's font and sprites from %s: %s", pkz.c_str(), ok ? "read" : "FAILED");
}

/// MXB App's server book.
void LoadList() {
    g_list.clear();
    g_top = 0, g_sel = -1;
    char*  app = nullptr;
    size_t n   = 0;
    std::string path;
    if (_dupenv_s(&app, &n, "APPDATA") == 0 && app) path = std::string(app) + "\\com.frost.mxbikes\\servers.json";
    free(app);
    std::string text;
    if (FILE* f = path.empty() ? nullptr : _fsopen(path.c_str(), "rb", _SH_DENYNO)) {
        char buf[65536];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
        fclose(f);
    }
    g_list = srvpage::ReadServers(text);
    // The spam and cheat-ad servers go, by the rules that hide them in the game's own browser
    // (frostmod_serverfilter.yaml) - the same rules MXB App applies.
    size_t hidden = 0;
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(),
                                [&](const srvpage::Server& s) {
                                    frostmod::serverfilter::ServerInfo si;
                                    si.name       = s.name;
                                    si.ip         = s.address.substr(0, s.address.rfind(':'));
                                    si.port       = uint16_t(atoi(s.address.c_str() + s.address.rfind(':') + 1));
                                    si.players    = s.players;
                                    si.maxPlayers = s.max_players;
                                    si.locked     = s.passworded;
                                    const bool h  = !frostmod::serverfilter::ShouldHide(si).empty();
                                    hidden += h;
                                    return h;
                                }),
                 g_list.end());
    std::stable_sort(g_list.begin(), g_list.end(), srvpage::Before);
    Log("[servers] %d spam servers left out by the server filter", int(hidden));
    int busy = 0;
    for (const srvpage::Server& s : g_list) busy += s.players > 0;
    char b[160];
    if (g_list.empty()) sprintf_s(b, "No servers yet - open MXB App once so it can fetch the list");
    else sprintf_s(b, "%d servers, %d with riders   -   the list MXB App keeps", int(g_list.size()), busy);
    g_list_note = b;
    Log("[servers] %d servers from %s", int(g_list.size()), path.c_str());
}

// ---------------------------------------------------------------------------------------
// Drawing, in window pixels with (0,0) at the top left

int g_W = 1, g_H = 1;

void Quad(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0), glVertex2f(x1, y0), glVertex2f(x1, y1), glVertex2f(x0, y1);
    glEnd();
}

void Sprite(const Tex& t, float x0, float y0, float x1, float y1, float a = 1.0f) {
    if (!t.id) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glColor4f(1, 1, 1, a);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0), glVertex2f(x0, y0);
    glTexCoord2f(1, 0), glVertex2f(x1, y0);
    glTexCoord2f(1, 1), glVertex2f(x1, y1);
    glTexCoord2f(0, 1), glVertex2f(x0, y1);
    glEnd();
}

enum Align { LEFT, RIGHT, CENTRE };

/// `s` with its top at `y`, `size` pixels tall, in the game's font.
void Text(const FontTex& ft, const std::string& s, float x, float y, float size, Align al, float r, float g, float b,
          float a = 1.0f) {
    const srvpage::Font& f = ft.f;
    if (!ft.t.id || f.cell <= 0) return;
    const float w = srvpage::TextWidth(f, s, size), k = size / float(f.cell);
    if (al == RIGHT) x -= w;
    else if (al == CENTRE) x -= w * 0.5f;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, ft.t.id);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    for (unsigned char c : s) {
        const srvpage::Glyph& gl = f.g[c];
        if (!gl.ok) continue;
        if (gl.w > 0) {
            const float gx0 = x + float(gl.lsb) * k, gx1 = gx0 + float(gl.x1 - gl.x0) * k;
            const float gy0 = y, gy1 = y + float(gl.y1 - gl.y0) * k;
            const float u0 = float(gl.x0) / float(f.aw), u1 = float(gl.x1) / float(f.aw);
            const float v0 = float(gl.y0) / float(f.ah), v1 = float(gl.y1) / float(f.ah);
            glTexCoord2f(u0, v0), glVertex2f(gx0, gy0);
            glTexCoord2f(u1, v0), glVertex2f(gx1, gy0);
            glTexCoord2f(u1, v1), glVertex2f(gx1, gy1);
            glTexCoord2f(u0, v1), glVertex2f(gx0, gy1);
        }
        x += float(gl.advance()) * k;
    }
    glEnd();
}

// The bar's buttons, as the main menu draws its own: the game's sprite under the pointer, the
// label right-aligned in main.fnt at the menu's size.
struct Button {
    const char* label;
    float       r[4];
};
const Button kJoin    = {"Join", {0.5875f, srvpage::kBarY0, 0.7375f, srvpage::kBarY0 + 0.044444f}};
const Button kRefresh = {"Refresh", {0.7375f, srvpage::kBarY0, 0.8875f, srvpage::kBarY0 + 0.044444f}};
const Button kBack    = {"Back", {0.8875f, srvpage::kBarY0, 1.0f, srvpage::kBarY0 + 0.044444f}};

void DrawButton(const Button& b, float mx, float my, bool down, bool enabled) {
    const float x0 = b.r[0] * g_W, y0 = b.r[1] * g_H, x1 = b.r[2] * g_W, y1 = b.r[3] * g_H;
    const bool  over = enabled && srvpage::Inside(b.r, mx, my);
    if (over) Sprite(down ? g_pick : g_hover, x0, y0, x1, y1);
    const float c = enabled ? 240.0f / 255.0f : 0.45f;
    Text(g_main, b.label, x1 - 0.00625f * g_W, y0, 0.044444f * g_H, RIGHT, c, c, c);
}

// ---------------------------------------------------------------------------------------
// Join: MXB App restarts the game into the server

/// Where the join request goes: frostmod_join.txt beside FrostMod's log, where the master-list
/// feed already goes. FrostMod runs nothing itself: MXB App reads the request once the game has
/// closed, checks the address again, and starts the game with -directconnect, the way its own
/// Join button does.
std::string JoinFile() {
    std::string p = g_logPath;
    const size_t s = p.find_last_of('\\');
    return s == std::string::npos ? std::string() : p.substr(0, s + 1) + "frostmod_join.txt";
}

void Join(const srvpage::Server& s, HWND game) {
    const std::string addr = srvpage::SafeAddress(s.address);
    const std::string path = JoinFile();
    if (addr.empty() || path.empty()) return Status("That server can't be joined from here");
    const std::string tmp = path + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") || !f) return Status("Couldn't hand the join to MXB App");
    fprintf(f, "frostmod-join 1 %llu\n%s\t%s\n", (unsigned long long)time(nullptr) * 1000ull, addr.c_str(), s.name.c_str());
    fclose(f);
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return Status("Couldn't hand the join to MXB App");
    Log("[servers] join %s (%s): handed to MXB App, closing the game", addr.c_str(), s.name.c_str());
    Status("Joining " + s.name + " - MXB App restarts the game into it", 30000);
    if (game) PostMessageW(game, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------------------
// A test shot: frostmod_shot.flag beside the log asks for the next frame as frostmod_shot.tga.
// A fullscreen game can't be captured from outside, and the page needs proving on the real
// frame. Looked for twice a second; the read-back only happens when asked.

void MaybeShot(int w, int h) {
    static uint64_t next = 0;
    const uint64_t  now  = GetTickCount64();
    if (now < next) return;
    next = now + 500;
    std::string dir = g_logPath;
    const size_t s = dir.find_last_of('\\');
    if (s == std::string::npos) return;
    dir.resize(s + 1);
    const std::string flag = dir + "frostmod_shot.flag";
    if (GetFileAttributesA(flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(flag.c_str());
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, px.data());
    glPopClientAttrib();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    const uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 8};
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "frostmod_shot.tga").c_str(), "wb") || !f) return;
    fwrite(hdr, 1, sizeof(hdr), f);  // bottom row first, as GL reads it
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    Log("[servers] test shot written (%dx%d)", w, h);
}

// ---------------------------------------------------------------------------------------
// The frame

/// Every presented frame in the menus, from the swap hook.
void Frame(HDC hdc) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    struct Shot {  // after this frame's page is drawn, whichever way Frame returns
        HDC dc;
        ~Shot() {
            RECT r{};
            if (HWND w = WindowFromDC(dc); w && GetClientRect(w, &r) && r.right > 0 && r.bottom > 0) MaybeShot(r.right, r.bottom);
        }
    } shot{hdc};
    const uint64_t now_ms = GetTickCount64();
    const bool     seen   = g_marker.load(std::memory_order_relaxed) &&
                      now_ms - g_marker_ms.load(std::memory_order_relaxed) < 500;
    ++g_frame;
    HWND hwnd = WindowFromDC(hdc);
    RECT rc{};
    POINT pt{};
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    g_W = rc.right, g_H = rc.bottom;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    const float mx = float(pt.x) / float(g_W), my = float(pt.y) / float(g_H);
    const bool  focus = GetForegroundWindow() == hwnd;
    const bool  down  = focus && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool  click = g_down && !down;  // released this frame
    g_down            = down;
    // Under the pointer the game draws our button's hover sprite instead of the marker, so the
    // marker goes missing exactly when the button is clicked. Whether the pointer came onto the
    // button from the main menu is what counts while it stays there.
    static bool over_prev = false, entered_on_menu = false;
    const bool  over = srvpage::Inside(srvpage::kButton, mx, my);
    if (over && !over_prev) entered_on_menu = seen;
    over_prev       = over;
    const bool menu = over ? entered_on_menu : seen;

    if (!g_open) {
        if (menu && click && srvpage::Inside(srvpage::kButton, mx, my)) {
            LoadArt();
            if (g_loaded) {
                LoadList();
                g_open = true, g_gone = 0;
                Log("[servers] page opened");
            }
        }
        if (!g_open) return;
    }
    // The game moved on (Testing, Settings...): the page goes with the main menu.
    g_gone = menu ? 0 : g_gone + 1;
    if (g_gone > 3) {
        g_open = false;
        Log("[servers] page closed with the main menu");
        return;
    }

    const int count = int(g_list.size());
    // Input
    if (click) {
        const int row = srvpage::RowAt(mx, my, g_top, count);
        if (mx > 0.975f && my >= srvpage::kListY0 && my < srvpage::kListY0 + srvpage::kRows * srvpage::kRowH && count > 0) {
            // the scroll strip on the right edge: jump to that share of the list
            const float k = (my - srvpage::kListY0) / (srvpage::kRows * srvpage::kRowH);
            g_top = (std::max)(0, (std::min)(count - srvpage::kRows, int(k * float(count)) - srvpage::kRows / 2));
        } else if (row >= 0) {
            const uint64_t now = GetTickCount64();
            if (row == g_last_click_row && now - g_last_click_ms < 450) Join(g_list[size_t(row)], hwnd);
            g_sel = row, g_last_click_row = row, g_last_click_ms = now;
        } else if (srvpage::Inside(kJoin.r, mx, my) && g_sel >= 0 && g_sel < count) {
            Join(g_list[size_t(g_sel)], hwnd);
        } else if (srvpage::Inside(kRefresh.r, mx, my)) {
            LoadList();
            Status("List read again", 2000);
        } else if (srvpage::Inside(kBack.r, mx, my)) {
            g_open = false;
            Log("[servers] page closed");
            return;
        }
    }
    if (focus) {
        static bool held[4] = {};
        const int   keys[4] = {VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT};
        const int   step[4] = {-1, 1, -srvpage::kRows, srvpage::kRows};
        for (int i = 0; i < 4; ++i) {
            const bool k = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (k && !held[i]) g_top += step[i];
            held[i] = k;
        }
        g_top = (std::max)(0, (std::min)(g_top, count - srvpage::kRows));
        if (g_top < 0) g_top = 0;
    }

    // Draw
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, g_W, g_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, g_W, g_H);
    glDisable(GL_DEPTH_TEST), glDisable(GL_CULL_FACE), glDisable(GL_LIGHTING), glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST), glDisable(GL_SCISSOR_TEST), glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    const float W = float(g_W), H = float(g_H), x0 = srvpage::kPanelX0 * W;
    const float hi = 240.0f / 255.0f, dim = 0.62f;
    Quad(x0, 0, W, H, 0, 0, 0, 0.78f);
    Text(g_main, "Servers", 0.235f * W, 0.035f * H, 0.066667f * H, LEFT, hi, hi, hi);
    Text(g_data, g_list_note, 0.237f * W, 0.105f * H, 0.024f * H, LEFT, dim, dim, dim);

    const float cName = 0.235f, cTrack = 0.585f, cRiders = 0.81f, cClass = 0.83f, rowSize = 0.0255f;
    const float hy = 0.142f * H;
    Text(g_data, "Server", cName * W, hy, 0.021f * H, LEFT, dim, dim, dim);
    Text(g_data, "Track", cTrack * W, hy, 0.021f * H, LEFT, dim, dim, dim);
    Text(g_data, "Riders", cRiders * W, hy, 0.021f * H, RIGHT, dim, dim, dim);
    Text(g_data, "Class", cClass * W, hy, 0.021f * H, LEFT, dim, dim, dim);
    Quad(x0 + 0.0225f * W, srvpage::kListY0 * H - 3, 0.97f * W, srvpage::kListY0 * H - 2, 1, 1, 1, 0.18f);

    const int hover = srvpage::RowAt(mx, my, g_top, count);
    for (int i = 0; i < srvpage::kRows && g_top + i < count; ++i) {
        const int              idx = g_top + i;
        const srvpage::Server& s   = g_list[size_t(idx)];
        const float            ry0 = (srvpage::kListY0 + i * srvpage::kRowH) * H, ry1 = ry0 + srvpage::kRowH * H;
        if (idx == g_sel) Sprite(g_pick, x0 + 0.0125f * W, ry0, 0.97f * W, ry1);
        else if (idx == hover) Sprite(g_hover, x0 + 0.0125f * W, ry0, 0.97f * W, ry1, 0.8f);
        const float ty = ry0 + (srvpage::kRowH - rowSize) * 0.5f * H, sz = rowSize * H;
        const float c  = s.joinable ? hi : 0.45f;
        Text(g_data, srvpage::Fit(g_data.f, s.name + (s.passworded ? "  [pw]" : ""), sz, (cTrack - cName - 0.015f) * W), cName * W, ty,
             sz, LEFT, c, c, c);
        Text(g_data, srvpage::Fit(g_data.f, s.track, sz, (cRiders - cTrack - 0.06f) * W), cTrack * W, ty, sz, LEFT, c, c, c);
        char riders[24];
        sprintf_s(riders, "%d/%d", s.players, s.max_players);
        const bool busy = s.players > 0;
        Text(g_data, riders, cRiders * W, ty, sz, RIGHT, busy ? 1.0f : c, busy ? 0.78f : c, busy ? 0.25f : c);
        Text(g_data, srvpage::Fit(g_data.f, s.category, sz, (0.965f - cClass) * W), cClass * W, ty, sz, LEFT, dim, dim, dim);
    }
    // the scroll strip
    if (count > srvpage::kRows) {
        const float sy0 = srvpage::kListY0 * H, sy1 = (srvpage::kListY0 + srvpage::kRows * srvpage::kRowH) * H;
        Quad(0.978f * W, sy0, 0.982f * W, sy1, 1, 1, 1, 0.12f);
        const float a = float(g_top) / float(count), b = float(g_top + srvpage::kRows) / float(count);
        Quad(0.976f * W, sy0 + a * (sy1 - sy0), 0.984f * W, sy0 + b * (sy1 - sy0), 1, 1, 1, 0.55f);
    }

    // the bar
    Quad(x0, srvpage::kBarY0 * H, W, (srvpage::kBarY0 + 0.044444f) * H, 0, 0, 0, 0.35f);
    char range[64];
    sprintf_s(range, count ? "%d-%d of %d" : "", g_top + 1, (std::min)(count, g_top + srvpage::kRows), count);
    Text(g_data, range, 0.237f * W, (srvpage::kBarY0 + 0.011f) * H, 0.022f * H, LEFT, dim, dim, dim);
    if (GetTickCount64() < g_status_until)
        Text(g_data, g_status, 0.36f * W, (srvpage::kBarY0 + 0.011f) * H, 0.022f * H, LEFT, 1.0f, 0.78f, 0.25f);
    DrawButton(kJoin, mx, my, down, g_sel >= 0);
    DrawButton(kRefresh, mx, my, down, true);
    DrawButton(kBack, mx, my, down, true);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

}  // namespace srvgl

/// At init: on only when the override main.mnu with our button sits beside mxbikes.exe.
static void SrvInstall(HMODULE gl) {
    const std::string mnu = srvgl::GameDir() + "\\ui\\main.mnu";
    bool              ours = false;
    if (FILE* f = _fsopen(mnu.c_str(), "rb", _SH_DENYNO)) {
        char   buf[32768];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        ours   = strstr(buf, "id_frost_servers") != nullptr;
    }
    if (!ours) return;
    if (!gl) return Log("[servers] the Servers button is in main.mnu, but opengl32 isn't loaded - page off");
    int ok = 0;
    if (!g_mdoTexImage2D) {
        if (auto p = GetProcAddress(gl, "glTexImage2D"))
            ok += InstallHook((void*)p, (void*)&hkMdTexImage2D, (void**)&g_mdoTexImage2D, "servers opengl32!glTexImage2D");
    } else ++ok;
    if (auto p = GetProcAddress(gl, "glBindTexture"))
        ok += InstallHook((void*)p, (void*)&srvgl::hkBindTexture, (void**)&srvgl::g_origBind, "servers opengl32!glBindTexture");
    srvgl::g_on.store(ok == 2);
    Log("[servers] the Servers button is in main.mnu: page %s", ok == 2 ? "on" : "OFF (a texture hook failed)");
}
