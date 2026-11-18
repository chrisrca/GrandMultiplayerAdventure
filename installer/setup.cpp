// Grand Multiplayer Adventure - one-file installer.
//
// GUI (double-click): finds the game through Steam and installs or removes the
// mod (zlib.dll proxy, embedded in this exe). All multiplayer settings live in
// the game itself (Settings > Multiplayer); the mod writes its defaults on
// first launch. Installing also lets the game through Windows Firewall (inbound
// UDP, so friends can join; Windows asks for admin), and uninstalling removes
// that rule again.
//
// Command line (used by CI and for scripting; leaves the firewall alone):
//   GMA-Multiplayer-Setup.exe --install [--dir <game dir>]
//   GMA-Multiplayer-Setup.exe --uninstall [--dir <game dir>]
//   GMA-Multiplayer-Setup.exe --status [--dir <game dir>]      (exit 0 = installed, 3 = not)
// Development:
//   GMA-Multiplayer-Setup.exe --preview <file.png> [scale [hover [kind message]]]  (the window as a PNG)
//   GMA-Multiplayer-Setup.exe --icon <file.ico>                (the app icon, for installer/icon.ico)
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "scene.h"

#ifndef GMAMP_VERSION
#define GMAMP_VERSION "dev"
#endif
#define IDI_APP 1
#define IDR_MODDLL 101
#define IDR_TITLEFONT 102
#define IDR_LINKFONT 103

static const wchar_t* kExeName = L"gma_wonderlands.exe";
static const wchar_t* kAppName = L"Grand Multiplayer Adventure";
static HWND g_wnd;  // the installer window (GUI)

// ---- helpers ------------------------------------------------------------------
static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring Join(std::wstring a, const std::wstring& b) {
    if (!a.empty() && a.back() != L'\\' && a.back() != L'/') a += L'\\';
    return a + b;
}
static bool ReadFile(const std::wstring& p, std::string& out) {
    std::ifstream f(p.c_str(), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
static std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    std::wstring s(buf);
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}
// The folder as it is spelled on disk ("c:\program files (x86)\steam" from
// Steam's registry value becomes "C:\Program Files (x86)\Steam").
static std::wstring OnDiskPath(const std::wstring& p) {
    std::wstring out = p;
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[2048];
        DWORD n = GetFinalPathNameByHandleW(h, buf, 2048, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n > 0 && n < 2048) {
            out = buf;
            if (out.compare(0, 8, L"\\\\?\\UNC\\") == 0) out = L"\\\\" + out.substr(8);
            else if (out.compare(0, 4, L"\\\\?\\") == 0) out = out.substr(4);
        }
        CloseHandle(h);
    }
    if (out.size() >= 2 && out[1] == L':') out[0] = (wchar_t)towupper(out[0]);
    return out;
}

// ---- locating the game --------------------------------------------------------
static std::wstring FindGameDir() {
    std::vector<std::wstring> roots;
    for (auto r : {RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                   RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                   RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"),
                   std::wstring(L"C:\\Program Files (x86)\\Steam")})
        if (!r.empty()) roots.push_back(r);
    std::vector<std::wstring> libs = roots;
    for (auto& root : roots) {
        std::string vdf;
        if (!ReadFile(Join(root, L"steamapps\\libraryfolders.vdf"), vdf)) continue;
        std::istringstream in(vdf);
        std::string line;
        while (std::getline(in, line)) {
            size_t k = line.find("\"path\"");
            if (k == std::string::npos) continue;
            size_t a = line.find('"', k + 6), b = a == std::string::npos ? a : line.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string p = line.substr(a + 1, b - a - 1), u;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                u += p[i];
            }
            libs.push_back(W(u));
        }
    }
    for (auto& lib : libs) {
        // Prefer the manifest's install dir, fall back to the default name.
        std::string acf, installdir = "GMA Wonderlands";
        if (ReadFile(Join(lib, L"steamapps\\appmanifest_1247360.acf"), acf)) {
            size_t k = acf.find("\"installdir\"");
            if (k != std::string::npos) {
                size_t a = acf.find('"', k + 12), b = acf.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) installdir = acf.substr(a + 1, b - a - 1);
            }
        }
        std::wstring dir = Join(Join(lib, L"steamapps\\common"), W(installdir));
        if (Exists(Join(dir, kExeName))) return OnDiskPath(dir);
    }
    return L"";
}

// ---- install state --------------------------------------------------------------
static bool IsOurDll(const std::wstring& path) {
    std::string data;
    return ReadFile(path, data) && data.find("zlib_orig.inflate") != std::string::npos;
}
static bool IsInstalled(const std::wstring& dir) {
    return Exists(Join(dir, L"zlib_orig.dll")) && IsOurDll(Join(dir, L"zlib.dll"));
}
static bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kExeName) == 0) found = true;
    CloseHandle(snap);
    return found;
}

