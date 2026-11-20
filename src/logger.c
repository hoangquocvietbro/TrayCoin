/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "logger.h"
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION g_logCs;
static int              g_logInited = 0;
static volatile LONG    g_logEnabled = 0;
static wchar_t          g_logPath[MAX_PATH] = {0};

void log_get_path(wchar_t *outPath, int cap)
{
    if (!outPath || cap <= 0) return;
    if (g_logPath[0]) {
        lstrcpynW(outPath, g_logPath, cap);
        return;
    }
    wchar_t cfgPath[MAX_PATH] = {0};
    config_get_path(cfgPath, MAX_PATH);
    wchar_t *slash = wcsrchr(cfgPath, L'\\');
    if (slash) {
        slash[1] = 0;
        _snwprintf(outPath, cap, L"%straycoin.log", cfgPath);
    } else {
        lstrcpynW(outPath, L"traycoin.log", cap);
    }
    outPath[cap - 1] = 0;
}

void log_init(void)
{
    if (g_logInited) return;
    InitializeCriticalSection(&g_logCs);
    g_logInited = 1;
    log_get_path(g_logPath, MAX_PATH);
}

void log_shutdown(void)
{
    if (!g_logInited) return;
    DeleteCriticalSection(&g_logCs);
    g_logInited = 0;
}

void log_set_enabled(int enabled)
{
    LONG prev = InterlockedExchange(&g_logEnabled, enabled ? 1 : 0);
    if (enabled && !prev) {
        log_write(LOG_INFO, L"LOG", L"=== Logging enabled (TrayCoin %s) ===", APP_VERSION);
    }
}

int log_is_enabled(void)
{
    return g_logEnabled != 0;
}

void log_clear(void)
{
    if (!g_logInited) return;
    EnterCriticalSection(&g_logCs);
    if (g_logPath[0]) {
        HANDLE f = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    LeaveCriticalSection(&g_logCs);
}

static void write_raw_line_locked(const char *utf8Line)
{
    if (!g_logPath[0]) log_get_path(g_logPath, MAX_PATH);
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;

    /* Auto-rotate/truncate if > 2 MB */
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > (2 << 20)) {
        CloseHandle(f);
        wchar_t bak[MAX_PATH];
        _snwprintf(bak, MAX_PATH, L"%s.old", g_logPath);
        bak[MAX_PATH - 1] = 0;
        MoveFileExW(g_logPath, bak, MOVEFILE_REPLACE_EXISTING);
        f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) return;
    }

    DWORD wr = 0;
    WriteFile(f, utf8Line, (DWORD)strlen(utf8Line), &wr, NULL);
    CloseHandle(f);
}

void log_write(LogLevel lvl, const wchar_t *tag, const wchar_t *fmt, ...)
{
    if (!g_logEnabled && lvl < LOG_ERROR) return;
    if (!g_logEnabled) return;
    if (!g_logInited) log_init();

    static const wchar_t *LVL_STR[4] = { L"DEBUG", L"INFO ", L"WARN ", L"ERROR" };
    if (lvl < LOG_DEBUG || lvl > LOG_ERROR) lvl = LOG_INFO;

    wchar_t body[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(body, 511, fmt, ap);
    va_end(ap);
    body[511] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t full[640];
    _snwprintf(full, 640, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] [T:%5lu] [%-7s] %s\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               LVL_STR[lvl], (unsigned long)GetCurrentThreadId(), tag ? tag : L"APP", body);
    full[639] = 0;

    char utf8[1280];
    wide_to_utf8(full, utf8, sizeof(utf8));

    EnterCriticalSection(&g_logCs);
    write_raw_line_locked(utf8);
    LeaveCriticalSection(&g_logCs);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;

    wchar_t modPath[MAX_PATH] = L"<unknown>";
    HMODULE hm = NULL;
    uintptr_t rva = 0;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &hm) && hm) {
        GetModuleFileNameW(hm, modPath, MAX_PATH);
        rva = (uintptr_t)addr - (uintptr_t)hm;
    }

    wchar_t line[640];
    _snwprintf(line, 640,
               L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [CRASH] [T:%5lu] Unhandled exception 0x%08lX at 0x%p (%s + 0x%llX)\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               (unsigned long)GetCurrentThreadId(), (unsigned long)code, addr, modPath, (unsigned long long)rva);
    line[639] = 0;

    char utf8[1280];
    wide_to_utf8(line, utf8, sizeof(utf8));

    /* Always persist crash info even if debug logging was off */
    if (!g_logPath[0]) log_get_path(g_logPath, MAX_PATH);
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, utf8, (DWORD)strlen(utf8), &wr, NULL);
        CloseHandle(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void log_install_crash_handler(void)
{
    SetUnhandledExceptionFilter(crash_filter);
}
