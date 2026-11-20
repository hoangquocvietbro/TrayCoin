/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "ws.h"
#include "providers.h"
#include "json.h"
#include "logger.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

typedef enum {
    WS_CH_BINANCE = 0,
    WS_CH_BINANCE_FUT,
    WS_CH_OKX,
    WS_CH_BYBIT,
    WS_CH_GATE,
    WS_CH_COUNT
} WsChanId;

typedef struct {
    int        coinIdx;
    ProviderId provider;
    wchar_t    origQuery[128];
    wchar_t    base[32];
    wchar_t    quote[16];
    char       symKey[64]; /* e.g. "BTCUSDT", "SOL-USDT", "DOGE_USDT" */
} WsSub;

typedef struct {
    WsChanId         id;
    HANDLE           thread;
    CRITICAL_SECTION sendCs;
    HINTERNET        hConn;
    HINTERNET        hWs;
    char             activeSig[2048];
    ULONGLONG        lastPingMs;
    ULONGLONG        lastRecvMs;
    int              connected;
} WsChannel;

static HINTERNET        g_wsSession = NULL;
static HANDLE           g_wsWake    = NULL;
static HANDLE           g_pingThread = NULL;
static volatile LONG    g_wsRunning = 0;
static volatile LONG    g_wsGen     = 0;
static volatile LONG    g_wsForceGen = 0;
static ULONGLONG        g_lastUiPostMs = 0;
static WsChannel        g_ch[WS_CH_COUNT];

int ws_provider_supported(ProviderId prov)
{
    return prov == PROV_BINANCE || prov == PROV_BINANCE_FUTURES ||
           prov == PROV_OKX     || prov == PROV_BYBIT || prov == PROV_GATE;
}

static ProviderId chan_to_provider(WsChanId id)
{
    switch (id) {
    case WS_CH_BINANCE:     return PROV_BINANCE;
    case WS_CH_BINANCE_FUT: return PROV_BINANCE_FUTURES;
    case WS_CH_OKX:         return PROV_OKX;
    case WS_CH_BYBIT:       return PROV_BYBIT;
    case WS_CH_GATE:        return PROV_GATE;
    default:                return PROV_BINANCE;
    }
}

static const wchar_t *chan_source_name(WsChanId id)
{
    switch (id) {
    case WS_CH_BINANCE:     return L"Binance";
    case WS_CH_BINANCE_FUT: return L"Binance Perp";
    case WS_CH_OKX:         return L"OKX";
    case WS_CH_BYBIT:       return L"Bybit";
    case WS_CH_GATE:        return L"Gate.io";
    default:                return L"WS";
    }
}