// ---- firewall -------------------------------------------------------------------
static const wchar_t* kRuleName = L"Grand Multiplayer Adventure";

// Runs a command line hidden; its exit code (or -1).
static int RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static std::wstring Netsh(const std::wstring& args) {
    wchar_t sys[MAX_PATH] = L"C:\\Windows\\System32";
    GetSystemDirectoryW(sys, MAX_PATH);
    return L"\"" + Join(sys, L"netsh.exe") + L"\" advfirewall firewall " + args;
}
static std::wstring RuleName() { return std::wstring(L"name=\"") + kRuleName + L"\""; }

// The elevated half (this installer run again as administrator with
// --firewall-allow <game dir> or --firewall-remove): lets the game take
// inbound UDP (hosting), replacing any rule an earlier install made so it
// always points at this game folder; or removes the rule.
static int FirewallElevated(bool allow, const std::wstring& dir) {
    RunHidden(Netsh(L"delete rule " + RuleName()));
    if (!allow) return 0;
    return RunHidden(Netsh(L"add rule " + RuleName() + L" dir=in action=allow protocol=UDP profile=any program=\"" +
                           Join(dir, kExeName) + L"\"")) == 0 ? 0 : 1;
}
// Runs this installer as administrator with `params` (Windows' UAC prompt
// names the installer) and waits for it. Declining the prompt just skips it.
static void RunSelfElevated(const std::wstring& params) {
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_wnd;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return;
    WaitForSingleObject(sei.hProcess, 60000);
    CloseHandle(sei.hProcess);
}
static void AllowThroughFirewall(const std::wstring& dir) {
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();  // no \" in the quoted argument
    RunSelfElevated(L"--firewall-allow \"" + d + L"\"");
}
static void RemoveFirewallRule() {
    if (RunHidden(Netsh(L"show rule " + RuleName())) == 0) RunSelfElevated(L"--firewall-remove");
}

// ---- install / uninstall ------------------------------------------------------
static std::wstring Install(const std::wstring& dir) {
    if (!Exists(Join(dir, kExeName))) return L"gma_wonderlands.exe was not found in that folder.";
    if (GameRunning()) return L"Please close Grand Mountain Adventure first.";
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_MODDLL), (LPCWSTR)RT_RCDATA);
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = h ? LockResource(h) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) return L"The installer is damaged (mod payload missing).";

    std::wstring zlib = Join(dir, L"zlib.dll"), orig = Join(dir, L"zlib_orig.dll");
    if (!Exists(orig)) {
        if (IsOurDll(zlib))
            return L"zlib.dll is already the mod but its backup is missing. Use Steam's \"Verify integrity of game "
                   L"files\", then run the installer again.";
        if (!MoveFileW(zlib.c_str(), orig.c_str())) return L"Could not back up zlib.dll (error " + std::to_wstring(GetLastError()) + L").";
    } else if (!IsOurDll(zlib)) {
        // Steam restored the original after an update: refresh the backup.
        if (!CopyFileW(zlib.c_str(), orig.c_str(), FALSE)) return L"Could not refresh zlib_orig.dll.";
    }
    std::wstring tmp = zlib + L".new";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary);
        if (!f) return L"Could not write to the game folder (try running the installer as administrator).";
        f.write((const char*)data, size);
    }
    if (!MoveFileExW(tmp.c_str(), zlib.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return L"Could not replace zlib.dll (error " + std::to_wstring(GetLastError()) + L").";
    }
    return L"";
}

static std::wstring Uninstall(const std::wstring& dir) {
    if (GameRunning()) return L"Please close Grand Mountain Adventure first.";
    std::wstring zlib = Join(dir, L"zlib.dll"), orig = Join(dir, L"zlib_orig.dll");
    if (!Exists(orig)) return L"The mod is not installed in that folder.";
    if (Exists(zlib) && !DeleteFileW(zlib.c_str())) return L"Could not remove the mod's zlib.dll.";
    if (!MoveFileW(orig.c_str(), zlib.c_str())) return L"Could not restore the original zlib.dll.";
    // Leave the folder exactly as Steam installed it.
    for (const wchar_t* f : {L"gma_mp.ini", L"gma_mp.log", L"gma_mp_shot.req", L"gma_mp_shot.bmp"})
        DeleteFileW(Join(dir, f).c_str());
    return L"";
}

// ---- drawing ------------------------------------------------------------------
// The window is 640 x 420 logical pixels, scaled for the monitor's DPI, and
// laid out like the game's piste map screen: a rendered mountainside
// (scene.cpp) under the title and stat bands at the top left, key hints at the
// top right, italic links in the bottom corners and the map's "Travel to"
// button in the middle. Text is drawn as glyph outlines placed by the fonts'
// own metrics (baseline, ink edges) so everything lines up, and casts the
// game's soft drop shadow (a blurred layer under the controls).
using namespace Gdiplus;

