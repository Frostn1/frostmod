// srvpage_gl.h - the in-game MXB page: the hooks, the input, the pipe to MXB App and the draw.
// Included once, from frostmod.cpp, after the texture hooks (hkMdTexImage2D) and before the
// swap hook that calls srvgl::Frame. The pure parts (font, sprites, JSON, layout) are in
// srvpage.h.
//
// An "MXB" button on the game's main menu opens a page in the game's own look with what the
// game's browser can't do: Friends (who's online and on which server, join them, add one by
// code) and Install & Join (any server, even on a track you don't have: MXB App installs it,
// then joins). Everything comes from MXB App over its local pipe; without the app running the
// page says so.
//
// How the page knows the main menu is up. The game has no hook for "which screen is this", so
// the override main.mnu gives our button an idle sprite of its own, frostsrv1.tga: 16 x 16, every
// pixel the colour kMarker with alpha 0, so it draws nothing. Its upload is spotted in
// glTexImage2D (that size, every pixel that colour) and its texture name kept; a frame in which
// the game binds that name is a frame with the main menu on screen.
//
// Input is polled (GetAsyncKeyState, GetCursorPos) like the rest of FrostMod's panels. Nothing is
// swallowed: on the main menu the right of the screen has nothing of the game's to click, and
// the game's own column stays live - pick Testing and the page closes with the menu.
//
// Joining: the game reads -directconnect only at startup, so MXB App does it - installing first
// when something is missing, with the progress shown here - then says "restart", FrostMod closes
// the game, and the app starts it again into the server. FrostMod starts no process itself.
//
// Cost: a pointer compare per glBindTexture, a few calls a frame on the main menu, the page's
// quads only while it is open, and a pipe thread that sleeps 30 ms between polls once started.

#include <windowsx.h>  // GET_X_LPARAM

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
std::vector<srvpage::Server> g_list;
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

