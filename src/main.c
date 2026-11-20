/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro) - Modified 2026-11
 * Based on TraySense, Copyright (C) 2026 DMTWEAKS.
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 * See LICENSE and NOTICE for full copyright and modification details.
 */
#include "types.h"
#include "config.h"
#include "http.h"
#include "providers.h"
#include "poller.h"
#include "ws.h"
#include "alerts.h"
#include "logger.h"
#include "taskbar.h"
#include "widget.h"
#include "ui_settings.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>

HWND             g_wnd = NULL;
HINSTANCE        g_hinst = NULL;
CRITICAL_SECTION g_stateCs;
AppConfig        g_cfg;
volatile LONG    g_quit = 0;
volatile LONG    g_paused = 0;
volatile LONG    g_anyFetching = 0;
int              g_light = 0;

static UINT g_taskbarCreated = 0;
/* GUID_MONITOR_POWER_ON: {02731015-4510-4526-99E6-E5A17EBD1AEA} */
static const GUID G_MONITOR_ON = {0x02731015,0x4510,0x4526,{0x99,0xe6,0xe5,0xa1,0x7e,0xbd,0x1a,0xea}};
static int g_sawDisplayEvent = 0;

enum {
    HK_TOGGLE_WIDGET = 1,
    HK_TOGGLE_LOCK   = 2,
    HK_REFRESH_NOW   = 3
};

enum {
    TIMER_ANIM        = 1,
    TIMER_ROTATE      = 2,
    TIMER_NET_RECOVER = 3
};

/* Dynamic iphlpapi.dll network-interface watcher (Wi-Fi switch / Ethernet / VPN / reconnect) */
typedef VOID  (WINAPI *PFN_IP_IF_CB)(PVOID CallerContext, PVOID Row, DWORD NotificationType);
typedef DWORD (WINAPI *PFN_NOTIFY_IP_IF_CHANGE)(USHORT Family, PFN_IP_IF_CB Callback, PVOID CallerContext, BOOLEAN InitialNotification, HANDLE *NotificationHandle);
typedef DWORD (WINAPI *PFN_CANCEL_MIB_NOTIFY2)(HANDLE NotificationHandle);

static HMODULE g_hIphlp = NULL;
static HANDLE  g_netNotify = NULL;
static PFN_CANCEL_MIB_NOTIFY2 g_pCancelMib = NULL;

static VOID WINAPI net_if_change_cb(PVOID ctx, PVOID row, DWORD notifType)
{
    (void)ctx; (void)row;
    if (!g_quit && g_wnd) {
        PostMessageW(g_wnd, WM_NET_CHANGED, (WPARAM)notifType, 0);
    }
}