constexpr float kW = 640, kH = 420;
constexpr float kLeft = 36, kRight = kW - 36;  // margins

static const Color kTextGrey(255, 222, 224, 228);  // the map's stat text
static const Color kCream(255, 255, 230, 179);     // the "Travel to" button text
static const Color kSalmon(255, 244, 154, 132);    // problems

// A font and the metrics used to place it (in ems).
struct Face {
    FontFamily* family = nullptr;
    int style = FontStyleRegular;
    float ascent = 0.8f;  // top of the line box to the baseline
};
static PrivateFontCollection* g_fonts;
static Face g_title;  // Staatliches (OFL, installer/fonts): the game's menu font
static Face g_link;   // Contrail One (OFL, installer/fonts): the game's italic "Menu" / "Next Mountain"
static Face g_body;   // Segoe UI Semibold: paths and help text

static void SetMetrics(Face& f) {
    f.ascent = (float)f.family->GetCellAscent(f.style) / f.family->GetEmHeight(f.style);
}
static void LoadFonts() {
    g_fonts = new PrivateFontCollection();
    for (int id : {IDR_TITLEFONT, IDR_LINKFONT}) {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
        HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
        if (h) g_fonts->AddMemoryFont(LockResource(h), (INT)SizeofResource(nullptr, res));
    }
    int count = g_fonts->GetFamilyCount(), found = 0;
    FontFamily* fams = new FontFamily[count > 0 ? count : 1];
    if (count > 0) g_fonts->GetFamilies(count, fams, &found);
    for (int i = 0; i < found; ++i) {
        wchar_t name[LF_FACESIZE] = {};
        fams[i].GetFamilyName(name);
        if (wcscmp(name, L"Staatliches") == 0) g_title.family = &fams[i];
        if (wcscmp(name, L"Contrail One") == 0) g_link.family = &fams[i];
    }
    g_body.family = new FontFamily(L"Segoe UI Semibold");
    if (g_body.family->GetLastStatus() != Ok) {
        delete g_body.family;
        g_body.family = new FontFamily(L"Segoe UI");
        g_body.style = FontStyleBold;
    }
    if (!g_title.family) g_title = g_body;
    if (!g_link.family) g_link.family = g_body.family, g_link.style = g_body.style | FontStyleItalic;
    for (Face* f : {&g_title, &g_link, &g_body}) SetMetrics(*f);
}

static void RoundRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static Color Alpha(Color c, float a) { return Color((BYTE)(c.GetA() * a), c.GetR(), c.GetG(), c.GetB()); }

enum Align { kAlignLeft, kAlignCenter, kAlignRight };
// The outline of s at px pixels with its baseline at `baseline`; x is where the
// ink's left edge, centre or right edge goes. Returns the ink's bounds.
static RectF Outline(GraphicsPath& out, const std::wstring& s, const Face& f, float px, float x, float baseline,
                     Align a = kAlignLeft) {
    GraphicsPath p;
    p.AddString(s.c_str(), (INT)s.size(), f.family, f.style, px, PointF(0, 0), StringFormat::GenericTypographic());
    RectF b;
    p.GetBounds(&b);
    float dx = a == kAlignLeft ? x - b.X : a == kAlignCenter ? x - (b.X + b.Width / 2) : x - (b.X + b.Width);
    float dy = baseline - f.ascent * px;
    Matrix m;
    m.Translate(dx, dy);
    p.Transform(&m);
    out.AddPath(&p, FALSE);
    return RectF(b.X + dx, b.Y + dy, b.Width, b.Height);
}
static float InkWidth(const std::wstring& s, const Face& f, float px) {
    GraphicsPath p;
    return Outline(p, s, f, px, 0, 0).Width;
}
// The height of capitals above the baseline, in pixels.
static float CapHeight(const Face& f, float px) {
    GraphicsPath p;
    return -Outline(p, L"H", f, px, 0, 0).Y;
}
// A folder path shortened from the middle ("C:\a\b\...\Game") to fit maxW.
static std::wstring FitPath(const std::wstring& path, const Face& f, float px, float maxW) {
    if (InkWidth(path, f, px) <= maxW) return path;
    std::vector<std::wstring> parts;
    std::wstringstream ss(path);
    for (std::wstring part; std::getline(ss, part, L'\\');) parts.push_back(part);
    for (size_t keep = parts.size() - 1; keep-- > 1;) {
        std::wstring s;
        for (size_t i = 0; i < keep; ++i) s += parts[i] + L"\\";
        s += L"\u2026\\" + parts.back();
        if (InkWidth(s, f, px) <= maxW) return s;
    }
    return L"\u2026\\" + parts.back();
}

// ---- GUI ----------------------------------------------------------------------
enum Hit { kNone, kClose, kMinimize, kBrowse, kInstall, kUninstall, kHitCount };
enum MsgKind { kInfo, kOk, kError };
static std::wstring g_dir;
static bool g_installed = false;
static Hit g_hover = kNone, g_pressed = kNone;
static std::wstring g_msg;
static MsgKind g_msgKind = kInfo;
static RectF g_hitRect[kHitCount];  // logical pixels, from the last layout