/// The game's font and sprites, out of its own ui.pkz. Once.
void LoadArt() {
    if (g_loaded || g_load_failed) return;
    const std::string pkz = GameDir() + "\\ui.pkz";
    const bool ok = LoadFont(pkz, "ui/main.fnt", g_main) && LoadFont(pkz, "ui/data.fnt", g_data);
    if (ok) g_loaded = true;
    else g_load_failed = true;
    Log("[servers] the game's font and sprites from %s: %s", pkz.c_str(), ok ? "read" : "FAILED");
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

// The page's colours (srvpage::Theme): the game's look unless frostmod_page.ini says otherwise.
srvpage::Theme g_theme = srvpage::GameTheme();

void LoadTheme() {
    std::string dir = g_logPath;
    const size_t s = dir.find_last_of('\\');
    std::string  ini;
    if (s != std::string::npos) {
        if (FILE* f = _fsopen((dir.substr(0, s + 1) + "frostmod_page.ini").c_str(), "rb", _SH_DENYNO)) {
            char   buf[8192];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            ini.assign(buf, n);
        }
    }
    g_theme = srvpage::ReadTheme(ini);
}

void Fill(const float r[4], const srvpage::Rgba& c) { Quad(r[0] * g_W, r[1] * g_H, r[2] * g_W, r[3] * g_H, c.r, c.g, c.b, c.a); }

void Say(const FontTex& f, const std::string& s, float x, float y, float size, Align al, const srvpage::Rgba& c) {
    Text(f, s, x * g_W, y * g_H, size * g_H, al, c.r, c.g, c.b, c.a);
}

/// A button as the game draws them: a flat block, its label right-aligned in main.fnt, lighter
/// under the pointer. `fill`/`hover` default to the theme's grey buttons.
void DrawButton(const char* label, const float r[4], float mx, float my, bool enabled, const srvpage::Rgba* fill = nullptr,
                const srvpage::Rgba* hover = nullptr) {
    const bool over = enabled && srvpage::Inside(r, mx, my);
    Fill(r, over ? (hover ? *hover : g_theme.button_hover) : (fill ? *fill : g_theme.button));
    const float h = (r[3] - r[1]);
    Say(g_main, label, r[2] - 0.00625f, r[1] + (h - srvpage::kTextSize) * 0.5f, srvpage::kTextSize, RIGHT,
        enabled ? g_theme.button_text : g_theme.button_off);
}

/// A label in the bottom bar, as Back / Join sit in the game's: on a coloured block.
void DrawBarButton(const char* label, const float r[4], float mx, float my, bool enabled, bool left, const srvpage::Rgba& fill,
                   const srvpage::Rgba& hover) {
    const bool over = enabled && srvpage::Inside(r, mx, my);
    if (enabled) Fill(r, over ? hover : fill);
    const float h = (r[3] - r[1]);
    Say(g_main, label, left ? r[0] + 0.00625f : r[2] - 0.00625f, r[1] + (h - srvpage::kTextSize) * 0.5f, srvpage::kTextSize,
        left ? LEFT : RIGHT, enabled ? g_theme.bar_text : g_theme.button_off);
}

// ---------------------------------------------------------------------------------------
// MXB App, over its local pipe
//
// \\.\pipe\mxbapp-ingame, served by MXB App for the current user. One JSON message a line
// each way. FrostMod asks ("hello", "servers", "friends", "join", "friend_add"); the app
// answers with "re" set to what was asked, and pushes "join" progress as it installs and
// restarts. Nothing here blocks the render thread: a worker owns the pipe and the page reads
// and writes two queues.

namespace pipe {

std::mutex               g_mu;
std::deque<std::string>  g_out;  // lines to send
std::deque<std::string>  g_in;   // lines received
std::atomic<bool>        g_connected{false};
std::atomic<bool>        g_started{false};

void Send(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_out.size() < 64) g_out.push_back(line + "\n");
}

DWORD WINAPI Worker(LPVOID) {
    std::string partial;
    for (;;) {
        HANDLE h = CreateFileW(L"\\\\.\\pipe\\mxbapp-ingame", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            g_connected = false;
            Sleep(2000);
            continue;
        }
        g_connected = true;
        Log("[mxb] connected to MXB App");
        partial.clear();
        bool alive = true;
        while (alive) {
            std::deque<std::string> out;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                out.swap(g_out);
            }
            for (const std::string& l : out) {
                DWORD n = 0;
                if (!WriteFile(h, l.data(), DWORD(l.size()), &n, nullptr)) {
                    alive = false;
                    break;
                }
            }
            DWORD avail = 0;
            if (alive && !PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) alive = false;
            while (alive && avail > 0) {
                char  buf[4096];
                DWORD got = 0;
                if (!ReadFile(h, buf, (std::min)(avail, DWORD(sizeof(buf))), &got, nullptr) || got == 0) {
                    alive = false;
                    break;
                }
                avail -= (std::min)(avail, got);
                partial.append(buf, got);
                size_t nl;
                while ((nl = partial.find('\n')) != std::string::npos) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (g_in.size() < 64) g_in.push_back(partial.substr(0, nl));
                    partial.erase(0, nl + 1);
                }
                if (partial.size() > (8u << 20)) partial.clear();  // a line that never ends
            }
            if (alive) Sleep(30);
        }
        CloseHandle(h);
        g_connected = false;
        Log("[mxb] MXB App went away");
        Sleep(1000);
    }
}

void Start() {
    if (g_started.exchange(true)) return;
    if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
}

std::vector<std::string> Drain() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::string> v(g_in.begin(), g_in.end());
    g_in.clear();
    return v;
}

}  // namespace pipe

// ---------------------------------------------------------------------------------------
// The page's data, as MXB App last sent it (render thread only)