static void net_watcher_start(void)
{
    if (g_netNotify) return;
    g_hIphlp = LoadLibraryExW(L"iphlpapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_hIphlp) g_hIphlp = LoadLibraryW(L"iphlpapi.dll");
    if (!g_hIphlp) return;

    PFN_NOTIFY_IP_IF_CHANGE pNotify = (PFN_NOTIFY_IP_IF_CHANGE)(void *)GetProcAddress(g_hIphlp, "NotifyIpInterfaceChange");
    g_pCancelMib = (PFN_CANCEL_MIB_NOTIFY2)(void *)GetProcAddress(g_hIphlp, "CancelMibChangeNotify2");
    if (pNotify && g_pCancelMib) {
        DWORD st = pNotify(0 /* AF_UNSPEC */, net_if_change_cb, NULL, FALSE, &g_netNotify);
        if (st == NO_ERROR && g_netNotify) {
            LOGI(L"NET", L"Network interface watcher active (NotifyIpInterfaceChange)");
        } else {
            LOGW(L"NET", L"NotifyIpInterfaceChange failed (status=%lu)", (unsigned long)st);
            g_netNotify = NULL;
        }
    }
}

static void net_watcher_stop(void)
{
    if (g_netNotify && g_pCancelMib) {
        g_pCancelMib(g_netNotify);
        g_netNotify = NULL;
    }
    if (g_hIphlp) {
        FreeLibrary(g_hIphlp);
        g_hIphlp = NULL;
    }
    g_pCancelMib = NULL;
}

static int g_animTimerOn = 0;

static int has_active_animation(void)
{
    if (g_anyFetching) return 1;
    if (!g_cfg.enableTickFlash) return 0;
    ULONGLONG now = GetTickCount64();
    int active = 0;
    EnterCriticalSection(&g_stateCs);
    for (int i = 0; i < g_cfg.coinCount; i++) {
        const CoinItem *c = &g_cfg.coins[i];
        if (c->enabled && c->flashDir != 0 && c->flashStartMs > 0 &&
            now >= c->flashStartMs && (now - c->flashStartMs) < 550ULL) {
            active = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_stateCs);
    return active;
}

void app_ensure_anim_timer(void)
{
    if (!g_wnd) return;
    if (has_active_animation()) {
        if (!g_animTimerOn) {
            SetTimer(g_wnd, TIMER_ANIM, 66, NULL);
            g_animTimerOn = 1;
        }
    } else if (g_animTimerOn) {
        KillTimer(g_wnd, TIMER_ANIM);
        g_animTimerOn = 0;
    }
}

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

void app_update_hotkeys(void)
{
    if (!g_wnd) return;
    UnregisterHotKey(g_wnd, HK_TOGGLE_WIDGET);
    UnregisterHotKey(g_wnd, HK_TOGGLE_LOCK);
    UnregisterHotKey(g_wnd, HK_REFRESH_NOW);
    if (!g_cfg.enableHotkeys) return;

    UINT mods = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
    if (!RegisterHotKey(g_wnd, HK_TOGGLE_WIDGET, mods, 'C'))
        LOGW(L"MAIN", L"Hotkey Ctrl+Shift+C unavailable (err=%lu)", (unsigned long)GetLastError());
    if (!RegisterHotKey(g_wnd, HK_TOGGLE_LOCK, mods, 'L'))
        LOGW(L"MAIN", L"Hotkey Ctrl+Shift+L unavailable (err=%lu)", (unsigned long)GetLastError());
    if (!RegisterHotKey(g_wnd, HK_REFRESH_NOW, mods, 'R'))
        LOGW(L"MAIN", L"Hotkey Ctrl+Shift+R unavailable (err=%lu)", (unsigned long)GetLastError());
}

/* ---- Autostart (HKCU\...\Run — no admin needed) ---- */

int app_autostart_is_on(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_REG_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

void app_autostart_set(int enable)
{
    if (!enable) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_REG_KEY, APP_NAME);
        return;
    }
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 8];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    _snwprintf(cmd, MAX_PATH + 8, L"\"%s\"", exe);
    cmd[MAX_PATH + 7] = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_REG_KEY, APP_NAME, REG_SZ, cmd,
                    (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t)));
}

void app_autostart_toggle(void)
{
    app_autostart_set(!app_autostart_is_on());
}

/* ---- Optional CLI self-test: TrayCoin.exe --test-api ---- */

