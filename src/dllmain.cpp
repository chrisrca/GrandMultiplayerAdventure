// Grand Multiplayer Adventure - entry point.
//
// Built as zlib.dll: every zlib export is forwarded to zlib_orig.dll (see
// zlib_proxy.def), so the game loads us automatically at startup.
//
//  * game thread (game.cpp): samples the local player's pose, injects remote
//    skiers into the game's character renderer, executes map travel.
//  * game GUI (gui.cpp): name labels, edge pins, player list, piste-map pins -
//    all drawn with the game's own text/sprite renderer.
//  * SDL_GL_SwapWindow hook (here): networking, pose smoothing, and building the
//    per-frame GUI data. Also captures the gameplay camera's
//    uViewProjectionMatrix (glew pointer hooks) to place labels in 3D, and
//    uploads Steam avatars as game textures (it runs on the GL thread).
//  * Steam (steam.cpp): every player sends their SteamID + persona name; each
//    client fetches the avatars itself from Steam.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "game.h"
#include "gui.h"
#include "hook.h"
#include "ini_template.h"
#include "log.h"
#include "map_extras.h"
#include "net.h"
#include "pose_math.h"
#include "settings_menu.h"
#include "steam.h"

namespace {

// ---- state -------------------------------------------------------------------
mp::Config g_cfg;
mp::Net g_net;
// Our challenge results as sent (kept to re-send after a network restart).
uint32_t g_scoreVersion = 0;
std::vector<std::string> g_scorePayloads;
bool g_gameOk = false;
bool g_guiOk = false;
bool g_netOk = false;
uint32_t g_localId = 0;
bool g_keyDown = false;
std::wstring g_gameDir;

using SwapFn = void(__cdecl*)(void*);
SwapFn g_realSwap = nullptr;

// ---- camera capture via glew pointers -----------------------------------------
typedef void(APIENTRY* UseProgramFn)(GLuint);
typedef void(APIENTRY* UniformMatrix4fvFn)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef GLint(APIENTRY* GetUniformLocationFn)(GLuint, const char*);
typedef void(APIENTRY* DeleteProgramFn)(GLuint);
void** g_varUseProgram = nullptr;
void** g_varUM4 = nullptr;
void** g_varGUL = nullptr;
void** g_varDelProg = nullptr;
UseProgramFn g_origUseProgram = nullptr;
UniformMatrix4fvFn g_origUM4 = nullptr;
GetUniformLocationFn g_origGUL = nullptr;
DeleteProgramFn g_origDelProg = nullptr;

GLuint g_curProg = 0;
std::unordered_map<GLuint, GLint> g_vpLoc;
int g_fbW = 0, g_fbH = 0;
float g_frameVP[16];
long long g_frameVPArea = 0;  // best candidate this frame
float g_vp[16];
bool g_haveVP = false;
double g_vpTime = 0;

void APIENTRY MyUseProgram(GLuint p) {
    g_curProg = p;
    g_origUseProgram(p);
}

void APIENTRY MyDeleteProgram(GLuint p) {
    g_vpLoc.erase(p);
    g_origDelProg(p);
}

void APIENTRY MyUM4(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (g_curProg && loc >= 0 && count >= 1 && v) {
        auto it = g_vpLoc.find(g_curProg);
        GLint want;
        if (it == g_vpLoc.end()) {
            want = g_origGUL(g_curProg, "uViewProjectionMatrix");
            g_vpLoc[g_curProg] = want;
        } else {
            want = it->second;
        }
        if (loc == want) {
            GLint vp[4];
            glGetIntegerv(GL_VIEWPORT, vp);
            long long area = (long long)vp[2] * vp[3];
            bool aspectOk = g_fbH > 0 && vp[3] > 0 &&
                            fabsf((float)vp[2] / vp[3] - (float)g_fbW / g_fbH) < 0.02f;  // skips square shadow maps
            if (aspectOk && area > g_frameVPArea) {
                g_frameVPArea = area;
                if (transpose) {
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) g_frameVP[c * 4 + r] = v[r * 4 + c];
                } else {
                    memcpy(g_frameVP, v, sizeof g_frameVP);
                }
            }
        }
    }
    g_origUM4(loc, count, transpose, v);
}