enum Tab { FRIENDS, SERVERS };
Tab                          g_tab = FRIENDS;
std::vector<srvpage::Friend> g_friends;
bool                         g_have_friends = false, g_have_servers = false;
std::string                  g_app;           // what the app says it is, once it says hello
std::string                  g_friends_note;  // e.g. "sign in to MXB App to see friends"
bool                         g_adding = false;  // the add-friend code box is open
std::string                  g_code;
uint64_t                     g_next_friends = 0;

bool Spam(const srvpage::Server& s) {
    frostmod::serverfilter::ServerInfo si;
    si.name       = s.name;
    si.ip         = s.address.substr(0, s.address.rfind(':'));
    si.port       = uint16_t(atoi(s.address.c_str() + s.address.rfind(':') + 1));
    si.players    = s.players;
    si.maxPlayers = s.max_players;
    si.locked     = s.passworded;
    return !frostmod::serverfilter::ShouldHide(si).empty();
}

void Ask() {
    pipe::Send("{\"op\":\"hello\",\"v\":1,\"frostmod\":\"" FROSTMOD_VERSION "\"}");
    pipe::Send("{\"op\":\"friends\"}");
    pipe::Send("{\"op\":\"servers\"}");
    g_next_friends = GetTickCount64() + 10000;
}

void Handle(const std::string& line, HWND game) {
    bool             ok = false;
    const srvpage::J m  = srvpage::ParseJson(line, &ok);
    if (!ok || m.t != srvpage::J::OBJ) return;
    const std::string re = m["re"].str();
    if (re == "hello") {
        g_app = srvpage::Ascii(m["app"].str());
    } else if (re == "servers") {
        std::vector<srvpage::Server> list;
        size_t                       hidden = 0;
        for (const srvpage::J& e : m["servers"].a) {
            srvpage::Server s = srvpage::ServerOf(e);
            if (s.address.empty()) continue;
            if (Spam(s)) {
                ++hidden;
                continue;
            }
            list.push_back(std::move(s));
        }
        std::stable_sort(list.begin(), list.end(), srvpage::Before);
        g_list = std::move(list), g_have_servers = true;
        if (g_tab == SERVERS) g_top = 0, g_sel = -1;
        Log("[mxb] %d servers from MXB App, %d spam left out", int(g_list.size()), int(hidden));
    } else if (re == "friends") {
        g_friends      = srvpage::ReadFriends(m["friends"]);
        g_friends_note = srvpage::Ascii(m["note"].str());
        g_have_friends = true;
        std::stable_sort(g_friends.begin(), g_friends.end(), [](const srvpage::Friend& a, const srvpage::Friend& b) {
            if (a.has_server != b.has_server) return a.has_server;
            if (a.online != b.online) return a.online;
            return a.name < b.name;
        });
    } else if (re == "join") {
        const std::string state = m["state"].str(), text = srvpage::Ascii(m["text"].str());
        const int         pct   = int(m["pct"].num(-1));
        char              b[200];
        if (pct >= 0) sprintf_s(b, "%s  %d%%", text.c_str(), pct);
        else sprintf_s(b, "%s", text.c_str());
        Status(b, state == "error" ? 8000 : 60000);
        Log("[mxb] join: %s - %s", state.c_str(), b);
        // The app has what it needs and will start the game again into the server.
        if (state == "restart" && game) PostMessageW(game, WM_CLOSE, 0, 0);
    } else if (re == "friend_add") {
        Status(srvpage::Ascii(m["text"].str()), 6000);
        pipe::Send("{\"op\":\"friends\"}");
    }
}

void Join(const srvpage::Server& s) {
    const std::string addr = srvpage::SafeAddress(s.address);
    if (addr.empty()) return Status("That server can't be joined from here");
    if (!pipe::g_connected) return Status("Open MXB App to join from here", 6000);
    pipe::Send("{\"op\":\"join\",\"address\":" + srvpage::Quote(addr) + ",\"name\":" + srvpage::Quote(s.name) + "}");
    Status(s.missing.empty() ? "Joining " + s.name + "..." : "Installing what " + s.name + " needs...", 60000);
    Log("[mxb] join %s (%s) asked of MXB App", addr.c_str(), s.name.c_str());
}