static bool Enabled(Hit h) {
    if (h == kInstall) return !g_dir.empty();
    if (h == kUninstall) return !g_dir.empty() && g_installed;
    return true;
}

// The window is drawn in three passes over the scene: bands under everything,
// then the drop shadows (into a layer that is blurred), then the controls.
enum Pass { kUnder, kShadow, kOver };
struct Ui {
    Graphics& g;
    Pass pass;
};

static void Fill(Ui& ui, GraphicsPath& p, Color c, float shadow = 1) {
    if (ui.pass == kOver) {
        SolidBrush b(c);
        ui.g.FillPath(&b, &p);
    } else if (ui.pass == kShadow && shadow > 0) {
        GraphicsPath s;
        s.AddPath(&p, FALSE);
        Matrix m;
        m.Translate(1.2f, 2.f);
        s.Transform(&m);
        SolidBrush b(Color((BYTE)(235 * shadow), 0, 0, 0));
        ui.g.FillPath(&b, &s);
    }
}
static RectF Text(Ui& ui, const std::wstring& s, const Face& f, float px, float x, float baseline, Color c,
                  Align a = kAlignLeft, float shadow = 1) {
    GraphicsPath p;
    RectF ink = Outline(p, s, f, px, x, baseline, a);
    Fill(ui, p, c, shadow);
    return ink;
}
// A translucent band behind a row of the stats: solid up to `solid`, then
// fading out over the next `fade` pixels.
static void Band(Ui& ui, float y, float h, float solid, float fade, bool highlight) {
    if (ui.pass != kUnder) return;
    Color c = highlight ? Color(56, 255, 255, 255) : Color(78, 10, 14, 24);
    float width = solid + fade;
    LinearGradientBrush b(PointF(0, 0), PointF(width, 0), c, Alpha(c, 0));
    Color colors[3] = {c, c, Alpha(c, 0)};
    REAL positions[3] = {0, solid / width, 1};
    b.SetInterpolationColors(colors, positions, 3);
    ui.g.FillRectangle(&b, 0.f, y, width, h);
}

// A key hint like the map's "TOGGLE OVERLAY [L]": a label and a key cap,
// right-aligned at x and centred on cy.
static void KeyHint(Ui& ui, Hit hit, const std::wstring& label, const std::wstring& key, float x, float cy) {
    bool hover = g_hover == hit;
    float capPx = 12.5f, keyW = key.empty() ? 20 : std::max(20.f, InkWidth(key, g_title, capPx) + 11);
    RectF cap(x - keyW, cy - 10, keyW, 20);
    GraphicsPath box;
    RoundRect(box, cap, 4);
    Fill(ui, box, hover ? kCream : Color(255, 240, 241, 243), 0.6f);
    if (ui.pass == kOver) {
        Color ink(255, 34, 38, 46);
        if (key.empty()) {  // minimize: a bar
            SolidBrush b(ink);
            ui.g.FillRectangle(&b, cap.X + 6, cy + 1.5f, cap.Width - 12, 2.f);
        } else {
            Text(ui, key, g_title, capPx, cap.X + cap.Width / 2, cy + CapHeight(g_title, capPx) / 2, ink, kAlignCenter, 0);
        }
    }
    float px = 17;
    RectF ink = Text(ui, label, g_title, px, cap.X - 8, cy + CapHeight(g_title, px) / 2, hover ? kCream : kTextGrey,
                     kAlignRight);
    g_hitRect[hit] = RectF(ink.X - 8, cy - 15, cap.GetRight() + 6 - (ink.X - 8), 30);
}