void EnsureGLHooks() {
    HMODULE glew = GetModuleHandleA("glew32.dll");
    if (!glew) return;
    if (!g_varUseProgram) {
        g_varUseProgram = (void**)GetProcAddress(glew, "__glewUseProgram");
        g_varUM4 = (void**)GetProcAddress(glew, "__glewUniformMatrix4fv");
        g_varGUL = (void**)GetProcAddress(glew, "__glewGetUniformLocation");
        g_varDelProg = (void**)GetProcAddress(glew, "__glewDeleteProgram");
        if (!g_varUseProgram || !g_varUM4 || !g_varGUL || !g_varDelProg) {
            Log("glew exports not found; camera capture disabled");
            g_varUseProgram = nullptr;
            return;
        }
    }
    // (Re)install if glewInit ran (again) and overwrote our pointers.
    if (*g_varUseProgram && *g_varUseProgram != (void*)&MyUseProgram) {
        g_origUseProgram = (UseProgramFn)*g_varUseProgram;
        hook::WritePtr(g_varUseProgram, (void*)&MyUseProgram);
    }
    if (*g_varUM4 && *g_varUM4 != (void*)&MyUM4) {
        g_origUM4 = (UniformMatrix4fvFn)*g_varUM4;
        hook::WritePtr(g_varUM4, (void*)&MyUM4);
    }
    if (*g_varDelProg && *g_varDelProg != (void*)&MyDeleteProgram) {
        g_origDelProg = (DeleteProgramFn)*g_varDelProg;
        hook::WritePtr(g_varDelProg, (void*)&MyDeleteProgram);
    }
    if (*g_varGUL) g_origGUL = (GetUniformLocationFn)*g_varGUL;
}

// ---- helpers -------------------------------------------------------------------
// Player colours (labels, pins, list). Bright, distinct, readable on snow.
const float kPalette[8][3] = {
    {1.00f, 0.60f, 0.80f}, {0.55f, 0.80f, 1.00f}, {0.65f, 0.95f, 0.55f}, {1.00f, 0.80f, 0.40f},
    {0.80f, 0.60f, 1.00f}, {0.45f, 0.95f, 0.90f}, {1.00f, 0.55f, 0.50f}, {0.95f, 0.95f, 0.55f},
};

struct Toast {
    std::string text;
    uint64_t steamId;
    double born;
};
std::deque<Toast> g_toasts;
void AddToast(const std::string& s, uint64_t steamId = 0) {
    g_toasts.push_back({s, steamId, game::Now()});
    while (g_toasts.size() > 3) g_toasts.pop_front();
}

// ---- Steam identity + avatars ------------------------------------------------------
uint64_t g_steamId = 0;
std::string g_displayName;  // Steam persona name, else the ini name

// Copy UTF-8 into a fixed buffer without splitting a multi-byte character.
void CopyUtf8(char* dst, size_t cap, const std::string& s) {
    size_t n = std::min(s.size(), cap - 1);
    while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}

struct AvatarSlot {
    bool ready = false;
    double nextTry = 0;
    int tries = 0;
};
std::unordered_map<uint64_t, AvatarSlot> g_avatars;

std::string AvatarTex(uint64_t id) { return "gmamp_av_" + std::to_string(id); }

// The texture name to draw for a player, or "" while it isn't uploaded yet.
std::string AvatarFor(uint64_t id) {
    auto it = g_avatars.find(id);
    return id && it != g_avatars.end() && it->second.ready ? AvatarTex(id) : std::string();
}