static int run_api_self_test(void)
{
    AttachConsole(ATTACH_PARENT_PROCESS);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    http_init();
    static const struct {
        ProviderId prov;
        const wchar_t *query;
        const wchar_t *chain;
    } CASES[] = {
        { PROV_BINANCE,         L"BTC/USDT", L"" },
        { PROV_BINANCE_FUTURES, L"ETHUSDT",  L"" },
        { PROV_OKX,             L"SOL-USDT", L"" },
        { PROV_BYBIT,           L"BTCUSDT",  L"" },
        { PROV_GATE,            L"DOGE_USDT",L"" },
        { PROV_MEXC,            L"PEPEUSDT", L"" },
        { PROV_KUCOIN,          L"BTC-USDT", L"" },
        { PROV_BITGET,          L"ETHUSDT",  L"" },
        { PROV_COINBASE,        L"BTC-USD",  L"" },
        { PROV_KRAKEN,          L"XBTUSD",   L"" },
        { PROV_HTX,             L"btcusdt",  L"" },
        /* PEPE ERC-20 contract on DexScreener */
        { PROV_DEXSCREENER,     L"0x6982508145454Ce325dDbE47a25d4ec3d2311933", L"ethereum" },
        /* BONK Solana mint on DexScreener */
        { PROV_DEXSCREENER,     L"DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263", L"solana" },
        { PROV_GECKOTERMINAL,   L"0x6982508145454Ce325dDbE47a25d4ec3d2311933", L"ethereum" },
        { PROV_COINGECKO,       L"bitcoin",  L"" },
        { PROV_AUTO,            L"DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263", L"" }
    };

    int passed = 0, total = (int)(sizeof(CASES) / sizeof(CASES[0]));
    for (int i = 0; i < total; i++) {
        CoinItem c; ZeroMemory(&c, sizeof(c));
        c.provider = CASES[i].prov;
        lstrcpynW(c.query, CASES[i].query, 128);
        lstrcpynW(c.chain, CASES[i].chain, 32);
        c.inputKind = provider_detect_input(c.query);

        FetchResult r;
        provider_fetch_one(&c, "", "", &r);

        wchar_t pstr[32], pct[16] = L"";
        format_price(r.price, -1, 1, 0, pstr, 32);
        if (r.hasChange24h) format_pct(r.change24h, pct, 16);

        wchar_t line[320];
        if (r.ok) {
            passed++;
            _snwprintf(line, 320, L"[PASS] %-22s | %-12.12s -> %s %s (%s)\r\n",
                       provider_display_name(c.provider),
                       r.symbol[0] ? r.symbol : c.query, pstr, pct, r.source);
        } else {
            _snwprintf(line, 320, L"[FAIL] %-22s | %-12.12s -> %s\r\n",
                       provider_display_name(c.provider), c.query, r.error);
        }
        char utf8[640];
        wide_to_utf8(line, utf8, sizeof(utf8));
        DWORD wr = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE)
            WriteFile(hOut, utf8, (DWORD)strlen(utf8), &wr, NULL);
    }

    /* Also test Binance batching */
    CoinItem b1, b2;
    ZeroMemory(&b1, sizeof(b1)); ZeroMemory(&b2, sizeof(b2));
    b1.provider = PROV_BINANCE; lstrcpyW(b1.query, L"BTC/USDT");
    b2.provider = PROV_BINANCE; lstrcpyW(b2.query, L"ETH/USDT");
    const CoinItem *grp[2] = { &b1, &b2 };
    FetchResult bres[2];
    int bdone = provider_fetch_batch(PROV_BINANCE, grp, 2, "", "", bres);
    char sumLine[160];
    _snprintf(sumLine, sizeof(sumLine), "Batch Binance: %d/2 (ok=%d,%d) | Summary: %d/%d single passed\r\n",
              bdone, bres[0].ok, bres[1].ok, passed, total);
    DWORD wr = 0;
    if (hOut && hOut != INVALID_HANDLE_VALUE)
        WriteFile(hOut, sumLine, (DWORD)strlen(sumLine), &wr, NULL);

    http_shutdown();
    return (passed >= total - 2) ? 0 : 1;
}

/* ---- Main hidden message window ---- */

