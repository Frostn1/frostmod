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

void DrawButton(const Button& b, float mx, float my, bool down, bool enabled) {
    const float x0 = b.r[0] * g_W, y0 = b.r[1] * g_H, x1 = b.r[2] * g_W, y1 = b.r[3] * g_H;
    const bool  over = enabled && srvpage::Inside(b.r, mx, my);
    if (over) Sprite(down ? g_pick : g_hover, x0, y0, x1, y1);
    const float c = enabled ? 240.0f / 255.0f : 0.45f;
    Text(g_main, b.label, x1 - 0.00625f * g_W, y0, 0.044444f * g_H, RIGHT, c, c, c);
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
    std::string            c[4];
    float                  rgb[4][3];
    const srvpage::Server* server = nullptr;  // what Join joins; null when nothing to join
};

std::vector<Row> Rows() {
    std::vector<Row> rows;
    const float      hi = 240.0f / 255.0f, dim = 0.62f;
    auto             set = [](Row& r, int i, const std::string& t, float cr, float cg, float cb) {
        r.c[i] = t, r.rgb[i][0] = cr, r.rgb[i][1] = cg, r.rgb[i][2] = cb;
    };
    if (g_tab == FRIENDS) {
        for (const srvpage::Friend& f : g_friends) {
            Row r;
            const float c = f.online ? hi : 0.45f;
            set(r, 0, f.name, c, c, c);
            if (f.has_server) {
                set(r, 1, f.on.name, hi, hi, hi);
                set(r, 2, f.on.track, dim, dim, dim);
                set(r, 3, f.on.missing.empty() ? "Join" : "Install & Join", 1.0f, 0.78f, 0.25f);
                r.server = &f.on;
            } else {
                set(r, 1, f.online ? "Online - in the menus" : "Offline", dim, dim, dim);
            }
            rows.push_back(r);
        }
    } else {
        for (const srvpage::Server& s : g_list) {
            Row r;
            const float c = s.joinable ? hi : 0.45f;
            set(r, 0, s.name + (s.passworded ? "  [pw]" : ""), c, c, c);
            set(r, 1, s.track, c, c, c);
            char riders[24];
            sprintf_s(riders, "%d/%d", s.players, s.max_players);
            set(r, 2, riders, s.players > 0 ? 1.0f : c, s.players > 0 ? 0.78f : c, s.players > 0 ? 0.25f : c);
            std::string need;
            for (const std::string& m : s.missing) need += (need.empty() ? "needs " : " + ") + m;
            set(r, 3, need.empty() ? "ready" : need, need.empty() ? 0.45f : 1.0f, need.empty() ? 0.75f : 0.62f,
                need.empty() ? 0.45f : 0.2f);
            r.server = &s;
            rows.push_back(r);
        }
    }
    return rows;
}

// ---------------------------------------------------------------------------------------
// Typing a friend code: letters, digits and '-', polled like everything else