// GL thread. Fetch each wanted avatar from Steam (asynchronous on Steam's side)
// and upload it through the game's texture holder once it has arrived.
void UpdateAvatars(double now, const std::vector<uint64_t>& ids) {
    if (!g_guiOk || !steam::Available()) return;
    void* globals = game::Globals();
    bool pending = false;
    for (uint64_t id : ids) {
        if (!id) continue;
        AvatarSlot& a = g_avatars[id];
        if (a.ready) continue;
        pending = true;
        if (now < a.nextTry || !globals) continue;
        steam::Avatar img;
        if (steam::GetAvatar(id, img) && gui::RegisterTexture(globals, AvatarTex(id), img.w, img.h, img.rgba.data())) {
            a.ready = true;
            Log("Steam avatar %llu ready (%dx%d)", (unsigned long long)id, img.w, img.h);
            continue;
        }
        ++a.tries;
        a.nextTry = now + (a.tries < 60 ? 0.5 : 15.0);  // still downloading / no avatar
    }
    static double lastPump = 0;
    if (pending && now - lastPump > 0.25) {
        lastPump = now;
        steam::Pump();
    }
}

// Per remote player: the pose we are displaying, eased toward the latest
// network sample (extrapolated by the observed velocity) every frame.
struct Smooth {
    game::Pose pose;
    double lastFrame;
};
std::unordered_map<uint32_t, Smooth> g_smooth;

float Dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

std::string Meters(float d) {
    char b[32];
    if (d < 1000) snprintf(b, sizeof b, "%dm", (int)(d + 0.5f));
    else snprintf(b, sizeof b, "%.1fkm", d / 1000.f);
    return b;
}

bool OurWindowFocused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Top of the skier's head from the game's pose (bone tag 11 = head).
bool HeadTop(const game::Pose& p, float out[3]) {
    bool found = false;
    for (int b = 0; b < p.count; ++b) {
        uint32_t tag;
        memcpy(&tag, &p.bones[b][9], 4);
        if (tag != 11) continue;
        for (int e = 0; e < 2; ++e) {
            const float* v = &p.bones[b][e * 3];
            if (!found || v[2] > out[2]) memcpy(out, v, 12), found = true;
        }
    }
    return found;
}

// Pose summary for the debug pose log: heading of the skis/board, and the body
// lean (angle from vertical + its compass direction) from feet to head top.
std::string PoseSummary(const game::Pose& p) {
    if (p.count <= 0) return "none";
    float head[3];
    if (!HeadTop(p, head)) return "nohead";
    const float* f = p.bones[0];
    float lx = head[0] - f[0], ly = head[1] - f[1], lz = head[2] - f[2];
    float horiz = sqrtf(lx * lx + ly * ly);
    char b[128];
    snprintf(b, sizeof b, "h=%.1f lean=%.1f leanaz=%.1f bones=%d", posemath::Heading(p.bones) * 57.29578f,
             atan2f(horiz, lz) * 57.29578f, atan2f(ly, lx) * 57.29578f, p.count);
    return b;
}

// Facing for the piste-map pin: what the sender's own game reports, else (older
// builds) a guess from the first bone, which runs tail -> tip of the skis.
void FacingOf(const mp::PlayerState& st, const game::Pose& p, float dir[2]) {
    if (st.flags & mp::kFlagHeading) {
        mp::UnpackHeading(st.heading, dir);
        return;
    }
    if (p.count <= 0) return;
    float hx = p.bones[0][3] - p.bones[0][0], hy = p.bones[0][4] - p.bones[0][1];
    float hl = sqrtf(hx * hx + hy * hy);
    if (hl > 1e-3f) dir[0] = hx / hl, dir[1] = hy / hl;
}

// ---- Settings > Multiplayer ----------------------------------------------------------
// The box on the game's Settings screen reads g_cfg from the game thread and
// queues clicks; the clicks are applied here on the frame thread. g_cfg is only
// ever written on the frame thread, under g_cfgMu.
//
// "Host address" and "Port" are typed into in place: clicking the row starts
// editing, and while editing the game window's keyboard messages come to us
// (MpWndProc) instead of the game. Enter saves, Esc cancels, Ctrl+V pastes.
std::mutex g_cfgMu;
std::vector<settings_menu::Row> g_menuClicks;
int g_editRow = -1;          // settings_menu::Row being typed into, or -1   (g_cfgMu)
std::string g_editText;      //                                              (g_cfgMu)
bool g_editCommit = false;   // Enter was pressed                            (g_cfgMu)

std::string HostAddressText() { return g_cfg.hostAddr; }