// An italic link like the map's "< Menu" / "Next Mountain".
static void Link(Ui& ui, Hit hit, const std::wstring& label, bool chevron, float x, float baseline, Align a) {
    bool on = Enabled(hit), hover = on && g_hover == hit, down = hover && g_pressed == hit;
    Color c = !on ? Alpha(Color(255, 236, 238, 242), 0.42f) : hover ? kCream : Color(255, 246, 247, 249);
    float px = 26, cap = CapHeight(g_link, px);
    float lift = down ? -1.f : 0.f;
    RectF ink;
    if (chevron) {  // "<", as in "< Menu"
        GraphicsPath stroke;
        float h = cap * 0.62f, cy = baseline - cap / 2;
        stroke.AddLine(PointF(x + h * 0.62f, cy - h / 2), PointF(x, cy));
        stroke.AddLine(PointF(x, cy), PointF(x + h * 0.62f, cy + h / 2));
        Pen pen(Color(255, 0, 0, 0), 3.f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        stroke.Widen(&pen);
        Fill(ui, stroke, c, on ? 1.f : 0.5f);
        ink = Text(ui, label, g_link, px, x + h * 0.62f + 9, baseline + lift, c, kAlignLeft, on ? 1.f : 0.5f);
        ink = RectF(x, ink.Y, ink.GetRight() - x, ink.Height);
    } else {
        ink = Text(ui, label, g_link, px, x, baseline + lift, c, a, on ? 1.f : 0.5f);
    }
    g_hitRect[hit] = RectF(ink.X - 8, ink.Y - 8, ink.Width + 16, ink.Height + 16);
}

// The map's "Travel to" button: translucent grey with a light rim, cream text.
static void Button(Ui& ui, Hit hit, const std::wstring& label, float cx, float cy) {
    bool on = Enabled(hit), hover = on && g_hover == hit, down = hover && g_pressed == hit;
    float px = 23, w = std::max(200.f, InkWidth(label, g_title, px) + 56), h = 42;
    RectF r(cx - w / 2, cy - h / 2, w, h);
    g_hitRect[hit] = r;
    GraphicsPath p;
    RoundRect(p, r, 7);
    if (ui.pass == kShadow) {
        SolidBrush b(Color(on ? 110 : 50, 0, 0, 0));
        GraphicsPath s;
        RoundRect(s, RectF(r.X, r.Y + 2, r.Width, r.Height), 7);
        ui.g.FillPath(&b, &s);
        return;
    }
    if (ui.pass != kOver) return;
    float a = on ? 1.f : 0.5f;
    int top = down ? 92 : hover ? 150 : 128, bottom = down ? 70 : hover ? 116 : 96;
    LinearGradientBrush fill(PointF(0, r.Y), PointF(0, r.GetBottom()), Color((BYTE)(200 * a), top, top, top + 2),
                             Color((BYTE)(200 * a), bottom, bottom, bottom + 3));
    ui.g.FillPath(&fill, &p);
    Pen outer(Color((BYTE)(120 * a), 0, 0, 0), 1.f);
    ui.g.DrawPath(&outer, &p);
    GraphicsPath rim;
    RoundRect(rim, RectF(r.X + 1.5f, r.Y + 1.5f, r.Width - 3, r.Height - 3), 5.5f);
    Pen inner(Color((BYTE)(210 * a), 232, 226, 212), 1.5f);
    ui.g.DrawPath(&inner, &rim);
    float base = cy + CapHeight(g_title, px) / 2 + (down ? 1.f : 0.f);
    GraphicsPath t, shade;
    Outline(t, label, g_title, px, cx, base, kAlignCenter);
    Outline(shade, label, g_title, px, cx + 1.2f, base + 1.8f, kAlignCenter);
    SolidBrush sb(Color((BYTE)(150 * a), 20, 18, 14)), tb(Alpha(kCream, a));
    ui.g.FillPath(&sb, &shade);
    ui.g.FillPath(&tb, &t);
}

// One row of the stats, like the map's "SKI PASSES 41 / 41": a label and a
// value (starting at x) on a band that fades out after them.
constexpr float kRowTop = 108, kRowH = 30, kRowGap = 3, kValueX = 152;
static void Row(Ui& ui, int row, const std::wstring& label, const std::wstring& value, const Face& f, float px, Color c,
                float x = kValueX, bool highlight = false) {
    float top = kRowTop + row * (kRowH + kRowGap), cy = top + kRowH / 2;
    float right = std::max(x + InkWidth(value, f, px), 260.f);
    Band(ui, top, kRowH, right + 16, 110, highlight);
    if (!label.empty()) Text(ui, label, g_title, 19, kLeft, cy + CapHeight(g_title, 19) / 2, kTextGrey);
    Text(ui, value, f, px, x, cy + CapHeight(f, px) / 2, c);
}

static void Layout(Ui& ui) {
    // a little shade behind the title and the links so they read on bright snow
    if (ui.pass == kUnder) {
        LinearGradientBrush top(PointF(0, 0), PointF(0, 150), Color(70, 8, 12, 22), Color(0, 8, 12, 22));
        ui.g.FillRectangle(&top, 0.f, 0.f, kW, 150.f);
        LinearGradientBrush bottom(PointF(0, kH - 90), PointF(0, kH), Color(0, 8, 12, 22), Color(96, 8, 12, 22));
        ui.g.FillRectangle(&bottom, 0.f, kH - 90, kW, 90.f);
    }

    // title, like the mountain's name on the map
    Text(ui, L"GRAND MULTIPLAYER ADVENTURE", g_title, 38, kLeft, 66, kTextGrey);
    Text(ui, L"for Grand Mountain Adventure: Wonderlands", g_link, 17, kLeft + 1, 90, Alpha(kCream, 0.92f));

    // stats
    if (g_dir.empty()) Row(ui, 0, L"STATUS", L"GAME NOT FOUND", g_title, 19, kSalmon);
    else Row(ui, 0, L"STATUS", g_installed ? L"INSTALLED" : L"NOT INSTALLED", g_title, 19, g_installed ? kCream : kTextGrey);
    std::wstring ver = W(GMAMP_VERSION);
    for (auto& ch : ver) ch = (wchar_t)towupper(ch);
    Row(ui, 1, L"VERSION", ver, g_title, 19, kTextGrey);
    const float bodyPx = 13.5f;
    const Color body(245, 246, 247, 249);
    if (g_dir.empty()) Row(ui, 2, L"GAME FOLDER", L"Not found \u2014 pick it with Change Folder", g_body, bodyPx, body);
    else Row(ui, 2, L"GAME FOLDER", FitPath(g_dir, g_body, bodyPx, kRight - kValueX), g_body, bodyPx, body);
    if (!g_msg.empty()) {
        Color c = g_msgKind == kOk ? kCream : g_msgKind == kError ? kSalmon : Color(255, 246, 247, 249);
        Row(ui, 3, L"", g_msg, g_link, 18, c, kLeft, true);
    }

    // key hints, top right
    KeyHint(ui, kClose, L"CLOSE", L"ESC", kRight, 30);
    KeyHint(ui, kMinimize, L"MINIMIZE", L"", kRight, 58);

    // actions along the bottom, centred on the links' capitals
    const float linkBase = kH - 26;
    const float midY = linkBase - CapHeight(g_link, 26) / 2;
    Link(ui, kUninstall, L"Uninstall", true, kLeft, linkBase, kAlignLeft);
    Link(ui, kBrowse, L"Change Folder", false, kRight, linkBase, kAlignRight);
    Button(ui, kInstall, g_installed ? L"UPDATE MULTIPLAYER" : L"INSTALL MULTIPLAYER", kW / 2, midY);

    if (ui.pass == kOver) {  // hairline frame
        Pen frame(Color(110, 0, 0, 0), 1.f);
        ui.g.DrawRectangle(&frame, 0.f, 0.f, kW - 1, kH - 1);
    }
}

// The rendered mountainside for a w x h window, cached.
static Bitmap* g_scene;
static Bitmap* Scene(int w, int h) {
    if (g_scene && (int)g_scene->GetWidth() == w && (int)g_scene->GetHeight() == h) return g_scene;
    delete g_scene;
    g_scene = new Bitmap(w, h, PixelFormat32bppARGB);
    std::vector<uint32_t> px((size_t)w * h);
    RenderScene(w, h, px.data());
    BitmapData d;
    Rect all(0, 0, w, h);
    if (g_scene->LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &d) == Ok) {
        for (int y = 0; y < h; ++y) memcpy((BYTE*)d.Scan0 + y * d.Stride, &px[(size_t)y * w], w * 4);
        g_scene->UnlockBits(&d);
    }
    return g_scene;
}