void TypeCode(bool focus) {
    static bool held[256] = {};
    if (!focus) return;
    auto edge = [&](int vk) {
        const bool k  = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool on = k && !held[vk];
        held[vk]      = k;
        return on;
    };
    for (int vk = 'A'; vk <= 'Z'; ++vk)
        if (edge(vk) && g_code.size() < 32) g_code += char(vk);
    for (int vk = '0'; vk <= '9'; ++vk)
        if (edge(vk) && g_code.size() < 32) g_code += char(vk);
    if (edge(VK_OEM_MINUS) && g_code.size() < 32) g_code += '-';
    if (edge(VK_BACK) && !g_code.empty()) g_code.pop_back();
    if (edge(VK_RETURN) && !g_code.empty()) {
        if (!pipe::g_connected) Status("Open MXB App to add friends from here", 6000);
        else {
            pipe::Send("{\"op\":\"friend_add\",\"code\":" + srvpage::Quote(g_code) + "}");
            Status("Sending the friend request...", 6000);
        }
        g_adding = false, g_code.clear();
    }
    if (edge(VK_ESCAPE)) g_adding = false, g_code.clear();
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

// The tabs, as big titles across the top in the menu's font; the bar's buttons per tab.
const float kTabFriends[4] = {0.2300f, 0.030f, 0.3900f, 0.095f};
const float kTabServers[4] = {0.4100f, 0.030f, 0.6600f, 0.095f};
const Button kJoin    = {"Join", {0.5875f, srvpage::kBarY0, 0.7375f, srvpage::kBarY0 + 0.044444f}};
const Button kAdd     = {"Add friend", {0.5375f, srvpage::kBarY0, 0.7375f, srvpage::kBarY0 + 0.044444f}};
const Button kRefresh = {"Refresh", {0.7375f, srvpage::kBarY0, 0.8875f, srvpage::kBarY0 + 0.044444f}};
const Button kBack    = {"Back", {0.8875f, srvpage::kBarY0, 1.0f, srvpage::kBarY0 + 0.044444f}};

void DrawTab(const char* label, const float r[4], bool on, float mx, float my) {
    const float hi = 240.0f / 255.0f;
    const bool  over = !on && srvpage::Inside(r, mx, my);
    const float c = on ? hi : over ? 0.85f : 0.42f;
    Text(g_main, label, r[0] * g_W, r[1] * g_H, (r[3] - r[1]) * g_H, LEFT, c, c, c);
    if (on) Quad(r[0] * g_W, r[3] * g_H + 2, r[0] * g_W + srvpage::TextWidth(g_main.f, label, (r[3] - r[1]) * g_H), r[3] * g_H + 5, 1.0f, 0.48f, 0.1f, 1.0f);
}

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

    // Whatever MXB App sent since the last frame, page open or not (join progress included).
    for (const std::string& line : pipe::Drain()) Handle(line, hwnd);

    if (!g_open) {
        if (menu && click && over) {
            LoadArt();
            if (g_loaded) {
                pipe::Start();
                if (pipe::g_connected) Ask();
                g_open = true, g_gone = 0, g_top = 0, g_sel = -1;
                Log("[mxb] page opened");
            }
        }
        if (!g_open) return;
    }
    // The game moved on (Testing, Settings...): the page goes with the main menu.
    g_gone = menu ? 0 : g_gone + 1;
    if (g_gone > 3) {
        g_open = false, g_adding = false;
        Log("[mxb] page closed with the main menu");
        return;
    }
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
    Button                 join   = kJoin;  // "Install & Join" when the server needs something first
    if (target && !target->missing.empty()) join.label = "Install & Join", join.r[0] = 0.4875f;

    // Input
    if (g_adding) TypeCode(focus);
    if (click) {
        const int row = srvpage::RowAt(mx, my, g_top, count);
        if (srvpage::Inside(kTabFriends, mx, my) && g_tab != FRIENDS) {
            g_tab = FRIENDS, g_top = 0, g_sel = -1;
            if (pipe::g_connected) pipe::Send("{\"op\":\"friends\"}");
        } else if (srvpage::Inside(kTabServers, mx, my) && g_tab != SERVERS) {
            g_tab = SERVERS, g_top = 0, g_sel = -1, g_adding = false;
        } else if (mx > 0.975f && my >= srvpage::kListY0 && my < srvpage::kListY0 + srvpage::kRows * srvpage::kRowH &&
                   count > 0) {
            const float k = (my - srvpage::kListY0) / (srvpage::kRows * srvpage::kRowH);
            g_top = (std::max)(0, (std::min)(count - srvpage::kRows, int(k * float(count)) - srvpage::kRows / 2));
        } else if (row >= 0) {
            if (row == g_last_click_row && now_ms - g_last_click_ms < 450 && rows[size_t(row)].server)
                Join(*rows[size_t(row)].server);
            g_sel = row, g_last_click_row = row, g_last_click_ms = now_ms;
        } else if (srvpage::Inside(join.r, mx, my) && target) {
            Join(*target);
        } else if (g_tab == FRIENDS && srvpage::Inside(kAdd.r, mx, my) && !g_adding) {
            g_adding = true, g_code.clear();
        } else if (srvpage::Inside(kRefresh.r, mx, my)) {
            if (pipe::g_connected) Ask();
            Status(pipe::g_connected ? "Asked MXB App again" : "Open MXB App to use this", 2500);
        } else if (srvpage::Inside(kBack.r, mx, my)) {
            g_open = false, g_adding = false;
            Log("[mxb] page closed");
            return;
        }
    }
    if (focus && !g_adding) {
        static bool held[4] = {};
        const int   keys[4] = {VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT};
        const int   step[4] = {-1, 1, -srvpage::kRows, srvpage::kRows};
        for (int i = 0; i < 4; ++i) {
            const bool k = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (k && !held[i]) g_top += step[i];
            held[i] = k;
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

    const float W = float(g_W), H = float(g_H), x0 = srvpage::kPanelX0 * W;
    const float hi = 240.0f / 255.0f, dim = 0.62f;
    Quad(x0, 0, W, H, 0, 0, 0, 0.78f);
    DrawTab("Friends", kTabFriends, g_tab == FRIENDS, mx, my);
    DrawTab("Install & Join", kTabServers, g_tab == SERVERS, mx, my);

    // The line under the tabs: whether MXB App is there, and what this tab is for.
    std::string note;
    float       nr = dim, ng = dim, nb = dim;
    if (!pipe::g_connected) {
        note = "Open MXB App on this PC to use this page", nr = 1.0f, ng = 0.62f, nb = 0.2f;
    } else if (g_tab == FRIENDS) {
        if (!g_have_friends) note = "Asking MXB App for your friends...";
        else if (!g_friends_note.empty()) note = g_friends_note, nr = 1.0f, ng = 0.62f, nb = 0.2f;
        else {
            int on = 0;
            for (const srvpage::Friend& f : g_friends) on += f.online;
            note = std::to_string(g_friends.size()) + " friends, " + std::to_string(on) +
                   " online   -   join one, even on a track you don't have yet";
        }
    } else {
        note = g_have_servers ? "Any server, even on a track you don't have: MXB App installs it, then joins"
                              : "Asking MXB App for the servers...";
    }
    Text(g_data, note, 0.237f * W, 0.112f * H, 0.024f * H, LEFT, nr, ng, nb);

    float cx[4] = {0.235f, 0.585f, 0.81f, 0.83f};
    if (g_tab == FRIENDS) cx[1] = 0.42f, cx[2] = 0.66f;
    const char*  head[2][4] = {{"Friend", "Where", "Track", ""}, {"Server", "Track", "Riders", "Needs"}};
    const Align  al[4]   = {LEFT, LEFT, g_tab == FRIENDS ? LEFT : RIGHT, LEFT};
    const float  hy      = 0.148f * H, rowSize = 0.0255f;
    for (int i = 0; i < 4; ++i)
        Text(g_data, head[g_tab == FRIENDS ? 0 : 1][i], cx[i] * W, hy, 0.021f * H, al[i], dim, dim, dim);
    Quad(x0 + 0.0225f * W, srvpage::kListY0 * H - 3, 0.97f * W, srvpage::kListY0 * H - 2, 1, 1, 1, 0.18f);

    const int hover = srvpage::RowAt(mx, my, g_top, count);
    for (int i = 0; i < srvpage::kRows && g_top + i < count; ++i) {
        const int   idx = g_top + i;
        const Row&  r   = rows[size_t(idx)];
        const float ry0 = (srvpage::kListY0 + i * srvpage::kRowH) * H, ry1 = ry0 + srvpage::kRowH * H;
        if (idx == g_sel) Sprite(g_pick, x0 + 0.0125f * W, ry0, 0.97f * W, ry1);
        else if (idx == hover) Sprite(g_hover, x0 + 0.0125f * W, ry0, 0.97f * W, ry1, 0.8f);
        const float ty = ry0 + (srvpage::kRowH - rowSize) * 0.5f * H, sz = rowSize * H;
        const float right[4] = {cx[1] - 0.015f, cx[2] - 0.015f, 0.82f, 0.965f};
        for (int c = 0; c < 4; ++c) {
            if (r.c[c].empty()) continue;
            const Align a   = (g_tab == SERVERS && c == 2) ? RIGHT : LEFT;
            const float w   = a == RIGHT ? 0.08f * W : (right[c] - cx[c]) * W;
            Text(g_data, srvpage::Fit(g_data.f, r.c[c], sz, w), cx[c] * W, ty, sz, a, r.rgb[c][0], r.rgb[c][1], r.rgb[c][2]);
        }
    }
    if (count == 0 && pipe::g_connected && (g_tab == SERVERS ? g_have_servers : g_have_friends))
        Text(g_data, g_tab == FRIENDS ? "No friends yet - Add friend with their code (in MXB App > Servers > Friends)"
                                      : "No servers right now",
             0.237f * W, (srvpage::kListY0 + 0.01f) * H, 0.024f * H, LEFT, dim, dim, dim);
    if (count > srvpage::kRows) {
        const float sy0 = srvpage::kListY0 * H, sy1 = (srvpage::kListY0 + srvpage::kRows * srvpage::kRowH) * H;
        Quad(0.978f * W, sy0, 0.982f * W, sy1, 1, 1, 1, 0.12f);
        const float a = float(g_top) / float(count), b = float(g_top + srvpage::kRows) / float(count);
        Quad(0.976f * W, sy0 + a * (sy1 - sy0), 0.984f * W, sy0 + b * (sy1 - sy0), 1, 1, 1, 0.55f);
    }

    // The bar
    Quad(x0, srvpage::kBarY0 * H, W, (srvpage::kBarY0 + 0.044444f) * H, 0, 0, 0, 0.35f);
    if (g_adding) {
        const std::string box = "Friend code: " + g_code + ((now_ms / 500) % 2 ? "_" : " ") + "   Enter to send";
        Text(g_data, box, 0.237f * W, (srvpage::kBarY0 + 0.011f) * H, 0.024f * H, LEFT, hi, hi, hi);
    } else if (GetTickCount64() < g_status_until) {
        Text(g_data, srvpage::Fit(g_data.f, g_status, 0.022f * H, 0.29f * W), 0.237f * W, (srvpage::kBarY0 + 0.011f) * H,
             0.022f * H, LEFT, 1.0f, 0.78f, 0.25f);
    } else if (count) {
        char range[64];
        sprintf_s(range, "%d-%d of %d", g_top + 1, (std::min)(count, g_top + srvpage::kRows), count);
        Text(g_data, range, 0.237f * W, (srvpage::kBarY0 + 0.011f) * H, 0.022f * H, LEFT, dim, dim, dim);
    }
    if (g_tab == FRIENDS && !target) DrawButton(kAdd, mx, my, down, pipe::g_connected && !g_adding);
    else DrawButton(join, mx, my, down, target != nullptr && pipe::g_connected);
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