settings_menu::State MenuState() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    settings_menu::State st;
    st.host = g_cfg.host;
    st.address = HostAddressText();
    st.port = g_cfg.port;
    st.names = g_cfg.showNames;
    st.editRow = g_editRow;
    st.editText = g_editText;
    return st;
}

void MenuClick(settings_menu::Row r) {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_menuClicks.push_back(r);
}

std::string ClipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1 && n < 4096) {
                out.resize(n - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

// "203.0.113.7", "my.host.net" or either with ":port".
bool ParseAddress(std::string s, std::string& out) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(0, 1);
    if (s.empty() || s.size() > 63) return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != ':') return false;
    std::string host = s;
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        if (s.find(':', colon + 1) != std::string::npos) return false;
        int p = atoi(s.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        host = s.substr(0, colon);
    }
    if (host.empty() || host.find('.') == std::string::npos) return false;
    out = s;
    return true;
}

// Characters accepted while typing into a row.
bool EditAccepts(int row, wchar_t c) {
    if (row == settings_menu::kRowPort) return c >= L'0' && c <= L'9';
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.' ||
           c == L'-' || c == L':';
}
size_t EditMax(int row) { return row == settings_menu::kRowPort ? 5 : 63; }

// Game window subclass: only active while a row is being typed into.
WNDPROC g_origWndProc = nullptr;

LRESULT CALLBACK MpWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Picking another skier on the piste map: a click that was ours (on / off a
    // skier) is kept from the game, button up included.
    static bool swallowUp = false;
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        RECT rc;
        swallowUp = false;
        if (GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0 &&
            gui::MapClick((float)(short)LOWORD(l) / rc.right, (float)(short)HIWORD(l) / rc.bottom)) {
            swallowUp = true;
            return 0;
        }
    }
    if (m == WM_LBUTTONUP && swallowUp) {
        swallowUp = false;
        return 0;
    }
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_KEYUP || m == WM_SYSKEYUP) {
        std::unique_lock<std::mutex> lk(g_cfgMu);
        if (g_editRow >= 0) {
            if (m == WM_KEYDOWN) {
                if (w == VK_RETURN) g_editCommit = true;
                else if (w == VK_ESCAPE) g_editRow = -1, g_editText.clear();
                else if (w == VK_BACK && !g_editText.empty()) g_editText.pop_back();
                else if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    int row = g_editRow;
                    lk.unlock();
                    std::string clip = ClipboardText();
                    lk.lock();
                    if (g_editRow == row)
                        for (char c : clip)
                            if (EditAccepts(row, (wchar_t)(unsigned char)c) && g_editText.size() < EditMax(row))
                                g_editText += c;
                }
            } else if (m == WM_CHAR) {
                wchar_t c = (wchar_t)w;
                if (EditAccepts(g_editRow, c) && g_editText.size() < EditMax(g_editRow)) g_editText += (char)c;
            }
            return 0;  // the game doesn't see keys while typing
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && wcsncmp(cls, L"SDL_app", 7) == 0) {
        *(HWND*)out = h;
        return FALSE;
    }
    return TRUE;
}

void EnsureWndProc() {
    static HWND hooked = nullptr;
    static double lastTry = -1e9;
    if (hooked && IsWindow(hooked)) return;
    double now = game::Now();
    if (now - lastTry < 2.0) return;
    lastTry = now;
    HWND h = nullptr;
    EnumWindows(FindGameWindow, (LPARAM)&h);
    if (!h) return;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&MpWndProc);
    if (g_origWndProc) {
        hooked = h;
        gui::SetWindow(h);
        Log("Keyboard text entry hooked (window %p)", (void*)h);
    }
}

void SaveConfig() {
    mp::IniValues v;
    v.host = g_cfg.host;
    v.hostAddress = g_cfg.hostAddr;
    v.port = g_cfg.port;
    v.name = g_cfg.name;
    v.showNames = g_cfg.showNames;
    v.showPanel = g_cfg.showPanel;
    v.toggleKey = "F" + std::to_string(g_cfg.toggleKey - VK_F1 + 1);
    v.sendRateHz = (int)(g_cfg.sendRateHz + 0.5f);
    FILE* f = _wfopen((g_gameDir + L"gma_mp.ini").c_str(), L"wb");
    if (!f) {
        Log("Could not save gma_mp.ini");
        return;
    }
    fputs(mp::MakeIni(v).c_str(), f);
    fclose(f);
}

