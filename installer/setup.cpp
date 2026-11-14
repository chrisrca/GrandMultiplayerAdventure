// Grand Multiplayer Adventure - one-file installer.
//
// GUI (double-click): finds the game through Steam, installs or removes the mod
// (zlib.dll proxy, embedded in this exe) and optionally adds the Windows
// Firewall rule hosts need. All multiplayer settings live in the game itself
// (Settings > Multiplayer); the mod writes its defaults on first launch.
//
// Command line (used by CI and for scripting):
//   GMA-Multiplayer-Setup.exe --install [--dir <game dir>] [--firewall]
//   GMA-Multiplayer-Setup.exe --uninstall [--dir <game dir>]
//   GMA-Multiplayer-Setup.exe --status [--dir <game dir>]      (exit 0 = installed, 3 = not)
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GMAMP_VERSION
#define GMAMP_VERSION "dev"
#endif
#define IDR_MODDLL 101

static const wchar_t* kExeName = L"gma_wonderlands.exe";
static const wchar_t* kRuleName = L"Grand Multiplayer Adventure";

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
        if (Exists(Join(dir, kExeName))) return dir;
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

// Runs a command hidden and returns its exit code (or -1).
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
static bool FirewallRuleExists() {
    return RunHidden(std::wstring(L"netsh advfirewall firewall show rule name=\"") + kRuleName + L"\"") == 0;
}
// Elevated (UAC prompt). Returns true if the rule exists afterwards.
static bool RunElevated(const std::wstring& params) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    WaitForSingleObject(sei.hProcess, 30000);
    CloseHandle(sei.hProcess);
    return true;
}
static bool AddFirewallRule(const std::wstring& dir) {
    if (FirewallRuleExists()) return true;
    RunElevated(std::wstring(L"advfirewall firewall add rule name=\"") + kRuleName +
                L"\" dir=in action=allow protocol=UDP profile=any program=\"" + Join(dir, kExeName) + L"\"");
    return FirewallRuleExists();
}
static void RemoveFirewallRule() {
    if (FirewallRuleExists())
        RunElevated(std::wstring(L"advfirewall firewall delete rule name=\"") + kRuleName + L"\"");
}

// ---- install / uninstall ------------------------------------------------------
static std::wstring Install(const std::wstring& dir, bool firewall) {
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
    if (firewall && !AddFirewallRule(dir))
        return L"Installed, but the Windows Firewall rule was not added (the admin prompt was declined?). Hosting may "
               L"not work until it is.";
    return L"";
}

static std::wstring Uninstall(const std::wstring& dir, bool removeRule) {
    if (GameRunning()) return L"Please close Grand Mountain Adventure first.";
    std::wstring zlib = Join(dir, L"zlib.dll"), orig = Join(dir, L"zlib_orig.dll");
    if (!Exists(orig)) return L"The mod is not installed in that folder.";
    if (Exists(zlib) && !DeleteFileW(zlib.c_str())) return L"Could not remove the mod's zlib.dll.";
    if (!MoveFileW(orig.c_str(), zlib.c_str())) return L"Could not restore the original zlib.dll.";
    // Leave the folder exactly as Steam installed it.
    for (const wchar_t* f : {L"gma_mp.ini", L"gma_mp.log", L"gma_mp_shot.req", L"gma_mp_shot.bmp"})
        DeleteFileW(Join(dir, f).c_str());
    if (removeRule) RemoveFirewallRule();
    return L"";
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
    bool firewall = false;
    for (const std::wstring& a : args)
        if (a == L"--firewall") firewall = true;
    if (action == L"--status") {
        bool inst = IsInstalled(dir);
        printf("%s: %s\n", U8(dir).c_str(), inst ? "installed" : "not installed");
        return inst ? 0 : 3;
    }
    std::wstring err = action == L"--uninstall" ? Uninstall(dir, false) : Install(dir, firewall);
    printf("%s\n", err.empty() ? "OK" : U8(err).c_str());
    return err.empty() ? 0 : 1;
}

// ---- GUI ----------------------------------------------------------------------
enum {
    ID_DIR = 100, ID_BROWSE, ID_STATUS, ID_FIREWALL, ID_INSTALL, ID_UNINSTALL, ID_CLOSE, ID_NOTE
};
static HWND g_wnd;
static HFONT g_font, g_titleFont;
static std::wstring g_dir;