static void str_lower_a(char *s)
{
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* Collect enabled coins matching this WebSocket channel */
static int collect_subs(WsChanId id, WsSub *subs, int maxSubs, char *outSig, int sigCap)
{
    ProviderId target = chan_to_provider(id);
    int n = 0;
    outSig[0] = '\0';

    EnterCriticalSection(&g_stateCs);
    int enabled = g_cfg.enableWebSocket && !g_paused;
    if (enabled) {
        for (int i = 0; i < g_cfg.coinCount && n < maxSubs; i++) {
            const CoinItem *c = &g_cfg.coins[i];
            if (!c->enabled) continue;

            int match = (c->provider == target);
            if (!match && c->provider == PROV_AUTO && c->resolvedSource[0]) {
                if (id == WS_CH_BINANCE && lstrcmpW(c->resolvedSource, L"Binance") == 0) match = 1;
                else if (id == WS_CH_OKX && lstrcmpW(c->resolvedSource, L"OKX") == 0) match = 1;
                else if (id == WS_CH_BYBIT && lstrcmpW(c->resolvedSource, L"Bybit") == 0) match = 1;
                else if (id == WS_CH_GATE && lstrcmpW(c->resolvedSource, L"Gate.io") == 0) match = 1;
            }
            if (!match) continue;
            if (provider_detect_input(c->query) != INPUT_PAIR) continue;

            WsSub *s = &subs[n];
            ZeroMemory(s, sizeof(*s));
            s->coinIdx = i;
            s->provider = c->provider;
            lstrcpynW(s->origQuery, c->query, 128);
            provider_split_pair(c->query, s->base, 32, s->quote, 16);

            char bA[32], qA[16];
            wide_to_utf8(s->base, bA, sizeof(bA));
            wide_to_utf8(s->quote, qA, sizeof(qA));
            if (id == WS_CH_OKX)
                _snprintf(s->symKey, sizeof(s->symKey), "%s-%s", bA, qA);
            else if (id == WS_CH_GATE)
                _snprintf(s->symKey, sizeof(s->symKey), "%s_%s", bA, qA);
            else
                _snprintf(s->symKey, sizeof(s->symKey), "%s%s", bA, qA);
            s->symKey[sizeof(s->symKey) - 1] = '\0';

            if ((int)strlen(outSig) + (int)strlen(s->symKey) + 4 < sigCap) {
                if (n > 0) strcat(outSig, ",");
                strcat(outSig, s->symKey);
            }
            n++;
        }
    }
    LeaveCriticalSection(&g_stateCs);
    return n;
}

static void ws_mark_channel_offline(WsChanId id)
{
    ProviderId target = chan_to_provider(id);
    EnterCriticalSection(&g_stateCs);
    for (int i = 0; i < g_cfg.coinCount; i++) {
        CoinItem *c = &g_cfg.coins[i];
        if (c->provider == target || (c->provider == PROV_AUTO && lstrcmpW(c->resolvedSource, chan_source_name(id)) == 0)) {
            c->wsLive = 0;
        }
    }
    LeaveCriticalSection(&g_stateCs);
}

static void ws_apply_tick(WsChanId id, const WsSub *subs, int subCount,
                          const char *symKey, double price, int hasPct, double pct)
{
    if (!symKey || !symKey[0] || price <= 0.0) return;
    ULONGLONG now = GetTickCount64();
    int updated = 0;

    EnterCriticalSection(&g_stateCs);
    for (int s = 0; s < subCount; s++) {
        if (_stricmp(subs[s].symKey, symKey) != 0) continue;
        int idx = subs[s].coinIdx;
        if (idx < 0 || idx >= g_cfg.coinCount) continue;
        CoinItem *c = &g_cfg.coins[idx];
        if (!c->enabled || c->provider != subs[s].provider || lstrcmpW(c->query, subs[s].origQuery) != 0)
            continue;

        coin_record_tick_locked(c, price, now);
        c->prevPrice = c->ok ? c->price : price;
        c->price     = price;
        if (hasPct) {
            c->change24h    = pct;
            c->hasChange24h = 1;
        }
        c->ok           = 1;
        c->fetching     = 0;
        c->failCount    = 0;
        c->wsLive       = 1;
        c->wsLastTickMs = now;
        c->lastUpdateMs = now;
        GetLocalTime(&c->lastUpdateTime);
        lstrcpynW(c->resolvedSource, chan_source_name(id), 32);
        lstrcpynW(c->resolvedSymbol, subs[s].base, 24);
        c->lastError[0] = 0;

        if (!c->pairUrl[0]) {
            if (id == WS_CH_BINANCE)
                _snwprintf(c->pairUrl, 256, L"https://www.binance.com/en/trade/%s_%s", subs[s].base, subs[s].quote);
            else if (id == WS_CH_BINANCE_FUT)
                _snwprintf(c->pairUrl, 256, L"https://www.binance.com/en/futures/%s%s", subs[s].base, subs[s].quote);
            else if (id == WS_CH_OKX) {
                wchar_t bl[32], ql[16];
                lstrcpynW(bl, subs[s].base, 32); lstrcpynW(ql, subs[s].quote, 16);
                for (wchar_t *p = bl; *p; p++) *p = towlower(*p);
                for (wchar_t *p = ql; *p; p++) *p = towlower(*p);
                _snwprintf(c->pairUrl, 256, L"https://www.okx.com/trade-spot/%s-%s", bl, ql);
            } else if (id == WS_CH_BYBIT)
                _snwprintf(c->pairUrl, 256, L"https://www.bybit.com/trade/spot/%s/%s", subs[s].base, subs[s].quote);
            else if (id == WS_CH_GATE)
                _snwprintf(c->pairUrl, 256, L"https://www.gate.io/trade/%s_%s", subs[s].base, subs[s].quote);
        }
        updated = 1;
    }

    int postUi = 0;
    if (updated && g_wnd && (now - g_lastUiPostMs >= 250ULL)) {
        g_lastUiPostMs = now;
        postUi = 1;
    }
    LeaveCriticalSection(&g_stateCs);

    if (postUi) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
}

static void ws_parse_message(WsChanId id, const WsSub *subs, int subCount, const char *msg)
{
    if (!msg || msg[0] != '{') return; /* ignore plain "pong" */
    JsonVal *j = json_parse(msg);
    if (!j) return;

    if (id == WS_CH_BINANCE || id == WS_CH_BINANCE_FUT) {
        const JsonVal *d = json_get(j, "data");
        if (!d) d = j;
        const char *sym = json_as_str(json_get(d, "s"), "");
        double closeP = 0, openP = 0;
        if (sym[0] && json_as_double(json_get(d, "c"), &closeP) && closeP > 0) {
            int hasPct = 0; double pct = 0;
            if (json_as_double(json_get(d, "o"), &openP) && openP > 0) {
                pct = ((closeP - openP) / openP) * 100.0;
                hasPct = 1;
            }
            ws_apply_tick(id, subs, subCount, sym, closeP, hasPct, pct);
        }
    } else if (id == WS_CH_OKX) {
        const JsonVal *d0 = json_at(json_get(j, "data"), 0);
        if (d0) {
            const char *inst = json_as_str(json_get(d0, "instId"), "");
            double last = 0, open = 0;
            if (inst[0] && json_as_double(json_get(d0, "last"), &last) && last > 0) {
                int hasPct = 0; double pct = 0;
                if (json_as_double(json_get(d0, "open24h"), &open) && open > 0) {
                    pct = ((last - open) / open) * 100.0;
                    hasPct = 1;
                }
                ws_apply_tick(id, subs, subCount, inst, last, hasPct, pct);
            }
        }
    } else if (id == WS_CH_BYBIT) {
        const JsonVal *d = json_get(j, "data");
        if (d) {
            const char *sym = json_as_str(json_get(d, "symbol"), "");
            double last = 0, pcnt = 0;
            if (sym[0] && json_as_double(json_get(d, "lastPrice"), &last) && last > 0) {
                int hasPct = json_as_double(json_get(d, "price24hPcnt"), &pcnt);
                ws_apply_tick(id, subs, subCount, sym, last, hasPct, pcnt * 100.0);
            }
        }
    } else if (id == WS_CH_GATE) {
        const char *ev = json_as_str(json_get(j, "event"), "");
        if (strcmp(ev, "update") == 0) {
            const JsonVal *r = json_get(j, "result");
            if (r) {
                const char *cp = json_as_str(json_get(r, "currency_pair"), "");
                double last = 0, pct = 0;
                if (cp[0] && json_as_double(json_get(r, "last"), &last) && last > 0) {
                    int hasPct = json_as_double(json_get(r, "change_percentage"), &pct);
                    ws_apply_tick(id, subs, subCount, cp, last, hasPct, pct);
                }
            }
        }
    }

    json_free(j);
}

static int ws_send_text(WsChannel *ch, const char *text)
{
    int ok = 0;
    EnterCriticalSection(&ch->sendCs);
    if (ch->hWs) {
        DWORD err = WinHttpWebSocketSend(ch->hWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (PVOID)text, (DWORD)strlen(text));
        ok = (err == NO_ERROR);
    }
    LeaveCriticalSection(&ch->sendCs);
    return ok;
}

static HINTERNET ws_connect_once(const wchar_t *host, INTERNET_PORT port, const wchar_t *path, HINTERNET *outConn)
{
    *outConn = NULL;
    if (!g_wsSession) return NULL;

    HINTERNET conn = WinHttpConnect(g_wsSession, host, port, 0);
    if (!conn) return NULL;

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); return NULL; }

    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return NULL;
    }

    HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!hWs) {
        WinHttpCloseHandle(conn);
        return NULL;
    }
    *outConn = conn;
    return hWs;
}