void RestartNet() {
    g_net.Stop();
    g_net = mp::Net();
    g_smooth.clear();
    g_netOk = g_net.Start(g_cfg, g_localId);
    if (g_netOk) g_net.SetScores(g_scoreVersion, g_scorePayloads);
    if (!g_netOk) AddToast(g_cfg.host ? "Port " + std::to_string(g_cfg.port) + " is busy" : "Network error");
    else AddToast(g_cfg.host ? "Hosting on port " + std::to_string(g_cfg.port) : "Joining " + HostAddressText());
}

// Applies typed text (g_cfgMu held). Returns true if networking must restart.
bool CommitEdit(int row, const std::string& text) {
    if (row == settings_menu::kRowAddress) {
        std::string addr;
        if (!ParseAddress(text, addr)) {
            AddToast("That isn't an IP address or host name");
            return false;
        }
        if (addr == g_cfg.hostAddr) return false;
        g_cfg.hostAddr = addr;
        if (g_cfg.host) AddToast("Host address set to " + addr);
        return !g_cfg.host;
    }
    int p = atoi(text.c_str());
    if (p <= 0 || p > 65535) {
        AddToast("Ports go from 1 to 65535");
        return false;
    }
    if (p == g_cfg.port) return false;
    g_cfg.port = (uint16_t)p;
    return true;
}

void ApplyMenuClicks() {
    std::vector<settings_menu::Row> clicks;
    bool restart = false, save = false;
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        clicks.swap(g_menuClicks);
        // Typing ends when Enter is pressed or the Settings screen is left.
        if (g_editRow >= 0 && settings_menu::SecondsSinceShown() > 0.5) g_editRow = -1, g_editText.clear();
        if (g_editCommit) {
            g_editCommit = false;
            if (g_editRow >= 0) {
                restart |= CommitEdit(g_editRow, g_editText);
                save = true;
                g_editRow = -1;
                g_editText.clear();
            }
        }
        for (settings_menu::Row r : clicks) {
            bool editable = r == settings_menu::kRowAddress || r == settings_menu::kRowPort;
            if (g_editRow >= 0) {  // a click while typing: save it, then act on the click
                int was = g_editRow;
                restart |= CommitEdit(g_editRow, g_editText);
                save = true;
                g_editRow = -1;
                g_editText.clear();
                if (was == r) continue;  // clicked the row being edited: just save
            }
            if (editable) {
                g_editRow = r;
                g_editText = r == settings_menu::kRowPort ? std::to_string(g_cfg.port) : g_cfg.hostAddr;
                continue;
            }
            if (r == settings_menu::kRowMode) g_cfg.host = !g_cfg.host, restart = true;
            else g_cfg.showNames = !g_cfg.showNames;
            save = true;
        }
    }
    if (save) SaveConfig();
    if (restart) RestartNet();
}

// ---- per-frame -----------------------------------------------------------------------
double g_lastFrame = 0;