static LRESULT CALLBACK main_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PRICES_UPDATED:
        taskbar_refresh(0);
        widget_refresh(0);
        ui_settings_notify_updated();
        app_ensure_anim_timer();
        return 0;

    case WM_REFRESH_STATE:
        taskbar_refresh(0);
        widget_refresh(0);
        app_ensure_anim_timer();
        return 0;

    case WM_PRICE_ALERT:
        alerts_handle_ui_message((AlertNotification *)lp);
        return 0;

    case WM_NET_CHANGED:
        LOGD(L"NET", L"IP interface change detected (type=%lu) -> debouncing 1.2s", (unsigned long)wp);
        SetTimer(g_wnd, TIMER_NET_RECOVER, 1200, NULL);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            taskbar_refresh(0);
            widget_refresh(0);
            app_ensure_anim_timer();
        } else if (wp == TIMER_ROTATE) {
            taskbar_rotate_tick();
        } else if (wp == TIMER_NET_RECOVER) {
            KillTimer(g_wnd, TIMER_NET_RECOVER);
            if (!g_paused) {
                LOGI(L"NET", L"Network/power recovery triggered -> reconnecting WS & resetting poller backoff");
                ws_force_reconnect();
                poller_on_network_recover();
            }
        }
        return 0;

    case WM_HOTKEY:
        if (g_paused) { InterlockedExchange(&g_paused, 0); poller_wake(); ws_reload(); }
        if (wp == HK_TOGGLE_WIDGET) {
            EnterCriticalSection(&g_stateCs);
            g_cfg.showWidget = !g_cfg.showWidget;
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            LOGI(L"MAIN", L"Hotkey Ctrl+Shift+C -> showWidget=%d", g_cfg.showWidget);
            widget_refresh(1);
            ui_settings_sync_controls();
        } else if (wp == HK_TOGGLE_LOCK) {
            EnterCriticalSection(&g_stateCs);
            int next = !g_cfg.widgetClickThrough;
            g_cfg.widgetClickThrough = next;
            g_cfg.widgetLocked = next;
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            LOGI(L"MAIN", L"Hotkey Ctrl+Shift+L -> clickThrough=%d", next);
            widget_refresh(1);
            ui_settings_sync_controls();
        } else if (wp == HK_REFRESH_NOW) {
            LOGI(L"MAIN", L"Hotkey Ctrl+Shift+R -> manual refresh");
            poller_trigger_refresh(0);
        }
        return 0;

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            if (g_paused) { InterlockedExchange(&g_paused, 0); poller_wake(); ws_reload(); }
            LOGI(L"MAIN", L"Tray icon right-click");
            taskbar_show_menu();
        } else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            if (g_paused) { InterlockedExchange(&g_paused, 0); poller_wake(); ws_reload(); }
            LOGI(L"MAIN", L"Tray icon left-click");
            ui_settings_open();
        }
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        taskbar_read_theme();
        taskbar_refresh(1);
        widget_refresh(1);
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            KillTimer(g_wnd, TIMER_NET_RECOVER);
            InterlockedExchange(&g_paused, 1);
            LOGI(L"MAIN", L"System suspending -> paused=1");
            poller_wake();
            ws_reload();
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            InterlockedExchange(&g_paused, 0);
            LOGI(L"MAIN", L"System resumed -> paused=0, scheduling instant reconnect");
            SetTimer(g_wnd, TIMER_NET_RECOVER, 1200, NULL);
        } else if (wp == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *ps = (POWERBROADCAST_SETTING *)lp;
            if (IsEqualGUID(&ps->PowerSetting, &G_MONITOR_ON) && ps->DataLength >= sizeof(DWORD)) {
                DWORD state = *(DWORD *)ps->Data;
                /* Ignore initial registration broadcast (can report 0 falsely on multi-GPU/virtual/headless setups) */
                if (!g_sawDisplayEvent) {
                    g_sawDisplayEvent = 1;
                    LOGI(L"MAIN", L"Initial monitor power event ignored (state=%lu)", (unsigned long)state);
                    return TRUE;
                }
                int wantPause = (state == 0) ? 1 : 0;
                if (wantPause) {
                    /* Double-check user isn't actively using keyboard/mouse right now */
                    LASTINPUTINFO lii = { sizeof(lii), 0 };
                    if (GetLastInputInfo(&lii) && (GetTickCount() - lii.dwTime) < 5000) {
                        LOGI(L"MAIN", L"Monitor OFF event ignored (recent user input)");
                        return TRUE;
                    }
                }
                LONG wasPaused = InterlockedExchange(&g_paused, wantPause);
                LOGI(L"MAIN", L"Monitor power changed: state=%lu paused=%d", (unsigned long)state, wantPause);
                if (!wantPause && wasPaused) {
                    ws_force_reconnect();
                    poller_on_network_recover();
                } else {
                    poller_wake();
                    ws_reload();
                }
            }
        }
        return TRUE;

    case WM_DESTROY:
        KillTimer(g_wnd, TIMER_ANIM);
        KillTimer(g_wnd, TIMER_ROTATE);
        KillTimer(g_wnd, TIMER_NET_RECOVER);
        g_animTimerOn = 0;
        net_watcher_stop();
        UnregisterHotKey(g_wnd, HK_TOGGLE_WIDGET);
        UnregisterHotKey(g_wnd, HK_TOGGLE_LOCK);
        UnregisterHotKey(g_wnd, HK_REFRESH_NOW);
        ui_settings_close();
        widget_shutdown();
        taskbar_shutdown();
        PostQuitMessage(0);
        return 0;
    }

    if (msg == g_taskbarCreated && msg) {
        taskbar_recreate_all();
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_hinst = hi;

    /* Harden DLL search order */
    {
        typedef BOOL (WINAPI *SDDD)(DWORD);
        SDDD f = (SDDD)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories");
        if (f) f(LOAD_LIBRARY_SEARCH_SYSTEM32);
        SetDllDirectoryW(L"");
    }

    if (wcsstr(GetCommandLineW(), L"--test-api")) {
        return run_api_self_test();
    }
    if (wcsstr(GetCommandLineW(), L"--test-poller")) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        InitializeCriticalSection(&g_stateCs);
        http_init();
        config_load(&g_cfg);
        g_cfg.enableWebSocket = 0; /* Measure pure parallel HTTP poller */
        ULONGLONG t0 = GetTickCount64();
        poller_start();
        for (int t = 0; t < 80; t++) {
            Sleep(50);
            int pending = 0;
            EnterCriticalSection(&g_stateCs);
            for (int i = 0; i < g_cfg.coinCount; i++) {
                if (g_cfg.coins[i].enabled && !g_cfg.coins[i].ok && !g_cfg.coins[i].lastError[0])
                    pending++;
            }
            LeaveCriticalSection(&g_stateCs);
            if (pending == 0 && t > 1) break;
        }
        ULONGLONG elapsed = GetTickCount64() - t0;
        int okCnt = 0, enCnt = 0;
        EnterCriticalSection(&g_stateCs);
        for (int i = 0; i < g_cfg.coinCount; i++) {
            const CoinItem *c = &g_cfg.coins[i];
            if (!c->enabled) continue;
            enCnt++;
            wchar_t pstr[32], pct[16] = L"", line[256];
            format_price(c->price, c->decimals, 1, 0, pstr, 32);
            if (c->hasChange24h) format_pct(c->change24h, pct, 16);
            if (c->ok) {
                okCnt++;
                _snwprintf(line, 256, L"[POLLER OK]   %-10s (%s) -> %s %s\r\n",
                           c->label[0] ? c->label : c->query, c->resolvedSource, pstr, pct);
            } else {
                _snwprintf(line, 256, L"[POLLER FAIL] %-10s -> %s\r\n",
                           c->label[0] ? c->label : c->query, c->lastError[0] ? c->lastError : L"still loading");
            }
            char utf8[512]; wide_to_utf8(line, utf8, sizeof(utf8));
            DWORD wr = 0;
            if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, utf8, (DWORD)strlen(utf8), &wr, NULL);
        }
        LeaveCriticalSection(&g_stateCs);
        char sum[128];
        _snprintf(sum, sizeof(sum), "Parallel HTTP Poller: %d/%d OK in %llums\r\n", okCnt, enCnt, (unsigned long long)elapsed);
        DWORD wr = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, sum, (DWORD)strlen(sum), &wr, NULL);
        poller_stop();
        http_shutdown();
        DeleteCriticalSection(&g_stateCs);
        return (okCnt == enCnt && enCnt > 0) ? 0 : 1;
    }
    if (wcsstr(GetCommandLineW(), L"--test-ws")) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        InitializeCriticalSection(&g_stateCs);
        config_init_defaults(&g_cfg);
        g_cfg.enableWebSocket = 1;
        g_cfg.coinCount = 0;
        static const struct { ProviderId p; const wchar_t *q; const wchar_t *l; } WSC[] = {
            { PROV_BINANCE,         L"BTC/USDT",  L"BTC"  },
            { PROV_BINANCE_FUTURES, L"ETHUSDT",   L"ETH"  },
            { PROV_OKX,             L"SOL-USDT",  L"SOL"  },
            { PROV_BYBIT,           L"BTCUSDT",   L"BTCB" },
            { PROV_GATE,            L"DOGE_USDT", L"DOGE" }
        };
        for (int i = 0; i < 5; i++) {
            CoinItem *c = &g_cfg.coins[g_cfg.coinCount++];
            ZeroMemory(c, sizeof(*c));
            c->enabled = 1; c->provider = WSC[i].p; c->decimals = -1;
            lstrcpynW(c->query, WSC[i].q, 128);
            lstrcpynW(c->label, WSC[i].l, 24);
        }
        ULONGLONG t0 = GetTickCount64();
        ws_start();
        for (int t = 0; t < 70; t++) {
            Sleep(100);
            int live = 0;
            EnterCriticalSection(&g_stateCs);
            for (int i = 0; i < g_cfg.coinCount; i++) if (g_cfg.coins[i].wsLive) live++;
            LeaveCriticalSection(&g_stateCs);
            if (live == g_cfg.coinCount) break;
        }
        ULONGLONG elapsed = GetTickCount64() - t0;
        int okCnt = 0;
        EnterCriticalSection(&g_stateCs);
        for (int i = 0; i < g_cfg.coinCount; i++) {
            const CoinItem *c = &g_cfg.coins[i];
            wchar_t pstr[32], pct[16] = L"", line[256];
            format_price(c->price, c->decimals, 1, 0, pstr, 32);
            if (c->hasChange24h) format_pct(c->change24h, pct, 16);
            if (c->wsLive && c->ok) {
                okCnt++;
                _snwprintf(line, 256, L"[WS LIVE] %-16s | %-6s -> %s %s\r\n",
                           provider_display_name(c->provider), c->label, pstr, pct);
            } else {
                _snwprintf(line, 256, L"[WS MISS] %-16s | %-6s -> no tick in %llums\r\n",
                           provider_display_name(c->provider), c->label, (unsigned long long)elapsed);
            }
            char utf8[512]; wide_to_utf8(line, utf8, sizeof(utf8));
            DWORD wr = 0;
            if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, utf8, (DWORD)strlen(utf8), &wr, NULL);
        }
        LeaveCriticalSection(&g_stateCs);
        char sum[128];
        _snprintf(sum, sizeof(sum), "WebSocket Self-Test: %d/%d streams live in %llums\r\n", okCnt, g_cfg.coinCount, (unsigned long long)elapsed);
        DWORD wr = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, sum, (DWORD)strlen(sum), &wr, NULL);
        g_quit = 1;
        ws_stop();
        DeleteCriticalSection(&g_stateCs);
        return (okCnt >= 4) ? 0 : 1;
    }

    if (wcsstr(GetCommandLineW(), L"--test-refresh")) {
        /* End-to-end regression test: WS + Poller + Auto-resolve + 2 consecutive manual refreshes */
        AttachConsole(ATTACH_PARENT_PROCESS);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        InitializeCriticalSection(&g_stateCs);
        log_init();
        http_init();
        config_load(&g_cfg);
        g_cfg.enableWebSocket = 1;
        poller_start();
        ws_start();

        /* Wait for initial fetch */
        for (int t = 0; t < 60; t++) {
            Sleep(100);
            int pending = 0;
            EnterCriticalSection(&g_stateCs);
            for (int i = 0; i < g_cfg.coinCount; i++)
                if (g_cfg.coins[i].enabled && !g_cfg.coins[i].ok) pending++;
            LeaveCriticalSection(&g_stateCs);
            if (pending == 0 && t > 5) break;
        }
        ULONGLONG t1[MAX_COINS] = {0};
        EnterCriticalSection(&g_stateCs);
        for (int i = 0; i < g_cfg.coinCount; i++) t1[i] = g_cfg.coins[i].lastUpdateMs;
        LeaveCriticalSection(&g_stateCs);

        Sleep(1500);
        poller_trigger_refresh(1);
        Sleep(2000);
        ULONGLONG t2[MAX_COINS] = {0};
        EnterCriticalSection(&g_stateCs);
        for (int i = 0; i < g_cfg.coinCount; i++) t2[i] = g_cfg.coins[i].lastUpdateMs;
        LeaveCriticalSection(&g_stateCs);

        Sleep(1500);
        poller_trigger_refresh(1);
        Sleep(2000);

        int advanced = 0, enCnt = 0;
        EnterCriticalSection(&g_stateCs);
        for (int i = 0; i < g_cfg.coinCount; i++) {
            const CoinItem *c = &g_cfg.coins[i];
            if (!c->enabled) continue;
            enCnt++;
            int ok2 = (t2[i] > t1[i]);
            int ok3 = (c->lastUpdateMs > t2[i]);
            if (ok2 && ok3) advanced++;
            char buf[256];
            char lblA[32]; wide_to_utf8(c->label[0] ? c->label : c->query, lblA, sizeof(lblA));
            _snprintf(buf, sizeof(buf), "[REFRESH] %-8s ws=%d ref1=%s ref2=%s time=%02u:%02u:%02u\r\n",
                      lblA, c->wsLive, ok2 ? "OK" : "STUCK", ok3 ? "OK" : "STUCK",
                      c->lastUpdateTime.wHour, c->lastUpdateTime.wMinute, c->lastUpdateTime.wSecond);
            DWORD wr = 0;
            if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, buf, (DWORD)strlen(buf), &wr, NULL);
        }
        LeaveCriticalSection(&g_stateCs);
        char sum[128];
        _snprintf(sum, sizeof(sum), "Manual Refresh Test: %d/%d coins updated on BOTH refreshes\r\n", advanced, enCnt);
        DWORD wr = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE) WriteFile(hOut, sum, (DWORD)strlen(sum), &wr, NULL);
        g_quit = 1;
        poller_stop();
        ws_stop();
        http_shutdown();
        log_shutdown();
        DeleteCriticalSection(&g_stateCs);
        return (advanced == enCnt && enCnt > 0) ? 0 : 1;
    }

    /* Single-instance check */
    HANDLE mx = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mx) CloseHandle(mx);
        /* Bring existing instance's settings to front if launched again */
        HWND existing = FindWindowW(APP_NAME, APP_NAME);
        if (existing) PostMessageW(existing, WM_TRAY, 0, WM_LBUTTONUP);
        return 0;
    }

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_stateCs);
    log_init();
    log_install_crash_handler();
    http_init();
    config_load(&g_cfg);
    log_set_enabled(g_cfg.enableLogging);
    LOGI(L"MAIN", L"TrayCoin %s started (coins=%d, interval=%lus, ws=%d)",
         APP_VERSION, g_cfg.coinCount, (unsigned long)g_cfg.globalIntervalSec, g_cfg.enableWebSocket);
    taskbar_read_theme();

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = hi;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);

    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_NAME, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    {
        typedef BOOL (WINAPI *CWMF)(HWND, UINT, DWORD, void *);
        CWMF f = (CWMF)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
        if (f) {
            f(g_wnd, g_taskbarCreated, 1, NULL);
            f(g_wnd, WM_TRAY, 1, NULL);
        }
    }
    RegisterPowerSettingNotification(g_wnd, &G_MONITOR_ON, DEVICE_NOTIFY_WINDOW_HANDLE);
    net_watcher_start();

    taskbar_init();
    widget_init();
    app_update_hotkeys();
    SetTimer(g_wnd, TIMER_ROTATE, 1000, NULL);
    taskbar_refresh(1);
    widget_refresh(1);
    poller_start();
    ws_start();

    MSG m;
    int trimmed = 0;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!trimmed && m.message == WM_PRICES_UPDATED) {
            trimmed = 1;
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
    }

    LOGI(L"MAIN", L"Shutting down TrayCoin...");
    net_watcher_stop();
    poller_stop();
    ws_stop();
    http_shutdown();
    log_shutdown();
    DeleteCriticalSection(&g_stateCs);
    if (mx) CloseHandle(mx);
    return 0;
}