static void ws_close_channel_handle(WsChannel *ch)
{
    EnterCriticalSection(&ch->sendCs);
    HINTERNET hWs = ch->hWs;
    HINTERNET hConn = ch->hConn;
    ch->hWs = NULL;
    ch->hConn = NULL;
    ch->connected = 0;
    LeaveCriticalSection(&ch->sendCs);

    /* Never call blocking WinHttpWebSocketClose() while Receive() is active on another thread;
       closing the handles aborts Receive() immediately without deadlocking! */
    if (hWs)   WinHttpCloseHandle(hWs);
    if (hConn) WinHttpCloseHandle(hConn);
}

static DWORD WINAPI ws_channel_proc(void *arg)
{
    WsChannel *ch = (WsChannel *)arg;
    WsChanId id = ch->id;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    DWORD backoffMs = 2000;
    char *msgBuf = (char *)HeapAlloc(GetProcessHeap(), 0, 16384);
    if (!msgBuf) return 0;

    while (!g_quit && g_wsRunning) {
        LONG myForceGen = g_wsForceGen;
        WsSub subs[MAX_COINS];
        char sig[2048];
        int subCount = collect_subs(id, subs, MAX_COINS, sig, sizeof(sig));
        if (subCount == 0) {
            ws_mark_channel_offline(id);
            WaitForSingleObject(g_wsWake, 2000);
            backoffMs = 2000;
            continue;
        }

        /* Build endpoint & optional subscribe JSON */
        const wchar_t *host = L"";
        INTERNET_PORT port = 443;
        wchar_t path[2560] = L"";
        char subPayload[4096] = "";

        if (id == WS_CH_BINANCE || id == WS_CH_BINANCE_FUT) {
            host = (id == WS_CH_BINANCE) ? L"stream.binance.com" : L"fstream.binance.com";
            port = (id == WS_CH_BINANCE) ? 9443 : 443;
            lstrcpyW(path, (id == WS_CH_BINANCE) ? L"/stream?streams=" : L"/market/stream?streams=");
            int pos = lstrlenW(path);
            for (int i = 0; i < subCount; i++) {
                char low[64]; lstrcpynA(low, subs[i].symKey, sizeof(low)); str_lower_a(low);
                wchar_t wlow[64]; utf8_to_wide(low, wlow, 64);
                int n = _snwprintf(path + pos, 2500 - pos, L"%s%s@miniTicker", i ? L"/" : L"", wlow);
                if (n > 0) pos += n;
            }
        } else if (id == WS_CH_OKX) {
            host = L"ws.okx.com"; port = 8443; lstrcpyW(path, L"/ws/v5/public");
            strcpy(subPayload, "{\"op\":\"subscribe\",\"args\":[");
            for (int i = 0; i < subCount; i++) {
                char item[128];
                _snprintf(item, sizeof(item), "%s{\"channel\":\"tickers\",\"instId\":\"%s\"}", i ? "," : "", subs[i].symKey);
                if (strlen(subPayload) + strlen(item) + 8 < sizeof(subPayload)) strcat(subPayload, item);
            }
            strcat(subPayload, "]}");
        } else if (id == WS_CH_BYBIT) {
            host = L"stream.bybit.com"; port = 443; lstrcpyW(path, L"/v5/public/spot");
            strcpy(subPayload, "{\"op\":\"subscribe\",\"args\":[");
            for (int i = 0; i < subCount; i++) {
                char item[96];
                _snprintf(item, sizeof(item), "%s\"tickers.%s\"", i ? "," : "", subs[i].symKey);
                if (strlen(subPayload) + strlen(item) + 8 < sizeof(subPayload)) strcat(subPayload, item);
            }
            strcat(subPayload, "]}");
        } else if (id == WS_CH_GATE) {
            host = L"api.gateio.ws"; port = 443; lstrcpyW(path, L"/ws/v4/");
            strcpy(subPayload, "{\"time\":1700000000,\"channel\":\"spot.tickers\",\"event\":\"subscribe\",\"payload\":[");
            for (int i = 0; i < subCount; i++) {
                char item[96];
                _snprintf(item, sizeof(item), "%s\"%s\"", i ? "," : "", subs[i].symKey);
                if (strlen(subPayload) + strlen(item) + 8 < sizeof(subPayload)) strcat(subPayload, item);
            }
            strcat(subPayload, "]}");
        }

        HINTERNET hConn = NULL;
        HINTERNET hWs = ws_connect_once(host, port, path, &hConn);
        if (!hWs && id == WS_CH_BINANCE && port == 9443) {
            /* Fallback to port 443 if 9443 is blocked */
            hWs = ws_connect_once(host, 443, path, &hConn);
        }

        if (!hWs) {
            LOGW(L"WS", L"%s connect failed (host=%s:%u, backoff=%lums, err=%lu)",
                 chan_source_name(id), host, (unsigned)port, (unsigned long)backoffMs, (unsigned long)GetLastError());
            ws_mark_channel_offline(id);
            WaitForSingleObject(g_wsWake, backoffMs);
            if (g_wsForceGen != myForceGen) {
                backoffMs = 1500;
            } else {
                backoffMs = (backoffMs < 30000) ? (backoffMs * 2) : 30000;
            }
            continue;
        }

        LOGI(L"WS", L"%s connected (%d sub(s))", chan_source_name(id), subCount);
        ULONGLONG connNow = GetTickCount64();
        EnterCriticalSection(&ch->sendCs);
        ch->hConn = hConn;
        ch->hWs = hWs;
        ch->connected = 1;
        ch->lastPingMs = connNow;
        ch->lastRecvMs = connNow;
        lstrcpynA(ch->activeSig, sig, sizeof(ch->activeSig));
        LeaveCriticalSection(&ch->sendCs);

        if (subPayload[0]) {
            ws_send_text(ch, subPayload);
        }

        backoffMs = 1500;
        DWORD msgLen = 0;
        int gotAnyTick = 0;

        while (!g_quit && g_wsRunning) {
            BYTE tmp[4096];
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE btype = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            DWORD err = WinHttpWebSocketReceive(hWs, tmp, sizeof(tmp), &read, &btype);
            if (err != NO_ERROR || btype == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                LOGD(L"WS", L"%s stream ended (err=%lu btype=%d)", chan_source_name(id), (unsigned long)err, (int)btype);
                break;
            }
            ch->lastRecvMs = GetTickCount64();
            if (btype == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                if (msgLen + read < 16380) {
                    memcpy(msgBuf + msgLen, tmp, read);
                    msgLen += read;
                } else {
                    msgLen = 0; /* Drop oversized frame cleanly */
                }
                if (btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                    msgBuf[msgLen] = '\0';
                    ws_parse_message(id, subs, subCount, msgBuf);
                    msgLen = 0;
                    gotAnyTick = 1;
                }
            }
        }

        ws_close_channel_handle(ch);
        ws_mark_channel_offline(id);

        if (!g_quit && g_wsRunning) {
            /* If we closed due to reload or network recovery, reconnect immediately; else brief backoff */
            char newSig[2048];
            WsSub tmpSubs[MAX_COINS];
            collect_subs(id, tmpSubs, MAX_COINS, newSig, sizeof(newSig));
            if (g_wsForceGen != myForceGen || strcmp(newSig, sig) != 0) {
                backoffMs = 1500;
                Sleep(100);
            } else {
                WaitForSingleObject(g_wsWake, gotAnyTick ? 1500 : backoffMs);
                if (g_wsForceGen != myForceGen) {
                    backoffMs = 1500;
                } else if (!gotAnyTick) {
                    backoffMs = (backoffMs < 30000) ? (backoffMs * 2) : 30000;
                }
            }
        }
    }

    ws_close_channel_handle(ch);
    HeapFree(GetProcessHeap(), 0, msgBuf);
    return 0;
}