static void Refresh() {
    SetDlgItemTextW(g_wnd, ID_DIR, g_dir.empty() ? L"(not found - click Browse)" : g_dir.c_str());
    std::wstring st = g_dir.empty() ? L"Game not found" : (IsInstalled(g_dir) ? L"Status: installed" : L"Status: not installed");
    SetDlgItemTextW(g_wnd, ID_STATUS, st.c_str());
    EnableWindow(GetDlgItem(g_wnd, ID_UNINSTALL), !g_dir.empty() && IsInstalled(g_dir));
    EnableWindow(GetDlgItem(g_wnd, ID_INSTALL), !g_dir.empty());
    SetDlgItemTextW(g_wnd, ID_INSTALL, !g_dir.empty() && IsInstalled(g_dir) ? L"Update" : L"Install");
}

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    UINT dpi = GetDpiForWindow(g_wnd);
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_CLOSE) DestroyWindow(h);
        else if (id == ID_BROWSE) {
            BROWSEINFOW bi{};
            bi.hwndOwner = h;
            bi.lpszTitle = L"Select the GMA Wonderlands folder (contains gma_wonderlands.exe)";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, path)) {
                    if (Exists(Join(path, kExeName))) {
                        g_dir = path;
                    } else {
                        MessageBoxW(h, L"gma_wonderlands.exe is not in that folder.", L"Grand Multiplayer Adventure",
                                    MB_ICONWARNING);
                    }
                }
                CoTaskMemFree(pidl);
            }
            Refresh();
        } else if (id == ID_INSTALL) {
            std::wstring err = Install(g_dir, IsDlgButtonChecked(h, ID_FIREWALL) == BST_CHECKED);
            Refresh();
            if (err.empty())
                MessageBoxW(h,
                            L"Installed! Start Grand Mountain Adventure from Steam.\n\nTo host or join, open "
                            L"Settings > Multiplayer in the game. F8 shows / hides the player list.",
                            L"Grand Multiplayer Adventure", MB_ICONINFORMATION);
            else
                MessageBoxW(h, err.c_str(), L"Grand Multiplayer Adventure", MB_ICONWARNING);
        } else if (id == ID_UNINSTALL) {
            bool rule = FirewallRuleExists() &&
                        MessageBoxW(h, L"Also remove the Windows Firewall rule? (asks for admin)",
                                    L"Grand Multiplayer Adventure", MB_YESNO | MB_ICONQUESTION) == IDYES;
            std::wstring err = Uninstall(g_dir, rule);
            Refresh();
            MessageBoxW(h, err.empty() ? L"The mod was removed and the original game files restored." : err.c_str(),
                        L"Grand Multiplayer Adventure", err.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (!args.empty()) return RunCli(args);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitialize(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"GmaMpSetup";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    std::wstring title = L"Grand Multiplayer Adventure " + W(GMAMP_VERSION) + L" - Setup";
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    UINT dpi = GetDpiForWindow(g_wnd);
    RECT rc{0, 0, MulDiv(520, dpi, 96), MulDiv(252, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    g_font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Segoe UI");
    g_titleFont = CreateFontW(-MulDiv(15, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");

    HWND t = Ctl(L"STATIC", L"Grand Multiplayer Adventure", 0, 20, 14, 480, 30, 0);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_titleFont, TRUE);
    Ctl(L"STATIC", L"Multiplayer mod for Grand Mountain Adventure: Wonderlands", 0, 20, 44, 480, 18, 0);
    Ctl(L"STATIC", L"Game folder", 0, 20, 76, 100, 18, 0);
    Ctl(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 20, 96, 390, 24, ID_DIR);
    Ctl(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 418, 95, 82, 26, ID_BROWSE);
    Ctl(L"STATIC", L"", 0, 20, 124, 480, 18, ID_STATUS);
    Ctl(L"BUTTON", L"Allow the game through Windows Firewall (needed to host; asks for admin)",
        BS_AUTOCHECKBOX | WS_TABSTOP, 20, 150, 480, 22, ID_FIREWALL);
    Ctl(L"STATIC", L"Hosting and joining are set up in the game: Settings > Multiplayer.", 0, 20, 178, 480, 18,
        ID_NOTE);
    Ctl(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 236, 212, 90, 28, ID_INSTALL);
    Ctl(L"BUTTON", L"Uninstall", BS_PUSHBUTTON | WS_TABSTOP, 332, 212, 90, 28, ID_UNINSTALL);
    Ctl(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 428, 212, 72, 28, ID_CLOSE);

    g_dir = FindGameDir();
    CheckDlgButton(g_wnd, ID_FIREWALL, FirewallRuleExists() ? BST_UNCHECKED : BST_CHECKED);
    Refresh();
    ShowWindow(g_wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
