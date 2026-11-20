/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "http.h"
#include "logger.h"
#include <winhttp.h>
#include <stdio.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

static HINTERNET g_session = NULL;
static CRITICAL_SECTION g_httpCs;
static int g_httpInited = 0;

int http_init(void)
{
    if (g_httpInited) return g_session != NULL;
    InitializeCriticalSection(&g_httpCs);
    g_httpInited = 1;

    g_session = WinHttpOpen(L"TrayCoin/2.0 (Windows NT 10.0; Win64)",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) {
        g_session = WinHttpOpen(L"TrayCoin/2.0 (Windows NT 10.0; Win64)",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!g_session) return 0;

    /* Set fast timeouts: resolve 5s, connect 6s, send 6s, receive 8s */
    WinHttpSetTimeouts(g_session, 5000, 6000, 6000, 8000);

    /* Enable automatic gzip/deflate decompression */
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    /* Enable HTTP/2 multiplexing & allow parallel keep-alive connections per server */
    DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(g_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
    DWORD maxConns = 8;
    WinHttpSetOption(g_session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxConns, sizeof(maxConns));

    /* Prefer modern TLS */
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
        protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    }
    return 1;
}

HINTERNET http_get_session(void)
{
    if (!g_session) http_init();
    return g_session;
}

void http_shutdown(void)
{
    if (!g_httpInited) return;
    EnterCriticalSection(&g_httpCs);
    if (g_session) {
        WinHttpCloseHandle(g_session);
        g_session = NULL;
    }
    LeaveCriticalSection(&g_httpCs);
    DeleteCriticalSection(&g_httpCs);
    g_httpInited = 0;
}

int http_get(const wchar_t *host, const wchar_t *path, const wchar_t *extraHeaders, HttpResponse *out)
{
    ZeroMemory(out, sizeof(*out));
    if (!g_session && !http_init()) {
        out->winError = GetLastError();
        return 0;
    }

    HINTERNET conn = NULL, req = NULL;
    char *buf = NULL;
    DWORD len = 0, cap = 0;
    int ok = 0;

    conn = WinHttpConnect(g_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { out->winError = GetLastError(); goto cleanup; }

    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { out->winError = GetLastError(); goto cleanup; }

    WinHttpAddRequestHeaders(req, L"Accept: application/json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (extraHeaders && extraHeaders[0]) {
        WinHttpAddRequestHeaders(req, extraHeaders, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        out->winError = GetLastError();
        goto cleanup;
    }

    DWORD status = 0, stSz = sizeof(status);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &stSz, WINHTTP_NO_HEADER_INDEX)) {
        out->statusCode = status;
    }

    for (;;) {
        DWORD avail = 0, rd = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { out->winError = GetLastError(); break; }
        if (!avail) { ok = 1; break; }
        if (len + avail > (4u << 20)) { /* 4 MB safety limit */
            ok = 0;
            break;
        }
        if (len + avail + 1 > cap) {
            DWORD nc = (len + avail + 1) * 2;
            if (nc < 4096) nc = 4096;
            char *nb = buf ? (char *)HeapReAlloc(GetProcessHeap(), 0, buf, nc)
                           : (char *)HeapAlloc(GetProcessHeap(), 0, nc);
            if (!nb) { ok = 0; break; }
            buf = nb;
            cap = nc;
        }
        if (!WinHttpReadData(req, buf + len, avail, &rd)) { out->winError = GetLastError(); break; }
        if (!rd) { ok = 1; break; }
        len += rd;
    }

    if (ok) {
        if (!buf) {
            buf = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1);
            len = 0;
        } else {
            buf[len] = '\0';
        }
        out->body = buf;
        out->bodyLen = len;
        buf = NULL;
    }

cleanup:
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (!ok || out->statusCode != 200) {
        LOGW(L"HTTP", L"GET https://%s%.96s -> ok=%d status=%lu winErr=%lu",
             host, path, ok, (unsigned long)out->statusCode, (unsigned long)out->winError);
    }
    return ok && out->statusCode > 0;
}

void http_free(HttpResponse *res)
{
    if (res && res->body) {
        HeapFree(GetProcessHeap(), 0, res->body);
        res->body = NULL;
        res->bodyLen = 0;
    }
}

void http_url_encode_w(const wchar_t *in, wchar_t *out, int outCap)
{
    if (!out || outCap <= 0) return;
    char utf8[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (n <= 0) { out[0] = 0; return; }
    static const wchar_t HEX[] = L"0123456789ABCDEF";
    int o = 0;
    for (int i = 0; utf8[i] && o < outCap - 4; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (wchar_t)c;
        } else {
            out[o++] = L'%';
            out[o++] = HEX[(c >> 4) & 0xF];
            out[o++] = HEX[c & 0xF];
        }
    }
    out[o] = 0;
}