// ---------------------------------------------------------------------------------------
// The rows each tab shows

struct Row {
    std::string            c[5];
    srvpage::Rgba          col[5];
    const srvpage::Server* server = nullptr;  // what Join joins; null when nothing to join
};

std::vector<Row> Rows() {
    std::vector<Row> rows;
    const srvpage::Theme& t = g_theme;
    auto need_text = [](const srvpage::Server& s) {
        std::string need;
        for (const std::string& m : s.missing) need += (need.empty() ? "needs " : " + ") + m;
        return need;
    };
    if (g_tab == FRIENDS) {
        for (const srvpage::Friend& f : g_friends) {
            Row r;
            r.c[0] = f.name, r.col[0] = f.online ? t.row_text : t.muted;
            if (f.has_server) {
                r.c[1] = f.on.name, r.col[1] = t.row_text;
                r.c[2] = f.on.track, r.col[2] = t.row_text;
                char riders[24];
                sprintf_s(riders, "%d/%d", f.on.players, f.on.max_players);
                r.c[3] = riders, r.col[3] = t.row_text;
                const std::string need = need_text(f.on);
                r.c[4] = need.empty() ? "ready" : need, r.col[4] = need.empty() ? t.ready : t.needs;
                r.server = &f.on;
            } else {
                r.c[1] = f.online ? "In the menus" : "Offline", r.col[1] = t.muted;
            }
            rows.push_back(r);
        }
    } else {
        for (const srvpage::Server& s : g_list) {
            Row r;
            const srvpage::Rgba c = s.joinable ? t.row_text : t.muted;
            r.c[0] = s.name + (s.passworded ? "  [pw]" : ""), r.col[0] = c;
            r.c[1] = s.track, r.col[1] = c;
            r.c[2] = s.category, r.col[2] = c;
            char riders[24];
            sprintf_s(riders, "%d/%d", s.players, s.max_players);
            r.c[3] = riders, r.col[3] = s.players > 0 ? t.busy : c;
            const std::string need = need_text(s);
            r.c[4] = need.empty() ? "ready" : need, r.col[4] = need.empty() ? t.ready : t.needs;
            r.server = &s;
            rows.push_back(r);
        }
    }
    return rows;
}

// ---------------------------------------------------------------------------------------
// Input the game must not see
//
// The page covers the game's whole main menu, buttons included, so while it is open the game
// may not have the mouse or the keyboard: the game's window procedure is wrapped and, while the
// page is up, mouse buttons, the wheel and keys stop here. On the main menu a click on Race
// stops here too and opens the page instead of the game's server browser. Classic hands one
// click on Race back to the game. The wheel scrolls the list; typed characters fill the
// friend-code box.

WNDPROC               g_game_proc = nullptr;
HWND                  g_game_wnd  = nullptr;
std::atomic<bool>     g_swallow{false};        // the page is open
std::atomic<bool>     g_race_armed{false};     // the main menu is up: Race is ours
std::atomic<bool>     g_open_asked{false};     // Race was clicked
std::atomic<uint64_t> g_pass_until{0};         // Classic: let the game have Race until then
std::atomic<int>      g_wheel{0};              // wheel notches since the last frame
std::mutex            g_typed_mu;
std::string           g_typed;                 // characters typed since the last frame
std::atomic<bool>     g_esc{false}, g_enter{false};

