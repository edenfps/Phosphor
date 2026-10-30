#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <share.h>

static FILE* g_log = nullptr;
static bool g_console = false;

void EnableConsoleLog()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
    {
        FILE* f = nullptr;
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        g_console = true;
    }
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    // Shared read so the log can be tailed while the overlay runs.
    if (!g_log)
        g_log = _wfsopen((ExeDir() + L"\\phosphor.log").c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    if (g_log)
    {
        fwprintf(g_log, L"%s\n", buf);
        fflush(g_log);
    }
    if (g_console)
    {
        fwprintf(stdout, L"%s\n", buf);
        fflush(stdout);
    }
}

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring FindAsset(const std::wstring& rel)
{
    std::wstring dir = ExeDir();
    for (int i = 0; i < 5; i++)
    {
        std::wstring p = dir + L"\\" + rel;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
        size_t cut = dir.find_last_of(L"\\/");
        if (cut == std::wstring::npos)
            break;
        dir = dir.substr(0, cut);
    }
    return ExeDir() + L"\\" + rel;
}

uint64_t FileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}