static DWORD WINAPI ws_ping_proc(void *unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LONG seenGen = g_wsGen;
    LONG seenForceGen = g_wsForceGen;

    while (!g_quit && g_wsRunning) {
        WaitForSingleObject(g_wsWake, 5000);
        if (g_quit || !g_wsRunning) break;

        LONG curGen = g_wsGen;
        LONG curForceGen = g_wsForceGen;
        int forceAll = 0;
        if (curForceGen != seenForceGen) {
            seenForceGen = curForceGen;
            forceAll = 1;
        }
        if (curGen != seenGen || forceAll) {
            seenGen = curGen;
            /* Let sleeping ws_channel_proc threads wake up before resetting */
            Sleep(25);
            if (g_wsRunning && !g_quit) ResetEvent(g_wsWake);
        }

        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < WS_CH_COUNT; i++) {
            WsChannel *ch = &g_ch[i];
            if (!ch->connected) continue;

            if (forceAll) {
                LOGI(L"WS", L"%s force-reconnecting (network/power event)", chan_source_name((WsChanId)i));
                ws_close_channel_handle(ch);
                continue;
            }

            /* Zombie / half-open socket watchdog: 45s with zero frames -> abort & reconnect */
            if (ch->lastRecvMs > 0 && now >= ch->lastRecvMs && (now - ch->lastRecvMs) >= 45000ULL) {
                LOGW(L"WS", L"%s silent for 45s (zombie socket) -> reconnecting", chan_source_name((WsChanId)i));
                ws_close_channel_handle(ch);
                continue;
            }

            /* Check if subscriptions changed or paused/disabled -> abort handle cleanly on background thread */
            WsSub tmp[MAX_COINS];
            char wantSig[2048];
            collect_subs((WsChanId)i, tmp, MAX_COINS, wantSig, sizeof(wantSig));
            int needClose = 0;
            EnterCriticalSection(&ch->sendCs);
            if (ch->connected && strcmp(ch->activeSig, wantSig) != 0) needClose = 1;
            LeaveCriticalSection(&ch->sendCs);
            if (needClose) {
                LOGI(L"WS", L"%s subscription changed -> reconnecting", chan_source_name((WsChanId)i));
                ws_close_channel_handle(ch);
                continue;
            }

            /* Send periodic keepalive ping for exchanges requiring client pings */
            if (now - ch->lastPingMs >= 18000ULL) {
                int pingOk = 1;
                if (i == WS_CH_OKX) {
                    pingOk = ws_send_text(ch, "ping");
                    ch->lastPingMs = now;
                } else if (i == WS_CH_BYBIT) {
                    pingOk = ws_send_text(ch, "{\"op\":\"ping\"}");
                    ch->lastPingMs = now;
                } else if (i == WS_CH_GATE) {
                    pingOk = ws_send_text(ch, "{\"time\":1700000000,\"channel\":\"spot.ping\"}");
                    ch->lastPingMs = now;
                }
                if (!pingOk) {
                    LOGW(L"WS", L"%s keepalive ping failed -> reconnecting", chan_source_name((WsChanId)i));
                    ws_close_channel_handle(ch);
                }
            }
        }
    }
    return 0;
}