void OnFrame() {
    double now = game::Now();
    double dt = g_lastFrame > 0 ? std::min(0.1, now - g_lastFrame) : 0.016;
    g_lastFrame = now;
    ApplyMenuClicks();

    {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            g_fbW = vp[2];
            g_fbH = vp[3];
            gui::SetAspect((float)g_fbW / g_fbH);
        }
    }
    if (g_frameVPArea > 0) {
        memcpy(g_vp, g_frameVP, sizeof g_vp);
        g_haveVP = true;
        g_vpTime = now;
    }
    g_frameVPArea = 0;
    bool vpFresh = g_haveVP && now - g_vpTime < 0.25;

    bool down = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (down && !g_keyDown && OurWindowFocused()) {  // show / hide the player list (remembered)
        {
            std::lock_guard<std::mutex> lk(g_cfgMu);
            g_cfg.showPanel = !g_cfg.showPanel;
        }
        SaveConfig();
    }
    g_keyDown = down;

    game::LocalInfo li = g_gameOk ? game::Local() : game::LocalInfo{};
    bool fresh = li.valid && now - li.stamp < 0.5;
    const int myColor = (int)(g_localId % 8);

    mp::PlayerState ps{};
    ps.steamId = g_steamId;
    CopyUtf8(ps.name, sizeof ps.name, g_displayName);
    snprintf(ps.scene, sizeof ps.scene, "%s", li.scene.c_str());
    memcpy(ps.pos, li.feet, sizeof ps.pos);
    memcpy(ps.head, li.head, sizeof ps.head);
    ps.hue = li.hue;
    ps.color = (uint8_t)myColor;
    ps.flags = fresh ? mp::kFlagInWorld : 0;
    if (fresh) {
        mp::PackPose(ps, li.pose.count, li.pose.bones);
        // The direction the game's own map pin uses for us.
        ps.heading = mp::PackHeading(li.dir);
        ps.flags |= mp::kFlagHeading;
    }
    if (g_netOk) g_net.Tick(now, ps);
    for (auto& e : g_net.TakeEvents()) AddToast(e.name + (e.joined ? " joined" : " left"), e.steamId);
    // Challenge results: ours out, everyone else's in.
    {
        mp::Records mine;
        if (map_extras::TakeLocalRecords(mine)) {
            g_scorePayloads = mp::EncodeRecords(mine, g_scoreVersion);
            if (g_netOk) g_net.SetScores(g_scoreVersion, g_scorePayloads);
        }
        if (g_netOk)
            for (const mp::ScoreChunk& c : g_net.TakeScoreChunks()) map_extras::OnScoreChunk(c);
    }

    std::vector<mp::RemotePlayer> remotes = g_netOk ? g_net.Remotes() : std::vector<mp::RemotePlayer>{};
    {
        std::vector<std::pair<uint32_t, std::string>> who;
        for (const mp::RemotePlayer& r : remotes) who.push_back({r.state.id, r.state.name});
        map_extras::SetPlayers(who);
    }
    std::vector<game::RemoteDraw> draws;
    gui::Frame gf;
    gf.showList = g_cfg.showPanel;
    gf.toggleKey = "F" + std::to_string(g_cfg.toggleKey - VK_F1 + 1);
    gf.showNames = g_cfg.showNames;
    gf.viewProjValid = vpFresh && fresh;
    memcpy(gf.viewProj, g_vp, sizeof gf.viewProj);
    gf.header = "Multiplayer";
    if (!g_gameOk) {
        gf.subline = "Unsupported game version";
        gf.subGood = false;
    } else if (!g_netOk) {
        gf.subline = g_cfg.host ? "Port " + std::to_string(g_cfg.port) + " is busy" : "Network error";
        gf.subGood = false;
    } else if (g_cfg.host) {
        gf.subline = "Hosting on port " + std::to_string(g_cfg.port);
    } else if (g_net.SinceHostHeard(now) < 3.0) {
        gf.subline = "Connected to " + g_cfg.hostAddr;
    } else {
        static const char* dots[4] = {"", ".", "..", "..."};
        gf.subline = "Connecting to " + g_cfg.hostAddr + dots[(int)(now * 2) % 4];
        gf.subGood = false;
    }

    std::vector<uint64_t> wantAvatars{g_steamId};
    for (const mp::RemotePlayer& r : remotes) wantAvatars.push_back(r.state.steamId);
    for (const Toast& t : g_toasts) wantAvatars.push_back(t.steamId);
    UpdateAvatars(now, wantAvatars);

    gui::Player me;
    me.name = g_displayName;
    me.avatar = AvatarFor(g_steamId);
    memcpy(me.color, kPalette[myColor], sizeof me.color);
    me.you = true;
    me.host = g_cfg.host;
    me.status = li.scene.empty() ? "" : game::PrettyScene(li.scene);
    gf.players.push_back(me);

    float k = 1.f - expf(-(float)dt * 16.f);
    std::string poseLog;
    for (const mp::RemotePlayer& r : remotes) {
        const mp::PlayerState& st = r.state;
        bool inWorld = (st.flags & mp::kFlagInWorld) != 0 && st.boneCount > 0;
        bool sameScene = inWorld && fresh && li.scene == st.scene;

        gui::Player gp;
        gp.id = st.id;
        gp.name = st.name;
        gp.avatar = AvatarFor(st.steamId);
        memcpy(gp.color, kPalette[st.color % 8], sizeof gp.color);
        gp.host = (st.flags & mp::kFlagHost) != 0;
        gp.dim = !sameScene;
        gp.scene = st.scene;

        // Velocity from the last two samples, to move them on between packets.
        float vel[3] = {0, 0, 0};
        double sdt = r.curTime - r.prevTime;
        if (sdt > 0.01 && sdt < 0.5)
            for (int i = 0; i < 3; ++i) vel[i] = (st.pos[i] - r.prevPos[i]) / (float)sdt;
        float ahead = (float)std::min(0.2, now - r.curTime);

        if (!sameScene) {
            gp.status = !inWorld ? "Loading" : game::PrettyScene(st.scene);
            if (inWorld) {
                // Not drawn on our slope, but shown on their mountain's piste map.
                gp.onMap = true;
                for (int i = 0; i < 3; ++i) gp.feet[i] = st.pos[i] + vel[i] * ahead;
                game::Pose raw;
                raw.count = mp::UnpackPose(st, raw.bones);
                FacingOf(st, raw, gp.dir);
            }
            gf.players.push_back(gp);
            g_smooth.erase(st.id);
            continue;
        }

        // Target pose = latest sample shifted by velocity * time since it arrived.
        game::Pose target;
        target.count = mp::UnpackPose(st, target.bones);
        for (int b = 0; b < target.count; ++b)
            for (int i = 0; i < 3; ++i) {
                target.bones[b][i] += vel[i] * ahead;
                target.bones[b][3 + i] += vel[i] * ahead;
            }

        auto it = g_smooth.find(st.id);
        bool snap = it == g_smooth.end() || it->second.pose.count != target.count ||
                    Dist(it->second.pose.bones[0], target.bones[0]) > 30.f;
        if (snap) {
            Smooth sm;
            sm.pose = target;
            it = g_smooth.insert_or_assign(st.id, sm).first;
        } else {
            Smooth& sm = it->second;
            posemath::EaseToward(sm.pose.bones, target.bones, target.count, k);
        }
        it->second.lastFrame = now;
        draws.push_back({st.id, st.hue, it->second.pose});
        if (g_cfg.debugPoseLog) poseLog += " | R:" + std::string(st.name) + " " + PoseSummary(it->second.pose);

        gp.visible = true;
        gp.onMap = true;
        gp.dist = Dist(li.feet, st.pos);
        gp.status = Meters(gp.dist);
        const game::Pose& sp = it->second.pose;
        if (!HeadTop(sp, gp.anchor)) memcpy(gp.anchor, st.head, sizeof gp.anchor);
        gp.anchor[2] += 0.25f;  // a little headroom above the helmet
        for (int b = 0; b < sp.count; ++b)
            for (int e = 0; e < 2; ++e) gp.outline.insert(gp.outline.end(), &sp.bones[b][e * 3], &sp.bones[b][e * 3] + 3);
        memcpy(gp.feet, sp.bones[0], sizeof gp.feet);
        FacingOf(st, sp, gp.dir);
        gf.players.push_back(gp);
    }
    for (auto it = g_smooth.begin(); it != g_smooth.end();)
        it = (now - it->second.lastFrame > 2.0) ? g_smooth.erase(it) : std::next(it);
    if (g_gameOk) game::SetRemoteDraws(std::move(draws));
    static double lastPoseLog = 0;
    if (g_cfg.debugPoseLog && fresh && now - lastPoseLog >= 0.25) {
        lastPoseLog = now;
        Log("posecheck t=%.3f L %s%s", now, PoseSummary(li.pose).c_str(), poseLog.c_str());
    }

    while (!g_toasts.empty() && now - g_toasts.front().born > 4.5) g_toasts.pop_front();
    for (const Toast& t : g_toasts) {
        double age = now - t.born;
        float a = (float)std::min({1.0, age / 0.3, (4.5 - age) / 0.8});
        if (a > 0) gf.toasts.push_back({t.text, AvatarFor(t.steamId), a});
    }
    if (g_guiOk) gui::Set(std::move(gf));

    static double lastDiag = 0;
    if (now - lastDiag > 10.0) {
        lastDiag = now;
        Log("diag: scene='%s' fresh=%d vp=%d remotes=%zu bones=%d", li.scene.c_str(), fresh,
            vpFresh, remotes.size(), li.pose.count);
    }
}

