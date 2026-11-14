#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

static std::mutex g_logMu;
static FILE* g_log = nullptr;

static FILE* Open() {
    if (g_log) return g_log;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"gma_mp.log";
    g_log = _wfopen(p.c_str(), L"w");
    return g_log;
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMu);
    FILE* f = Open();
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}