void ws_start(void)
{
    if (g_wsRunning) return;
    g_wsRunning = 1;

    /* Dedicated HTTP/1.1 session for RFC 6455 WebSocket upgrade */
    g_wsSession = WinHttpOpen(L"TrayCoin-WS/2.0 (Windows NT 10.0; Win64)",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_wsSession) {
        g_wsSession = WinHttpOpen(L"TrayCoin-WS/2.0 (Windows NT 10.0; Win64)",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (g_wsSession) {
        WinHttpSetTimeouts(g_wsSession, 5000, 6000, 6000, 0);
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(g_wsSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
            protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(g_wsSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
        }
    }

    g_wsWake = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < WS_CH_COUNT; i++) {
        ZeroMemory(&g_ch[i], sizeof(g_ch[i]));
        g_ch[i].id = (WsChanId)i;
        InitializeCriticalSection(&g_ch[i].sendCs);
        g_ch[i].thread = CreateThread(NULL, 0, ws_channel_proc, &g_ch[i], 0, NULL);
    }
    g_pingThread = CreateThread(NULL, 0, ws_ping_proc, NULL, 0, NULL);
}

void ws_reload(void)
{
    if (!g_wsRunning) return;
    LOGD(L"WS", L"ws_reload requested (non-blocking)");
    /* Completely non-blocking: wakes ws_ping_proc and sleeping channel threads */
    InterlockedIncrement(&g_wsGen);
    if (g_wsWake) SetEvent(g_wsWake);
}

void ws_force_reconnect(void)
{
    if (!g_wsRunning) return;
    LOGI(L"WS", L"ws_force_reconnect requested (resetting backoff & dropping stale sockets)");
    InterlockedIncrement(&g_wsForceGen);
    InterlockedIncrement(&g_wsGen);
    if (g_wsWake) SetEvent(g_wsWake);
}

void ws_stop(void)
{
    if (!g_wsRunning) return;
    g_wsRunning = 0;
    if (g_wsWake) SetEvent(g_wsWake);

    /* Abort active WS handles so blocking WinHttpWebSocketReceive returns immediately */
    for (int i = 0; i < WS_CH_COUNT; i++) ws_close_channel_handle(&g_ch[i]);

    if (g_pingThread) {
        WaitForSingleObject(g_pingThread, 2000);
        CloseHandle(g_pingThread);
        g_pingThread = NULL;
    }
    for (int i = 0; i < WS_CH_COUNT; i++) {
        if (g_ch[i].thread) {
            WaitForSingleObject(g_ch[i].thread, 2500);
            CloseHandle(g_ch[i].thread);
            g_ch[i].thread = NULL;
        }
        DeleteCriticalSection(&g_ch[i].sendCs);
    }
    if (g_wsWake) { CloseHandle(g_wsWake); g_wsWake = NULL; }
    if (g_wsSession) { WinHttpCloseHandle(g_wsSession); g_wsSession = NULL; }
}