// Debug aid: if <game>\gma_mp_shot.req exists, save the final frame to
// gma_mp_shot.bmp. Fullscreen GL windows can't be captured with GDI.
void MaybeDumpFrame() {
    static int counter = 0;
    if (++counter % 30) return;
    std::wstring req = g_gameDir + L"gma_mp_shot.req";
    if (GetFileAttributesW(req.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(req.c_str());
    int w = g_fbW, h = g_fbH;
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> px((size_t)w * h * 4);
    typedef void(APIENTRY * BindFb)(GLenum, GLuint);
    typedef void(APIENTRY * BindBuf)(GLenum, GLuint);
    auto bindFb = (BindFb)wglGetProcAddress("glBindFramebuffer");
    auto bindBuf = (BindBuf)wglGetProcAddress("glBindBuffer");
    GLint fbo = 0, pbo = 0, packAlign = 4;
    glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &fbo);
    glGetIntegerv(0x88ED /*PIXEL_PACK_BUFFER_BINDING*/, &pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
    if (bindBuf) bindBuf(0x88EB, 0);
    if (bindFb) bindFb(0x8CA8, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, 0x80E1 /*BGRA*/, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
    if (bindFb) bindFb(0x8CA8, fbo);
    if (bindBuf) bindBuf(0x88EB, pbo);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    FILE* f = _wfopen((g_gameDir + L"gma_mp_shot.bmp").c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    Log("Saved gma_mp_shot.bmp (%dx%d)", w, h);
}

void __cdecl HookSwap(void* window) {
    static bool first = true;
    if (first) {
        first = false;
        Log("Swap hook runs on thread %lu", GetCurrentThreadId());
        // Steam is up by the first frame (the game initialises it at startup).
        g_steamId = steam::LocalId();
        g_displayName = steam::LocalName();
        if (g_displayName.empty()) g_displayName = g_cfg.name;
        // Started here rather than in DllMain: Winsock must not be initialised
        // under the loader lock.
        g_netOk = g_net.Start(g_cfg, g_localId);
        if (!g_gameOk) AddToast("Multiplayer: unsupported game version");
        else if (!g_netOk) AddToast("Multiplayer: network failed to start");
        else if (g_cfg.host) AddToast("Hosting on port " + std::to_string(g_cfg.port));
        else AddToast("Joining " + g_cfg.hostAddr);
    }
    EnsureGLHooks();
    EnsureWndProc();
    OnFrame();
    MaybeDumpFrame();
    g_realSwap(window);
}

uint32_t RandomId() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t x = (uint64_t)c.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64();
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x | 1;
}

void Attach() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p(exePath);
    std::wstring exeName = p.substr(p.find_last_of(L"\\/") + 1);
    if (_wcsicmp(exeName.c_str(), L"gma_wonderlands.exe") != 0) return;  // only inject into the game
    g_gameDir = p.substr(0, p.find_last_of(L"\\/") + 1);

    Log("Grand Multiplayer Adventure loaded");
    g_cfg = mp::LoadConfig(g_gameDir + L"gma_mp.ini");
    g_localId = RandomId();

    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    g_gameOk = game::Init(base);
    g_guiOk = g_gameOk && gui::Init(base);
    if (g_gameOk) settings_menu::Init(base, MenuState, MenuClick);
    if (g_guiOk) map_extras::Init(base);

    g_realSwap = (SwapFn)hook::PatchIAT((void*)base, "SDL2.dll", "SDL_GL_SwapWindow", (void*)&HookSwap);
    if (!g_realSwap) Log("Failed to hook SDL_GL_SwapWindow; multiplayer disabled");
    else Log("Hooked SDL_GL_SwapWindow");
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Attach();
    }
    return TRUE;
}