// Softens the shadow layer: a box blur of its alpha, three times (~gaussian).
static void BlurShadow(Bitmap& b, int radius) {
    int w = b.GetWidth(), h = b.GetHeight();
    BitmapData d;
    Rect all(0, 0, w, h);
    if (b.LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &d) != Ok) return;
    std::vector<float> a((size_t)w * h), t((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) a[(size_t)y * w + x] = ((BYTE*)d.Scan0)[y * d.Stride + x * 4 + 3];
    float inv = 1.f / (2 * radius + 1);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            float acc = 0;
            const float* row = &a[(size_t)y * w];
            for (int i = -radius; i <= radius; ++i) acc += row[std::min(std::max(i, 0), w - 1)];
            for (int x = 0; x < w; ++x) {
                t[(size_t)y * w + x] = acc * inv;
                acc += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
            }
        }
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (int i = -radius; i <= radius; ++i) acc += t[(size_t)std::min(std::max(i, 0), h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                a[(size_t)y * w + x] = acc * inv;
                acc += t[(size_t)std::min(y + radius + 1, h - 1) * w + x] - t[(size_t)std::max(y - radius, 0) * w + x];
            }
        }
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE* p = (BYTE*)d.Scan0 + y * d.Stride + x * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = (BYTE)std::min(255.f, a[(size_t)y * w + x]);
        }
    b.UnlockBits(&d);
}

// The whole window at `scale` into `out` (w x h pixels).
static void PaintWindow(Bitmap& out, float scale) {
    int w = out.GetWidth(), h = out.GetHeight();
    Graphics g(&out);
    g.DrawImage(Scene(w, h), 0, 0, w, h);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(scale, scale);
    Ui under{g, kUnder};
    Layout(under);
    Bitmap shadow(w, h, PixelFormat32bppPARGB);
    {
        Graphics sg(&shadow);
        sg.SetSmoothingMode(SmoothingModeAntiAlias);
        sg.ScaleTransform(scale, scale);
        Ui ui{sg, kShadow};
        Layout(ui);
    }
    BlurShadow(shadow, std::max(1, (int)std::lround(1.6f * scale)));
    g.ResetTransform();
    g.DrawImage(&shadow, 0, 0, w, h);
    g.ScaleTransform(scale, scale);
    Ui over{g, kOver};
    Layout(over);
}