bool OverRace(HWND w, LPARAM lp) {
    RECT rc{};
    if (!GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    const float x = float(GET_X_LPARAM(lp)) / float(rc.right), y = float(GET_Y_LPARAM(lp)) / float(rc.bottom);
    return srvpage::Inside(srvpage::kButton, x, y);
}

LRESULT CALLBACK PageProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    const bool pass = GetTickCount64() < g_pass_until.load();
    if (!pass && g_race_armed && !g_swallow && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK) &&
        OverRace(w, lp)) {
        if (m == WM_LBUTTONUP) g_open_asked = true;
        return 0;
    }
    if (g_swallow) {
        switch (m) {
            case WM_MOUSEWHEEL: g_wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP: return 0;
            case WM_CHAR: {
                std::lock_guard<std::mutex> lk(g_typed_mu);
                if (wp >= 32 && wp < 127 && g_typed.size() < 64) g_typed += char(wp);
                else if (wp == 8) g_typed += '\b';
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) g_esc = true;
                if (wp == VK_RETURN) g_enter = true;
                return 0;
            case WM_KEYUP: case WM_SYSKEYDOWN: if (m == WM_KEYUP) return 0; break;
        }
    }
    return CallWindowProcW(g_game_proc, w, m, wp, lp);
}

void Hook(HWND w) {
    if (g_game_proc || !w) return;
    g_game_wnd  = w;
    g_game_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PageProc)));
    Log("[mxb] input hook %s", g_game_proc ? "in" : "FAILED");
}