static float Scale() { return GetDpiForWindow(g_wnd) / 96.f; }
static Hit HitAt(int px, int py) {
    float s = Scale(), x = px / s, y = py / s;
    for (Hit h : {kClose, kMinimize, kBrowse, kInstall, kUninstall})
        if (g_hitRect[h].Contains(x, y)) return h;
    return kNone;
}
static void Refresh() {
    g_installed = !g_dir.empty() && IsInstalled(g_dir);
    InvalidateRect(g_wnd, nullptr, FALSE);
}
static void SetMessage(MsgKind k, const std::wstring& m) {
    g_msgKind = k;
    g_msg = m;
    Refresh();
}

static void Act(Hit h) {
    if (!Enabled(h)) return;
    if (h == kClose) DestroyWindow(g_wnd);
    else if (h == kMinimize) ShowWindow(g_wnd, SW_MINIMIZE);
    else if (h == kBrowse) {
        BROWSEINFOW bi{};
        bi.hwndOwner = g_wnd;
        bi.lpszTitle = L"Select the GMA Wonderlands folder (contains gma_wonderlands.exe)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) {
                if (Exists(Join(path, kExeName))) {
                    g_dir = OnDiskPath(path);
                    SetMessage(kInfo, L"");
                } else {
                    SetMessage(kError, L"gma_wonderlands.exe is not in that folder.");
                }
            }
            CoTaskMemFree(pidl);
        }
        Refresh();
    } else if (h == kInstall) {
        bool update = g_installed;
        std::wstring err = Install(g_dir);
        if (err.empty()) {
            AllowThroughFirewall(g_dir);
            SetMessage(kOk, update ? L"Updated. Start the game from Steam." : L"Installed! Start the game from Steam.");
        } else {
            SetMessage(kError, err);
        }
    } else if (h == kUninstall) {
        std::wstring err = Uninstall(g_dir);
        if (err.empty()) RemoveFirewallRule();
        if (err.empty()) SetMessage(kOk, L"Removed. The game's own files are restored.");
        else SetMessage(kError, err);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            Bitmap buf(rc.right, rc.bottom, PixelFormat32bppPARGB);
            PaintWindow(buf, Scale());
            Graphics out(dc);
            out.DrawImage(&buf, 0, 0);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(h, &pt);
        return HitAt(pt.x, pt.y) == kNone ? HTCAPTION : HTCLIENT;  // drag anywhere else
    }
    case WM_MOUSEMOVE: {
        Hit now = HitAt((short)LOWORD(lp), (short)HIWORD(lp));
        if (now != g_hover) {
            g_hover = now;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = kNone;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        g_pressed = HitAt((short)LOWORD(lp), (short)HIWORD(lp));
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        Hit was = g_pressed;
        g_pressed = kNone;
        ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        if (was != kNone && was == HitAt((short)LOWORD(lp), (short)HIWORD(lp))) Act(was);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover != kNone && Enabled(g_hover)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (wp == VK_RETURN) Act(kInstall);
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- development: preview and icon ----------------------------------------------
static bool EncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *out = info[i].Clsid;
            return true;
        }
    return false;
}
// --preview <png> [scale] [hover] [message kind] [message]: the window as a PNG.
static int Preview(const std::vector<std::wstring>& args) {
    float scale = args.size() >= 3 ? (float)_wtof(args[2].c_str()) : 1.f;
    g_dir = FindGameDir();
    g_installed = !g_dir.empty() && IsInstalled(g_dir);
    if (args.size() >= 4) g_hover = (Hit)_wtoi(args[3].c_str());
    if (args.size() >= 6) g_msgKind = (MsgKind)_wtoi(args[4].c_str()), g_msg = args[5];
    Bitmap bmp((INT)std::lround(kW * scale), (INT)std::lround(kH * scale), PixelFormat32bppPARGB);
    PaintWindow(bmp, scale);
    CLSID png;
    return EncoderClsid(L"image/png", &png) && bmp.Save(args[1].c_str(), &png, nullptr) == Ok ? 0 : 1;
}

// The app icon: the rendered peak (scene.cpp) on a rounded tile, drawn at s
// pixels from the 256-pixel render.
static void DrawIcon(Graphics& g, Bitmap& art, float s) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float inset = s >= 32 ? s / 64.f : 0, rad = s * 0.2f;
    RectF tile(inset, inset, s - 2 * inset, s - 2 * inset);
    GraphicsPath p;
    RoundRect(p, tile, rad);
    // a downscaled copy as the fill, so small sizes are filtered, not sampled
    Bitmap scaled((INT)std::ceil(s), (INT)std::ceil(s), PixelFormat32bppARGB);
    {
        Graphics sg(&scaled);
        sg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        sg.SetPixelOffsetMode(PixelOffsetModeHalf);
        sg.DrawImage(&art, tile);
    }
    TextureBrush fill(&scaled, WrapModeClamp);
    g.FillPath(&fill, &p);
    if (s >= 24) {  // a faint rim so it holds its shape on light backgrounds
        Pen rim(Color(70, 10, 20, 40), std::max(1.f, s / 128.f));
        g.DrawPath(&rim, &p);
    }
}
static int WriteIcon(const std::wstring& path) {
    CLSID png;
    if (!EncoderClsid(L"image/png", &png)) return 1;
    const int sizes[] = {256, 64, 48, 32, 24, 16};
    Bitmap art(256, 256, PixelFormat32bppARGB);
    {
        std::vector<uint32_t> px(256 * 256);
        RenderIconScene(256, px.data());
        BitmapData d;
        Rect all(0, 0, 256, 256);
        if (art.LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &d) != Ok) return 1;
        for (int y = 0; y < 256; ++y) memcpy((BYTE*)d.Scan0 + y * d.Stride, &px[(size_t)y * 256], 256 * 4);
        art.UnlockBits(&d);
    }
    std::vector<std::string> images;
    for (int s : sizes) {
        Bitmap bmp(s, s, PixelFormat32bppARGB);
        {
            Graphics g(&bmp);
            DrawIcon(g, art, (float)s);
        }
        IStream* st = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &st);
        bmp.Save(st, &png, nullptr);
        STATSTG stat{};
        st->Stat(&stat, STATFLAG_NONAME);
        std::string data((size_t)stat.cbSize.QuadPart, 0);
        LARGE_INTEGER zero{};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        st->Read(&data[0], (ULONG)data.size(), &got);
        st->Release();
        images.push_back(data);
    }
    std::ofstream f(path.c_str(), std::ios::binary);
    auto u16 = [&](unsigned v) { f.put((char)(v & 255)).put((char)(v >> 8)); };
    auto u32 = [&](unsigned v) { u16(v & 0xffff), u16(v >> 16); };
    u16(0), u16(1), u16((unsigned)images.size());
    unsigned offset = 6 + 16 * (unsigned)images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        f.put((char)(sizes[i] >= 256 ? 0 : sizes[i])).put((char)(sizes[i] >= 256 ? 0 : sizes[i])).put(0).put(0);
        u16(1), u16(32), u32((unsigned)images[i].size()), u32(offset);
        offset += (unsigned)images[i].size();
    }
    for (auto& d : images) f.write(d.data(), d.size());
    return f ? 0 : 1;
}

// ---- command line -------------------------------------------------------------
static int RunCli(const std::vector<std::wstring>& args) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    std::wstring action, dir;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--install" || args[i] == L"--uninstall" || args[i] == L"--status") action = args[i];
        else if (args[i] == L"--dir" && i + 1 < args.size()) dir = args[++i];
    }
    if (dir.empty()) dir = FindGameDir();
    if (dir.empty()) {
        printf("Grand Mountain Adventure: Wonderlands was not found; pass --dir\n");
        return 2;
    }
    if (action == L"--status") {
        bool inst = IsInstalled(dir);
        printf("%s: %s\n", U8(dir).c_str(), inst ? "installed" : "not installed");
        return inst ? 0 : 3;
    }
    std::wstring err = action == L"--uninstall" ? Uninstall(dir) : Install(dir);
    printf("%s\n", err.empty() ? "OK" : U8(err).c_str());
    return err.empty() ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    GdiplusStartup(&gdiplus, &gsi, nullptr);
    LoadFonts();
    if (args.size() >= 2 && args[0] == L"--preview") return Preview(args);
    if (args.size() >= 2 && args[0] == L"--icon") return WriteIcon(args[1]);
    // the firewall step, run as administrator by the GUI (internal)
    if (args.size() >= 2 && args[0] == L"--firewall-allow") return FirewallElevated(true, args[1]);
    if (!args.empty() && args[0] == L"--firewall-remove") return FirewallElevated(false, L"");
    if (!args.empty()) return RunCli(args);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitialize(nullptr);

    WNDCLASSW wc{};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GmaMpSetup";
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
    RegisterClassW(&wc);
    std::wstring title = std::wstring(kAppName) + L" Setup";
    g_wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, title.c_str(), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                            0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    // centred on the monitor it opens on, at its DPI
    float s = Scale();
    int w = (int)std::lround(kW * s), h = (int)std::lround(kH * s);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wa = mi.rcWork;
    SetWindowPos(g_wnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_NOZORDER);
    DWORD round = 2;  // DWMWCP_ROUND (Windows 11; ignored before)
    DwmSetWindowAttribute(g_wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);

    g_dir = FindGameDir();
    Refresh();
    Scene(w, h);  // render the backdrop before the window shows
    ShowWindow(g_wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    GdiplusShutdown(gdiplus);
    return 0;
}