/// Classic: the game's own server browser, by handing it one click on Race.
void Classic(HWND w) {
    RECT rc{};
    if (!GetClientRect(w, &rc)) return;
    const int x = int(rc.right * (srvpage::kButton[0] + srvpage::kButton[2]) * 0.5f);
    const int y = int(rc.bottom * (srvpage::kButton[1] + srvpage::kButton[3]) * 0.5f);
    POINT     pt{x, y};
    ClientToScreen(w, &pt);
    SetCursorPos(pt.x, pt.y);
    g_pass_until = GetTickCount64() + 600;
    PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
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

const char* const kTabName[3] = {"Friends", "Servers", "Classic"};

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
    if (seen) Hook(hwnd);
    g_W = rc.right, g_H = rc.bottom;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    const float mx = float(pt.x) / float(g_W), my = float(pt.y) / float(g_H);
    const bool  focus = GetForegroundWindow() == hwnd;
    const bool  down  = focus && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool  click = g_down && !down;  // released this frame
    g_down            = down;
    // Under the pointer the game draws Race's hover sprite instead of the marker, so the marker
    // goes missing exactly when Race is clicked. Whether the pointer came onto Race from the main
    // menu is what counts while it stays there.
    static bool over_prev = false, entered_on_menu = false;
    const bool  over = srvpage::Inside(srvpage::kButton, mx, my);
    if (over && !over_prev) entered_on_menu = seen;
    over_prev       = over;
    const bool menu = over ? entered_on_menu : seen;
    g_race_armed    = menu && g_game_proc != nullptr;

    // Whatever MXB App sent since the last frame, page open or not (join progress included).
    for (const std::string& line : pipe::Drain()) Handle(line, hwnd);

    if (!g_open) {
        // Race, caught in the window procedure (or, without it, seen here).
        const bool asked = g_open_asked.exchange(false) || (!g_game_proc && menu && click && over);
        if (asked && menu) {
            LoadArt();
            if (g_loaded) {
                LoadTheme();
                pipe::Start();
                if (pipe::g_connected) Ask();
                g_open = true, g_gone = 0, g_top = 0, g_sel = -1, g_tab = FRIENDS;
                g_swallow = true, g_wheel = 0, g_esc = false, g_enter = false;
                Log("[mxb] page opened");
            }
        }
        if (!g_open) return;
    }
    auto close = [&](const char* why) {
        g_open = false, g_adding = false, g_swallow = false;
        Log("[mxb] page closed (%s)", why);
    };
    // The game moved on (a join restarting it, a dialog): the page goes with the main menu.
    g_gone = menu ? 0 : g_gone + 1;
    if (g_gone > 30) return close("the main menu went away");
    // Connected late (MXB App started after the page opened), and the friends list kept fresh.
    static bool was_connected = false;
    if (pipe::g_connected && !was_connected) Ask();
    was_connected = pipe::g_connected;
    if (pipe::g_connected && g_tab == FRIENDS && now_ms > g_next_friends) {
        pipe::Send("{\"op\":\"friends\"}");
        g_next_friends = now_ms + 10000;
    }

    const std::vector<Row> rows  = Rows();
    const int              count = int(rows.size());
    if (g_sel >= count) g_sel = -1;
    const srvpage::Server* target = g_sel >= 0 ? rows[size_t(g_sel)].server : nullptr;
    const bool             needs  = target && !target->missing.empty();
    const bool             can_go = target && pipe::g_connected &&
                        std::find(target->missing.begin(), target->missing.end(), "bike") == target->missing.end();

    // Keys: Esc closes (or cancels the friend code), typing fills the code box.
    std::string typed;
    {
        std::lock_guard<std::mutex> lk(g_typed_mu);
        typed.swap(g_typed);
    }
    if (g_adding) {
        for (char c : typed) {
            if (c == '\b') {
                if (!g_code.empty()) g_code.pop_back();
            } else if (g_code.size() < 16 && (isalnum((unsigned char)c) || c == '-')) {
                g_code += char(toupper((unsigned char)c));
            }
        }
        if (g_enter.exchange(false) && !g_code.empty()) {
            pipe::Send("{\"op\":\"friend_add\",\"code\":" + srvpage::Quote(g_code) + "}");
            Status("Sending the friend request...", 6000);
            g_adding = false, g_code.clear();
        }
        if (g_esc.exchange(false)) g_adding = false, g_code.clear();
    } else {
        g_enter = false;
        if (g_esc.exchange(false)) return close("Esc");
    }
    g_top -= g_wheel.exchange(0) * 3;

    // Clicks
    if (click) {
        const int row = srvpage::RowAt(mx, my, g_top, count);
        float     tab[4];
        int       hit_tab = -1;
        for (int i = 0; i < 3; ++i) {
            srvpage::TabRect(i, tab);
            if (srvpage::Inside(tab, mx, my)) hit_tab = i;
        }
        if (hit_tab == 2) {
            close("Classic");
            Classic(hwnd);
            return;
        } else if (hit_tab >= 0 && hit_tab != int(g_tab)) {
            g_tab = Tab(hit_tab), g_top = 0, g_sel = -1, g_adding = false;
            if (pipe::g_connected) pipe::Send(g_tab == FRIENDS ? "{\"op\":\"friends\"}" : "{\"op\":\"servers\"}");
        } else if (srvpage::Inside(srvpage::kBack, mx, my)) {
            return close("Back");
        } else if (srvpage::Inside(srvpage::kGo, mx, my) && can_go) {
            Join(*target);
        } else if (srvpage::Inside(srvpage::kLowBtn, mx, my)) {
            if (pipe::g_connected) Ask();
            Status(pipe::g_connected ? "Asked MXB App again" : "Open MXB App to use this", 2500);
        } else if (g_tab == FRIENDS && srvpage::Inside(srvpage::kTopBtn, mx, my) && pipe::g_connected) {
            g_adding = !g_adding, g_code.clear();
        } else if (mx > srvpage::kList[2] - 0.012f && my >= srvpage::kListY0 && my < srvpage::kList[3] && count > srvpage::kRows) {
            const float k = (my - srvpage::kListY0) / (srvpage::kList[3] - srvpage::kListY0);
            g_top = int(k * float(count)) - srvpage::kRows / 2;
        } else if (row >= 0) {
            if (row == g_last_click_row && now_ms - g_last_click_ms < 450 && rows[size_t(row)].server && pipe::g_connected)
                Join(*rows[size_t(row)].server);
            g_sel = row, g_last_click_row = row, g_last_click_ms = now_ms;
        }
    }
    g_top = (std::max)(0, (std::min)(g_top, count - srvpage::kRows));

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

    const srvpage::Theme& t = g_theme;
    // The main menu's column is under the page: the rest of the screen is dimmed to match it,
    // and the panel is solid where the column's buttons would show through.
    const float right[4] = {0.2125f, 0.0f, 1.0f, 1.0f};
    Fill(right, srvpage::Rgba{0, 0, 0, 0.5f});
    Fill(srvpage::kTitle, t.title_bg);
    Say(g_main, g_tab == FRIENDS ? "Friends" : "Install & Join", 0.9875f, 0.0111f, 0.066667f, RIGHT, t.title_text);
    srvpage::Rgba solid = t.panel;
    solid.a             = 1.0f;
    const float column[4] = {srvpage::kPanel[0], srvpage::kPanel[1], 0.2125f, srvpage::kPanel[3]};
    Fill(column, solid);
    Fill(srvpage::kPanel, t.panel);

    // Above the list: the tab's button on the left, what's going on on the right.
    if (g_tab == FRIENDS) DrawButton(g_adding ? "Cancel" : "Add Friend", srvpage::kTopBtn, mx, my, pipe::g_connected);
    std::string   note;
    srvpage::Rgba note_c = t.header_text;
    if (!pipe::g_connected) note = "Open MXB App on this PC to use this page", note_c = t.note_warn;
    else if (g_adding) note = "Friend code:  " + g_code + ((now_ms / 500) % 2 ? "_" : " ") + "     Enter to send", note_c = t.header_text;
    else if (g_tab == FRIENDS) {
        if (!g_have_friends) note = "Asking MXB App for your friends...";
        else if (!g_friends_note.empty()) note = g_friends_note, note_c = t.note_warn;
        else {
            int on = 0;
            for (const srvpage::Friend& f : g_friends) on += f.online;
            note = std::to_string(g_friends.size()) + " friends, " + std::to_string(on) + " online - join one, even on a track you don't have";
        }
    } else note = g_have_servers ? "A track you don't have? MXB App installs it, then joins" : "Asking MXB App for the servers...";
    Say(g_main, srvpage::Fit(g_main.f, note, srvpage::kTextSize * g_H, 0.8f * g_W), g_adding ? 0.15f : 0.9875f, 0.15f,
        srvpage::kTextSize, g_adding ? LEFT : RIGHT, note_c);

    // The list: headers, rows, the scroll bar.
    Fill(srvpage::kList, t.list_bg);
    const float hb[4] = {srvpage::kList[0], srvpage::kList[1], srvpage::kList[2], srvpage::kList[1] + srvpage::kHeaderH};
    Fill(hb, t.header_bg);
    const bool  fr        = g_tab == FRIENDS;
    const float cx[5]     = {0.0175f, fr ? 0.22f : 0.40f, fr ? 0.52f : 0.60f, 0.78f, 0.80f};
    const float cw[5]     = {fr ? 0.19f : 0.37f, fr ? 0.28f : 0.19f, fr ? 0.22f : 0.13f, 0.06f, 0.17f};
    const char* head[2][5] = {{"Friend", "Server", "Track", "Riders", "Needs"}, {"Name", "Track", "Class", "Riders", "Needs"}};
    for (int c = 0; c < 5; ++c)
        Say(g_main, head[fr ? 0 : 1][c], c == 3 ? cx[3] : cx[c], srvpage::kList[1] + 0.003f, srvpage::kRowText, c == 3 ? RIGHT : LEFT,
            t.header_text);
    const int hover = srvpage::RowAt(mx, my, g_top, count);
    for (int i = 0; i < srvpage::kRows && g_top + i < count; ++i) {
        const int   idx = g_top + i;
        const Row&  r   = rows[size_t(idx)];
        const float y0  = srvpage::kListY0 + float(i) * srvpage::kRowH;
        if (idx == g_sel) {
            const float sel[4] = {srvpage::kList[0], y0, srvpage::kList[2] - 0.012f, y0 + srvpage::kRowH};
            Fill(sel, t.row_pick_bg);
        }
        for (int c = 0; c < 5; ++c) {
            if (r.c[c].empty()) continue;
            const srvpage::Rgba col = idx == g_sel ? t.row_pick : idx == hover ? t.row_hover : r.col[c];
            Say(g_main, srvpage::Fit(g_main.f, r.c[c], srvpage::kRowText * g_H, cw[c] * g_W), cx[c], y0 + 0.001f, srvpage::kRowText,
                c == 3 ? RIGHT : LEFT, col);
        }
    }
    if (count == 0 && pipe::g_connected && (fr ? g_have_friends : g_have_servers))
        Say(g_main, fr ? "No friends yet - Add Friend with their code (MXB App > Servers > Friends shows yours)" : "No servers right now",
            0.0175f, srvpage::kListY0 + 0.004f, srvpage::kRowText, LEFT, t.muted);
    if (count > srvpage::kRows) {
        const float y0 = srvpage::kListY0, y1 = srvpage::kList[3];
        const float a = float(g_top) / float(count), b = float(g_top + srvpage::kRows) / float(count);
        const float track[4] = {srvpage::kList[2] - 0.008f, y0, srvpage::kList[2] - 0.003f, y1};
        const float thumb[4] = {srvpage::kList[2] - 0.009f, y0 + a * (y1 - y0), srvpage::kList[2] - 0.002f, y0 + b * (y1 - y0)};
        Fill(track, t.header_bg);
        Fill(thumb, t.button);
    }

    // Under the list: Refresh, and what MXB App is doing (join progress).
    DrawButton("Refresh", srvpage::kLowBtn, mx, my, true);
    if (GetTickCount64() < g_status_until)
        Say(g_main, srvpage::Fit(g_main.f, g_status, srvpage::kTextSize * g_H, 0.6f * g_W), 0.5f, 0.872f, srvpage::kTextSize, CENTRE,
            t.header_text);
    else if (count)
        Say(g_main, std::to_string(g_top + 1) + "-" + std::to_string((std::min)(count, g_top + srvpage::kRows)) + " of " + std::to_string(count),
            0.9875f, 0.872f, srvpage::kTextSize, RIGHT, t.header_text);

    // The tabs, under the panel as the game's Local / World are.
    for (int i = 0; i < 3; ++i) {
        float r[4];
        srvpage::TabRect(i, r);
        const bool on = i == int(g_tab);
        Fill(r, on ? t.tab_on : srvpage::Inside(r, mx, my) ? t.tab_on : t.tab);
        Say(g_main, kTabName[i], r[0] + 0.00625f, r[1] + (r[3] - r[1] - srvpage::kTextSize) * 0.5f, srvpage::kTextSize, LEFT,
            on ? t.tab_text_on : t.tab_text);
    }

    // The bottom bar: Back, and Join / Install & Join.
    Fill(srvpage::kBar, t.bar_bg);
    DrawBarButton("Back", srvpage::kBack, mx, my, true, true, t.back, t.back_hover);
    DrawBarButton(needs && can_go ? "Install & Join" : "Join", srvpage::kGo, mx, my, can_go, false, t.go, t.go_hover);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

}  // namespace srvgl

/// At init: on only when the override main.mnu with our marker on Race sits beside mxbikes.exe.
static void SrvInstall(HMODULE gl) {
    const std::string mnu = srvgl::GameDir() + "\\ui\\main.mnu";
    bool              ours = false;
    if (FILE* f = _fsopen(mnu.c_str(), "rb", _SH_DENYNO)) {
        char   buf[32768];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        ours   = strstr(buf, "frostsrv1.tga") != nullptr;
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
