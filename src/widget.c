/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "widget.h"
#include "config.h"
#include "providers.h"
#include "poller.h"
#include "taskbar.h"
#include "ui_settings.h"
#include "logger.h"
#include <shellapi.h>
#include <stdio.h>
#include <math.h>

#define WSS 2
#define W_MAX_TEXTS 512
#define W_MAX_HITS (MAX_COINS + 16)

typedef enum {
    WHIT_NONE = 0,
    WHIT_REFRESH,
    WHIT_LAYOUT,
    WHIT_LOCK,
    WHIT_SETTINGS,
    WHIT_CLOSE,
    WHIT_COIN
} WHitKind;

typedef struct {
    WHitKind kind;
    int      coinIdx;
    RECT     rc; /* 1x client coordinates */
} WHitBox;

typedef struct {
    int      x, y, w, h; /* 2x supersampled coords */
    int      fontKind;   /* 0=normal, 1=bold, 2=small, 3=icon */
    int      alignRight; /* 1=right align within [x, x+w] */
    COLORREF c;
    BYTE     al;
    wchar_t  t[48];
} WText;

static HWND     g_wWnd = NULL;
static int      g_wDpi = 96;
static int      g_wW = 0, g_wH = 0;
static HFONT    g_wFont = NULL, g_wBold = NULL, g_wSmall = NULL, g_wIco = NULL;
static int      g_wFontDpi = 0;
static int      g_wIcoOK = 0;
static WHitBox  g_wHits[W_MAX_HITS];
static int      g_wHitCount = 0;
static WHitKind g_wHoverKind = WHIT_NONE;
static int      g_wHoverCoin = -1;
static int      g_wTracking = 0;
static int      g_wScroll = 0;
static int      g_wDragging = 0;
static int      g_wDragMoved = 0;
static POINT    g_wDragStartScreen = {0, 0};
static POINT    g_wDragStartWin = {0, 0};
static wchar_t  g_wSig[4096] = {0};

static int WSC(int v) { return MulDiv(v, g_wDpi, 96); }

static void widget_get_dpi(void)
{
    typedef UINT (WINAPI *GDW)(HWND);
    static GDW fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        fn = (GDW)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    UINT d = (fn && g_wWnd) ? fn(g_wWnd) : 0;
    if (!d) { HDC dc = GetDC(NULL); d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    g_wDpi = d < 96 ? 96 : (int)d;
}

static HFONT w_mkfont(HDC dc, const wchar_t *face, int px, int weight)
{
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
    wchar_t got[LF_FACESIZE] = {0};
    HGDIOBJ o = SelectObject(dc, f);
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    if (lstrcmpiW(got, face)) { DeleteObject(f); return NULL; }
    return f;
}

static void widget_fonts(HDC dc)
{
    if (g_wFont && g_wFontDpi == g_wDpi) return;
    if (g_wFont)  DeleteObject(g_wFont);
    if (g_wBold)  DeleteObject(g_wBold);
    if (g_wSmall) DeleteObject(g_wSmall);
    if (g_wIco)   DeleteObject(g_wIco);

    int tp = WSC(13) * WSS, sp = WSC(11) * WSS, ip = WSC(12) * WSS;
    g_wFont = w_mkfont(dc, L"Segoe UI Variable Text", tp, FW_NORMAL);
    if (!g_wFont) g_wFont = w_mkfont(dc, L"Segoe UI", tp, FW_NORMAL);
    if (!g_wFont) g_wFont = CreateFontW(-tp, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    g_wBold = w_mkfont(dc, L"Segoe UI Variable Text", tp, FW_SEMIBOLD);
    if (!g_wBold) g_wBold = w_mkfont(dc, L"Segoe UI", tp, FW_SEMIBOLD);
    if (!g_wBold) g_wBold = CreateFontW(-tp, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    g_wSmall = w_mkfont(dc, L"Segoe UI Variable Text", sp, FW_NORMAL);
    if (!g_wSmall) g_wSmall = w_mkfont(dc, L"Segoe UI", sp, FW_NORMAL);
    if (!g_wSmall) g_wSmall = CreateFontW(-sp, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    g_wIco = w_mkfont(dc, L"Segoe Fluent Icons", ip, FW_NORMAL);
    if (!g_wIco) g_wIco = w_mkfont(dc, L"Segoe MDL2 Assets", ip, FW_NORMAL);

    g_wIcoOK = 0;
    if (g_wIco) {
        wchar_t ch = 0xE72C;
        WORD gi = 0xFFFF;
        HGDIOBJ o = SelectObject(dc, g_wIco);
        if (GetGlyphIndicesW(dc, &ch, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi != 0xFFFF)
            g_wIcoOK = 1;
        SelectObject(dc, o);
    }
    g_wFontDpi = g_wDpi;
}

static void widget_get_work_area(const AppConfig *snap, RECT *wa)
{
    HMONITOR hm = NULL;
    if (snap && snap->widgetX != WIDGET_POS_UNSET && snap->widgetY != WIDGET_POS_UNSET) {
        POINT pt = { snap->widgetX + (g_wW > 0 ? g_wW / 2 : 100),
                     snap->widgetY + (g_wH > 0 ? g_wH / 2 : 40) };
        hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else if (g_wWnd && IsWindow(g_wWnd)) {
        hm = MonitorFromWindow(g_wWnd, MONITOR_DEFAULTTONEAREST);
    }
    if (hm) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi)) { *wa = mi.rcWork; return; }
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, wa, 0);
}

static void widget_clamp_pos(LONG *ioX, LONG *ioY, int W, int H)
{
    RECT wa;
    if (*ioX == WIDGET_POS_UNSET || *ioY == WIDGET_POS_UNSET) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        *ioX = wa.right - W - WSC(18);
        *ioY = wa.top + WSC(18);
        if (*ioX < wa.left + WSC(8)) *ioX = wa.left + WSC(8);
        if (*ioY < wa.top + WSC(8))  *ioY = wa.top + WSC(8);
        return;
    }
    POINT pt = { *ioX + W / 2, *ioY + H / 2 };
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hm, &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    if (W <= wa.right - wa.left) {
        if (*ioX < wa.left) *ioX = wa.left;
        if (*ioX + W > wa.right) *ioX = wa.right - W;
    } else {
        *ioX = wa.left;
    }
    if (H <= wa.bottom - wa.top) {
        if (*ioY < wa.top) *ioY = wa.top;
        if (*ioY + H > wa.bottom) *ioY = wa.bottom - H;
    } else {
        *ioY = wa.top;
    }
}

static void widget_apply_styles(const AppConfig *snap)
{
    if (!g_wWnd) return;
    LONG_PTR ex = GetWindowLongPtrW(g_wWnd, GWL_EXSTYLE);
    LONG_PTR want = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (snap->widgetTopmost)      want |= WS_EX_TOPMOST;
    if (snap->widgetClickThrough) want |= WS_EX_TRANSPARENT;
    if (ex != want) {
        SetWindowLongPtrW(g_wWnd, GWL_EXSTYLE, want);
    }
    HWND zOrder = snap->widgetTopmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(g_wWnd, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* Blend premultiplied ARGB foreground over premultiplied ARGB background */
static DWORD blend_over(DWORD bg, DWORD r, DWORD g, DWORD b, DWORD a)
{
    if (a == 0) return bg;
    DWORD bgA = (bg >> 24) & 0xFF;
    DWORD bgR = (bg >> 16) & 0xFF;
    DWORD bgG = (bg >> 8)  & 0xFF;
    DWORD bgB =  bg        & 0xFF;

    DWORD fgR = (r * a) / 255;
    DWORD fgG = (g * a) / 255;
    DWORD fgB = (b * a) / 255;
    DWORD inv = 255 - a;

    DWORD outA = a + (bgA * inv) / 255;
    DWORD outR = fgR + (bgR * inv) / 255;
    DWORD outG = fgG + (bgG * inv) / 255;
    DWORD outB = fgB + (bgB * inv) / 255;
    if (outA > 255) outA = 255;
    if (outR > outA) outR = outA;
    if (outG > outA) outG = outA;
    if (outB > outA) outB = outA;
    return (outA << 24) | (outR << 16) | (outG << 8) | outB;
}

typedef struct {
    int      x, y, w, h; /* 1x client coords */
    int      coinIdx;
} WSpark;

static COLORREF w_lerp_color(COLORREF a, COLORREF b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int r  = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f);
    int g  = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f);
    return RGB(r, g, bl);
}

/* Signed distance to rounded rectangle centered at (cx, cy) with half-sizes (hx, hy) and corner radius r */
static float sdf_roundrect(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float dx = fabsf(px - cx) - (hx - r);
    float dy = fabsf(py - cy) - (hy - r);
    float ox = dx > 0.0f ? dx : 0.0f;
    float oy = dy > 0.0f ? dy : 0.0f;
    float inner = (dx > dy ? dx : dy);
    if (inner > 0.0f) inner = 0.0f;
    return sqrtf(ox * ox + oy * oy) + inner - r;
}

static void draw_aa_line(DWORD *px, int W, int H, float x0, float y0, float x1, float y1, COLORREF col, float thickness, BYTE alpha)
{
    int minX = (int)floorf((x0 < x1 ? x0 : x1) - thickness - 1.5f);
    int maxX = (int)ceilf ((x0 > x1 ? x0 : x1) + thickness + 1.5f);
    int minY = (int)floorf((y0 < y1 ? y0 : y1) - thickness - 1.5f);
    int maxY = (int)ceilf ((y0 > y1 ? y0 : y1) + thickness + 1.5f);
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX >= W) maxX = W - 1;
    if (maxY >= H) maxY = H - 1;

    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    DWORD r = GetRValue(col), g = GetGValue(col), b = GetBValue(col);
    float halfW = thickness * 0.5f;

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            float t = len2 > 1e-6f ? (((x - x0) * dx + (y - y0) * dy) / len2) : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float cx = x0 + t * dx, cy = y0 + t * dy;
            float dist = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - halfW;
            if (dist < 0.85f) {
                float cov = 0.85f - dist;
                if (cov > 1.0f) cov = 1.0f;
                if (cov > 0.0f) {
                    DWORD a = (DWORD)(alpha * cov + 0.5f);
                    if (a > 0) px[y * W + x] = blend_over(px[y * W + x], r, g, b, a);
                }
            }
        }
    }
}

static void draw_sparkline(DWORD *px, int W, int H, const WSpark *sp, const CoinItem *c,
                           COLORREF upCol, COLORREF downCol, COLORREF mutedCol)
{
    if (!c || c->histCount < 2) {
        /* Subtle flat baseline until 2+ ticks accumulate */
        float ym = sp->y + sp->h * 0.5f;
        draw_aa_line(px, W, H, (float)sp->x, ym, (float)(sp->x + sp->w), ym, mutedCol, 1.0f, 80);
        return;
    }
    double pts[COIN_HIST_LEN];
    int n = c->histCount;
    int start = (c->histHead - n + COIN_HIST_LEN) % COIN_HIST_LEN;
    double mn = 1e300, mx = -1e300;
    for (int i = 0; i < n; i++) {
        double v = c->history[(start + i) % COIN_HIST_LEN];
        pts[i] = v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    double span = mx - mn;
    COLORREF col = (pts[n - 1] >= pts[0]) ? upCol : downCol;
    float stepX = (float)sp->w / (float)(n - 1);
    float lastX = (float)sp->x, lastY = sp->y + sp->h * 0.5f;

    for (int i = 0; i < n; i++) {
        float norm = (span > 1e-15) ? (float)((pts[i] - mn) / span) : 0.5f;
        float x = sp->x + i * stepX;
        float y = sp->y + (1.0f - norm) * (float)sp->h;
        if (i > 0) draw_aa_line(px, W, H, lastX, lastY, x, y, col, 1.35f, 225);
        lastX = x; lastY = y;
    }
    /* Crisp live tip dot */
    draw_aa_line(px, W, H, lastX, lastY, lastX, lastY, col, 3.0f, 245);
}

static void add_hit(WHitKind kind, int coinIdx, int l, int t, int r, int b)
{
    if (g_wHitCount >= W_MAX_HITS) return;
    g_wHits[g_wHitCount].kind = kind;
    g_wHits[g_wHitCount].coinIdx = coinIdx;
    g_wHits[g_wHitCount].rc.left = l;
    g_wHits[g_wHitCount].rc.top = t;
    g_wHits[g_wHitCount].rc.right = r;
    g_wHits[g_wHitCount].rc.bottom = b;
    g_wHitCount++;
}

static void add_text(WText *arr, int *cnt, int maxCnt, int x, int y, int w, int h,
                     int fontKind, int alignRight, COLORREF c, BYTE al, const wchar_t *txt)
{
    if (*cnt >= maxCnt) return;
    WText *wt = &arr[(*cnt)++];
    wt->x = x * WSS;
    wt->y = y * WSS;
    wt->w = w * WSS;
    wt->h = h * WSS;
    wt->fontKind = fontKind;
    wt->alignRight = alignRight;
    wt->c = c;
    wt->al = al;
    lstrcpynW(wt->t, txt, 48);
}

static void widget_draw(int force)
{
    if (!g_wWnd) return;
    widget_get_dpi();

    static AppConfig snap;
    EnterCriticalSection(&g_stateCs);
    snap = g_cfg;
    LeaveCriticalSection(&g_stateCs);
    int fetching = (g_anyFetching != 0);
    ULONGLONG now = GetTickCount64();
    int spinStep = fetching ? (int)((now / 55ULL) % 12ULL) : 0;

    /* Collect enabled coins */
    int enIdx[MAX_COINS];
    int enCount = 0;
    int anyWs = 0, anyOk = 0, anyFlash = 0;
    SYSTEMTIME latestTime = {0};
    ULONGLONG latestMs = 0;
    for (int i = 0; i < snap.coinCount; i++) {
        if (!snap.coins[i].enabled) continue;
        enIdx[enCount++] = i;
        if (snap.coins[i].wsLive) anyWs = 1;
        if (snap.enableTickFlash && snap.coins[i].flashDir != 0 &&
            snap.coins[i].flashStartMs > 0 && now >= snap.coins[i].flashStartMs &&
            (now - snap.coins[i].flashStartMs) < 500ULL) {
            anyFlash = 1;
        }
        if (snap.coins[i].ok) {
            anyOk = 1;
            if (snap.coins[i].lastUpdateMs >= latestMs) {
                latestMs = snap.coins[i].lastUpdateMs;
                latestTime = snap.coins[i].lastUpdateTime;
            }
        }
    }

    RECT wa;
    widget_get_work_area(&snap, &wa);
    int maxWorkW = (wa.right - wa.left) - WSC(16);
    int maxWorkH = (wa.bottom - wa.top) - WSC(16);
    if (maxWorkW < WSC(320)) maxWorkW = WSC(320);
    if (maxWorkH < WSC(200)) maxWorkH = WSC(200);

    if (g_wScroll < 0) g_wScroll = 0;

    /* Build render signature to avoid redundant redraws */
    static wchar_t sig[4096];
    int sp = _snwprintf(sig, 4096, L"%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
                        g_wDpi, g_light, (int)snap.widgetLayout, snap.widgetOpacity,
                        snap.widgetLocked, snap.widgetClickThrough, fetching, spinStep,
                        snap.showSparkline, snap.enableTickFlash,
                        (int)g_wHoverKind, g_wHoverCoin, g_wScroll, enCount,
                        maxWorkW, maxWorkH);
    if (anyFlash && sp > 0 && sp < 4020) {
        sp += _snwprintf(sig + sp, 4096 - sp, L"F%llu|", (unsigned long long)(now / 45ULL));
    }
    for (int i = 0; i < enCount && sp > 0 && sp < 3920; i++) {
        const CoinItem *c = &snap.coins[enIdx[i]];
        wchar_t pstr[32], pct[16] = L"";
        if (c->ok) {
            format_price(c->price, c->decimals, snap.showCurrencySign, snap.compactPrices, pstr, 32);
            if (snap.showChangePct && c->hasChange24h) format_pct(c->change24h, pct, 16);
        } else {
            lstrcpyW(pstr, c->fetching ? L"..." : L"--");
        }
        sp += _snwprintf(sig + sp, 4096 - sp, L"%s|%s|%s|%d|%d|", c->label, pstr, pct, c->wsLive, c->histCount);
    }
    if (anyOk && sp > 0 && sp < 4060) {
        sp += _snwprintf(sig + sp, 4096 - sp, L"%02u%02u%02u", latestTime.wHour, latestTime.wMinute, latestTime.wSecond);
    }
    sig[4095] = 0;
    if (!force && lstrcmpW(sig, g_wSig) == 0) return;
    lstrcpyW(g_wSig, sig);

    /* Theme palette */
    COLORREF bgCol     = g_light ? RGB(246, 248, 252) : RGB(22, 24, 30);
    COLORREF borderCol = g_light ? RGB(180, 188, 200) : RGB(90, 98, 115);
    COLORREF fgCol     = g_light ? RGB(20, 22, 28)    : RGB(245, 247, 250);
    COLORREF mutedCol  = g_light ? RGB(95, 102, 115)  : RGB(155, 163, 178);
    COLORREF upCol     = g_light ? RGB(14, 138, 60)   : RGB(72, 218, 128);
    COLORREF downCol   = g_light ? RGB(205, 38, 38)   : RGB(255, 96, 96);
    COLORREF accCol    = g_light ? RGB(0, 103, 192)   : RGB(90, 195, 255);
    COLORREF lockCol   = g_light ? RGB(195, 110, 0)   : RGB(250, 185, 65);
    COLORREF dotCol    = anyWs ? upCol : (anyOk ? accCol : mutedCol);

    int cardAlpha = (snap.widgetOpacity * 255) / 100;
    if (cardAlpha < 64) cardAlpha = 64;
    if (cardAlpha > 255) cardAlpha = 255;

    HDC mdc = CreateCompatibleDC(NULL);
    widget_fonts(mdc);

    static WText texts[W_MAX_TEXTS];
    int tcnt = 0;
    static WSpark sparks[MAX_COINS];
    int spkCnt = 0;
    g_wHitCount = 0;

    int W = 0, H = 0;
    int hdrH = 0;
    int dotX = 0, dotY = 0, dotR = 0;
    int radius = WSC(11);
    int vColCount = 1, vColW = 0, vColStartX = 0, vDivTop = 0, vDivBot = 0;
    int hRowCount = 1, hRowH = 0, hPadY = 0;

    wchar_t icRef[2] = { g_wIcoOK ? 0xE72C : L'R', 0 };
    wchar_t icLay[2] = { g_wIcoOK ? 0xE8AB : L'=', 0 };
    wchar_t icLck[2] = { g_wIcoOK ? (snap.widgetLocked || snap.widgetClickThrough ? 0xE72E : 0xE785) : L'L', 0 };
    wchar_t icSet[2] = { g_wIcoOK ? 0xE713 : L'*', 0 };
    wchar_t icCls[2] = { g_wIcoOK ? 0xE711 : L'X', 0 };
    int fk = g_wIcoOK ? 3 : 2;

    if (snap.widgetLayout == WIDGET_VERTICAL) {
        int outerX  = WSC(6);
        int colPadX = WSC(10);
        int rowH    = WSC(25);
        hdrH        = WSC(30);
        int footH   = WSC(20);
        int wantSpk = snap.showSparkline ? 1 : 0;
        int colW    = wantSpk ? WSC(316) : WSC(260);

        int maxRowsFit = (maxWorkH - hdrH - footH - WSC(14)) / rowH;
        if (maxRowsFit < 4) maxRowsFit = 4;
        int maxColsFit = (maxWorkW - outerX * 2) / colW;
        if (maxColsFit < 1) maxColsFit = 1;

        int colCount = 1, visRows = 1;
        if (enCount <= 0) {
            colCount = 1;
            visRows  = 1;
        } else if (enCount <= maxRowsFit) {
            colCount = 1;
            visRows  = enCount;
        } else {
            colCount = (enCount + maxRowsFit - 1) / maxRowsFit;
            if (colCount > maxColsFit) colCount = maxColsFit;
            visRows  = (enCount + colCount - 1) / colCount;
            if (visRows > maxRowsFit) visRows = maxRowsFit;
        }

        int totalCap = visRows * colCount;
        int maxScroll = enCount > totalCap ? (enCount - totalCap) : 0;
        if (g_wScroll > maxScroll) g_wScroll = maxScroll;
        if (g_wScroll < 0) g_wScroll = 0;

        W = outerX * 2 + colCount * colW;
        H = hdrH + WSC(4) + visRows * rowH + WSC(4) + footH + WSC(4);

        vColCount  = colCount;
        vColW      = colW;
        vColStartX = outerX;
        vDivTop    = hdrH + WSC(4);
        vDivBot    = hdrH + WSC(4) + visRows * rowH;

        /* Header status dot + title */
        dotX = outerX + colPadX + WSC(3);
        dotY = hdrH / 2;
        dotR = WSC(3);
        add_text(texts, &tcnt, W_MAX_TEXTS, outerX + colPadX + WSC(12), WSC(5), WSC(115), WSC(20), 1, 0, fgCol, 240, L"TrayCoin");

        /* Header icon buttons: Refresh, Layout, Lock, Settings, Close */
        int btnW = WSC(22), btnH = WSC(22), btnY = (hdrH - btnH) / 2;
        int bx = W - WSC(8) - btnW * 5 - WSC(8);

        add_hit(WHIT_REFRESH,  -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fetching ? accCol : fgCol, 220, icRef);
        bx += btnW + WSC(2);

        add_hit(WHIT_LAYOUT,   -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 200, icLay);
        bx += btnW + WSC(2);

        add_hit(WHIT_LOCK,     -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0,
                 (snap.widgetLocked || snap.widgetClickThrough) ? lockCol : fgCol, 215, icLck);
        bx += btnW + WSC(2);

        add_hit(WHIT_SETTINGS, -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 200, icSet);
        bx += btnW + WSC(2);

        add_hit(WHIT_CLOSE,    -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 200, icCls);

        /* Coin rows across 1..N columns */
        if (enCount == 0) {
            add_text(texts, &tcnt, W_MAX_TEXTS, outerX + colPadX, hdrH + WSC(4),
                     W - (outerX + colPadX) * 2, rowH, 2, 0, mutedCol, 220, L"No coins enabled (click Settings)");
        } else {
            int drawCount = enCount - g_wScroll;
            if (drawCount > totalCap) drawCount = totalCap;
            for (int v = 0; v < drawCount; v++) {
                int idx = enIdx[g_wScroll + v];
                const CoinItem *c = &snap.coins[idx];
                int col = v / visRows;
                int row = v % visRows;
                int cx0 = outerX + col * colW;
                int ry  = hdrH + WSC(4) + row * rowH;

                wchar_t lbl[24], pstr[32], pct[16] = L"";
                provider_default_label(c, lbl, 24);

                float flashT = 0.0f;
                if (snap.enableTickFlash && c->flashDir != 0 && c->flashStartMs > 0 && now >= c->flashStartMs) {
                    ULONGLONG dt = now - c->flashStartMs;
                    if (dt < 500ULL) flashT = 1.0f - (float)dt / 500.0f;
                }
                COLORREF flashCol = (c->flashDir > 0) ? upCol : downCol;

                add_hit(WHIT_COIN, idx, cx0 + WSC(2), ry, cx0 + colW - WSC(2), ry + rowH);
                add_text(texts, &tcnt, W_MAX_TEXTS, cx0 + colPadX, ry, WSC(66), rowH, 1, 0, fgCol, 245, lbl);

                int priceX = cx0 + colPadX + WSC(70);
                if (wantSpk && spkCnt < MAX_COINS) {
                    sparks[spkCnt].x = cx0 + colPadX + WSC(68);
                    sparks[spkCnt].y = ry + WSC(6);
                    sparks[spkCnt].w = WSC(46);
                    sparks[spkCnt].h = rowH - WSC(12);
                    sparks[spkCnt].coinIdx = idx;
                    spkCnt++;
                    priceX = cx0 + colPadX + WSC(120);
                }

                int colRight = cx0 + colW - colPadX;
                if (c->ok) {
                    format_price(c->price, c->decimals, snap.showCurrencySign, snap.compactPrices, pstr, 32);
                    COLORREF pCol = fgCol;
                    if (snap.colorByChange && c->hasChange24h && !snap.showChangePct)
                        pCol = (c->change24h >= 0.0) ? upCol : downCol;
                    if (flashT > 0.01f) pCol = w_lerp_color(pCol, flashCol, flashT);

                    int pctW = (snap.showChangePct && c->hasChange24h) ? WSC(64) : 0;
                    int priceW = (colRight - pctW) - priceX;
                    if (priceW < WSC(56)) priceW = WSC(56);
                    add_text(texts, &tcnt, W_MAX_TEXTS, priceX, ry, priceW, rowH, 0, 1, pCol, 255, pstr);

                    if (snap.showChangePct && c->hasChange24h) {
                        format_pct(c->change24h, pct, 16);
                        COLORREF cCol = snap.colorByChange ? ((c->change24h >= 0.0) ? upCol : downCol) : mutedCol;
                        add_text(texts, &tcnt, W_MAX_TEXTS, colRight - WSC(64), ry, WSC(64), rowH, 0, 1, cCol, 245, pct);
                    }
                } else {
                    add_text(texts, &tcnt, W_MAX_TEXTS, priceX, ry, colRight - priceX, rowH,
                             0, 1, mutedCol, 200, c->fetching ? L"..." : L"--");
                }
            }
        }

        /* Footer status (with optional Total Portfolio value) */
        double totalHold = 0.0;
        for (int i = 0; i < snap.coinCount; i++) {
            if (snap.coins[i].ok && snap.coins[i].holdingQty > 0.0)
                totalHold += snap.coins[i].price * snap.coins[i].holdingQty;
        }
        wchar_t foot[96];
        if (snap.widgetClickThrough) {
            lstrcpyW(foot, L"Click-Through ON  •  Ctrl+Shift+L");
        } else if (snap.widgetLocked) {
            lstrcpyW(foot, L"Locked  •  Ctrl+Shift+L to unlock");
        } else if (totalHold > 0.0) {
            wchar_t pval[32];
            format_price(totalHold, 2, 1, 0, pval, 32);
            _snwprintf(foot, 96, L"Portfolio: %s  •  %02u:%02u:%02u",
                       pval, latestTime.wHour, latestTime.wMinute, latestTime.wSecond);
        } else if (enCount > totalCap) {
            _snwprintf(foot, 96, L"Scroll %d-%d/%d  •  %02u:%02u:%02u",
                       g_wScroll + 1, g_wScroll + totalCap, enCount,
                       latestTime.wHour, latestTime.wMinute, latestTime.wSecond);
        } else if (anyOk) {
            if (colCount > 1) {
                _snwprintf(foot, 96, L"%s  •  %d coins (%d cols)  •  %02u:%02u:%02u",
                           anyWs ? L"Live WS" : L"Synced", enCount, colCount,
                           latestTime.wHour, latestTime.wMinute, latestTime.wSecond);
            } else {
                _snwprintf(foot, 96, L"%s  •  %02u:%02u:%02u",
                           anyWs ? L"Live WS" : L"Synced",
                           latestTime.wHour, latestTime.wMinute, latestTime.wSecond);
            }
        } else {
            lstrcpyW(foot, L"Connecting...");
        }
        add_text(texts, &tcnt, W_MAX_TEXTS, outerX + colPadX, H - footH - WSC(3),
                 W - (outerX + colPadX) * 2, footH, 2, 0, mutedCol, 200, foot);
    } else {
        /* Horizontal Floating Pill Layout with Lock + Settings icons and automatic multi-row wrapping */
        radius = WSC(10);
        int rowH = WSC(30);
        int padY = WSC(2);
        int btnW = WSC(22), btnH = WSC(22);
        int btnY = padY + (rowH - btnH) / 2;

        /* Left action icon on Row 0: Refresh */
        int leftStartX = WSC(8);
        add_hit(WHIT_REFRESH, -1, leftStartX, btnY, leftStartX + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, leftStartX, btnY, btnW, btnH, fk, 0, fetching ? accCol : fgCol, 225, icRef);

        /* Reserve space on right of Row 0 for 4 icons: Layout, Lock, Settings, Close */
        int rightClusterW = btnW * 4 + WSC(2) * 3;
        int rightReserved = WSC(8) + rightClusterW + WSC(8);

        int curRow = 0;
        int curX = leftStartX + btnW + WSC(8);
        int itemsInRow = 0;
        int row0EndX = curX;
        int maxContentRight = curX;
        int maxRowsHoriz = (maxWorkH - padY * 2) / rowH;
        if (maxRowsHoriz < 1) maxRowsHoriz = 1;

        if (enCount == 0) {
            int tw = WSC(64);
            add_text(texts, &tcnt, W_MAX_TEXTS, curX, padY, tw, rowH, 1, 0, fgCol, 230, L"TrayCoin");
            row0EndX = curX + tw + WSC(6);
            maxContentRight = row0EndX;
        } else {
            for (int i = 0; i < enCount; i++) {
                int idx = enIdx[i];
                const CoinItem *c = &snap.coins[idx];
                wchar_t lbl[24], pstr[32], pct[16] = L"";
                provider_default_label(c, lbl, 24);
                if (c->ok) {
                    format_price(c->price, c->decimals, snap.showCurrencySign, snap.compactPrices, pstr, 32);
                    if (snap.showChangePct && c->hasChange24h) format_pct(c->change24h, pct, 16);
                } else {
                    lstrcpyW(pstr, c->fetching ? L"..." : L"--");
                }

                SIZE sl = {0}, sp2 = {0}, sc2 = {0};
                SelectObject(mdc, g_wBold); GetTextExtentPoint32W(mdc, lbl, lstrlenW(lbl), &sl);
                SelectObject(mdc, g_wFont); GetTextExtentPoint32W(mdc, pstr, lstrlenW(pstr), &sp2);
                if (pct[0]) GetTextExtentPoint32W(mdc, pct, lstrlenW(pct), &sc2);

                int wl = (sl.cx + WSS - 1) / WSS + WSC(4);
                int wp = (sp2.cx + WSS - 1) / WSS + WSC(4);
                int wc = pct[0] ? ((sc2.cx + WSS - 1) / WSS + WSC(4)) : 0;
                int itemW = wl + WSC(2) + wp + (wc > 0 ? (WSC(2) + wc) : 0);

                int rowLimitX = (curRow == 0) ? (maxWorkW - rightReserved) : (maxWorkW - WSC(10));
                if (itemsInRow > 0 && curX + itemW > rowLimitX) {
                    if (curRow + 1 >= maxRowsHoriz) break;
                    curRow++;
                    curX = WSC(10);
                    itemsInRow = 0;
                }

                int rowY = padY + curRow * rowH;
                int startX = curX;

                add_text(texts, &tcnt, W_MAX_TEXTS, curX, rowY, wl, rowH, 1, 0, fgCol, 215, lbl);
                curX += wl + WSC(2);

                COLORREF pCol = c->ok ? fgCol : mutedCol;
                if (c->ok && snap.colorByChange && c->hasChange24h && !snap.showChangePct)
                    pCol = (c->change24h >= 0.0) ? upCol : downCol;
                if (c->ok && snap.enableTickFlash && c->flashDir != 0 && c->flashStartMs > 0 && now >= c->flashStartMs) {
                    ULONGLONG dt = now - c->flashStartMs;
                    if (dt < 500ULL) {
                        float ft = 1.0f - (float)dt / 500.0f;
                        pCol = w_lerp_color(pCol, (c->flashDir > 0) ? upCol : downCol, ft);
                    }
                }
                add_text(texts, &tcnt, W_MAX_TEXTS, curX, rowY, wp, rowH, 0, 0, pCol, 255, pstr);
                curX += wp;

                if (pct[0]) {
                    curX += WSC(2);
                    COLORREF cCol = snap.colorByChange ? ((c->change24h >= 0.0) ? upCol : downCol) : mutedCol;
                    add_text(texts, &tcnt, W_MAX_TEXTS, curX, rowY, wc, rowH, 0, 0, cCol, 240, pct);
                    curX += wc;
                }
                add_hit(WHIT_COIN, idx, startX - WSC(3), rowY + WSC(2), curX + WSC(3), rowY + rowH - WSC(2));

                if (curRow == 0 && curX > row0EndX) row0EndX = curX;
                if (curX > maxContentRight) maxContentRight = curX;

                curX += WSC(14);
                itemsInRow++;
            }
        }

        hRowCount = curRow + 1;
        hRowH     = rowH;
        hPadY     = padY;

        int needW0 = row0EndX + rightReserved;
        int needWO = maxContentRight + WSC(10);
        W = (needW0 > needWO) ? needW0 : needWO;
        if (W > maxWorkW) W = maxWorkW;
        if (W < WSC(180)) W = WSC(180);
        H = hRowCount * rowH + padY * 2;

        /* Right-aligned action icons on Row 0: Layout, Lock, Settings, Close */
        int bx = W - WSC(8) - rightClusterW;

        add_hit(WHIT_LAYOUT, -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 195, icLay);
        bx += btnW + WSC(2);

        add_hit(WHIT_LOCK, -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0,
                 (snap.widgetLocked || snap.widgetClickThrough) ? lockCol : fgCol, 215, icLck);
        bx += btnW + WSC(2);

        add_hit(WHIT_SETTINGS, -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 195, icSet);
        bx += btnW + WSC(2);

        add_hit(WHIT_CLOSE, -1, bx, btnY, bx + btnW, btnY + btnH);
        add_text(texts, &tcnt, W_MAX_TEXTS, bx, btnY, btnW, btnH, fk, 0, fgCol, 195, icCls);
    }

    if (W < WSC(140)) W = WSC(140);
    if (H < WSC(30))  H = WSC(30);

    /* Allocate 1x output DIB and 2x text-mask DIB */
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    DWORD *outPx = NULL, *txtPx = NULL;
    HBITMAP outBmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&outPx, NULL, 0);

    int WB = W * WSS, HB = H * WSS;
    bi.bmiHeader.biWidth = WB;
    bi.bmiHeader.biHeight = -HB;
    HBITMAP txtBmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&txtPx, NULL, 0);
    if (!outBmp || !txtBmp) {
        if (outBmp) DeleteObject(outBmp);
        if (txtBmp) DeleteObject(txtBmp);
        DeleteDC(mdc);
        return;
    }

    /* Pass 1: Analytic anti-aliased rounded glass card + border + divider + hover pill + live dot */
    float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
    float hx = W * 0.5f - 0.5f, hy = H * 0.5f - 0.5f, rad = (float)radius;
    DWORD bgR = GetRValue(bgCol), bgG = GetGValue(bgCol), bgB = GetBValue(bgCol);
    DWORD bdR = GetRValue(borderCol), bdG = GetGValue(borderCol), bdB = GetBValue(borderCol);
    DWORD bdA = g_light ? 95 : 75;

    int hasHover = 0;
    RECT hoverRc = {0, 0, 0, 0};
    if (g_wHoverKind != WHIT_NONE) {
        for (int i = 0; i < g_wHitCount; i++) {
            if (g_wHits[i].kind == g_wHoverKind &&
                (g_wHoverKind != WHIT_COIN || g_wHits[i].coinIdx == g_wHoverCoin)) {
                hoverRc = g_wHits[i].rc;
                hasHover = 1;
                break;
            }
        }
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float d = sdf_roundrect((float)x, (float)y, cx, cy, hx, hy, rad);
            if (d >= 0.75f) {
                outPx[y * W + x] = 0;
                continue;
            }
            float cov = 0.75f - d;
            if (cov > 1.0f) cov = 1.0f;
            if (cov < 0.0f) cov = 0.0f;
            DWORD a = (DWORD)(cardAlpha * cov + 0.5f);
            DWORD px = (a << 24) | ((bgR * a / 255) << 16) | ((bgG * a / 255) << 8) | (bgB * a / 255);

            /* 1px anti-aliased hairline border */
            if (d > -1.35f) {
                float bcov = 1.0f - fabsf(d + 0.35f);
                if (bcov > 0.0f) {
                    DWORD ba = (DWORD)(bdA * bcov * cov + 0.5f);
                    px = blend_over(px, bdR, bdG, bdB, ba);
                }
            }
            /* Subtle header divider in Vertical layout */
            if (hdrH > 0 && y == hdrH && x >= WSC(8) && x < W - WSC(8)) {
                px = blend_over(px, bdR, bdG, bdB, g_light ? 45 : 38);
            }
            /* Subtle vertical column hairlines in multi-column Vertical layout */
            if (vColCount > 1 && y >= vDivTop && y < vDivBot &&
                x > vColStartX && x < vColStartX + vColCount * vColW &&
                ((x - vColStartX) % vColW) == 0) {
                px = blend_over(px, bdR, bdG, bdB, g_light ? 40 : 32);
            }
            /* Subtle horizontal row hairlines in multi-row Horizontal layout */
            if (hRowCount > 1 && x >= WSC(8) && x < W - WSC(8) &&
                y > hPadY && y < hPadY + hRowCount * hRowH &&
                ((y - hPadY) % hRowH) == 0) {
                px = blend_over(px, bdR, bdG, bdB, g_light ? 36 : 28);
            }
            /* Hover pill highlight */
            if (hasHover && x >= hoverRc.left && x < hoverRc.right && y >= hoverRc.top && y < hoverRc.bottom) {
                int corner = ((x == hoverRc.left || x == hoverRc.right - 1) &&
                              (y == hoverRc.top  || y == hoverRc.bottom - 1));
                if (!corner) {
                    DWORD hc = g_light ? 0 : 255;
                    px = blend_over(px, hc, hc, hc, g_light ? 18 : 26);
                }
            }
            /* Live status dot in Vertical header */
            if (dotR > 0) {
                float ddx = (float)(x - dotX), ddy = (float)(y - dotY);
                float dist = sqrtf(ddx * ddx + ddy * ddy) - (float)dotR;
                if (dist < 0.75f) {
                    float dcov = 0.75f - dist;
                    if (dcov > 1.0f) dcov = 1.0f;
                    if (dcov > 0.0f) {
                        px = blend_over(px, GetRValue(dotCol), GetGValue(dotCol), GetBValue(dotCol), (DWORD)(245.0f * dcov));
                    }
                }
            }
            outPx[y * W + x] = px;
        }
    }

    /* Pass 1b: Subtle row flash highlight & 24-pt anti-aliased mini sparklines */
    for (int i = 0; i < g_wHitCount; i++) {
        if (g_wHits[i].kind != WHIT_COIN) continue;
        int cid = g_wHits[i].coinIdx;
        if (cid < 0 || cid >= snap.coinCount) continue;
        const CoinItem *c = &snap.coins[cid];
        if (snap.enableTickFlash && c->flashDir != 0 && c->flashStartMs > 0 && now >= c->flashStartMs) {
            ULONGLONG dt = now - c->flashStartMs;
            if (dt < 500ULL) {
                float ft = 1.0f - (float)dt / 500.0f;
                DWORD fa = (DWORD)(30.0f * ft);
                COLORREF fc = (c->flashDir > 0) ? upCol : downCol;
                DWORD fr = GetRValue(fc), fg2 = GetGValue(fc), fb = GetBValue(fc);
                RECT rc = g_wHits[i].rc;
                for (int y = rc.top; y < rc.bottom && y < H; y++)
                    for (int x = rc.left; x < rc.right && x < W; x++)
                        if (x >= 0 && y >= 0) outPx[y * W + x] = blend_over(outPx[y * W + x], fr, fg2, fb, fa);
            }
        }
    }
    for (int i = 0; i < spkCnt; i++) {
        draw_sparkline(outPx, W, H, &sparks[i], &snap.coins[sparks[i].coinIdx], upCol, downCol, mutedCol);
    }

    /* Pass 2: Render each text/icon item on 2x DIB and composite onto outPx */
    HGDIOBJ ob = SelectObject(mdc, txtBmp);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(255, 255, 255));

    for (int i = 0; i < tcnt; i++) {
        const WText *wt = &texts[i];
        HFONT f = (wt->fontKind == 1) ? g_wBold : (wt->fontKind == 2) ? g_wSmall : (wt->fontKind == 3 && g_wIco) ? g_wIco : g_wFont;
        SelectObject(mdc, f);

        int bx0 = wt->x / WSS - 1; if (bx0 < 0) bx0 = 0;
        int by0 = wt->y / WSS - 1; if (by0 < 0) by0 = 0;
        int bx1 = (wt->x + wt->w) / WSS + 2; if (bx1 > W) bx1 = W;
        int by1 = (wt->y + wt->h) / WSS + 2; if (by1 > H) by1 = H;

        /* Clear only the bounding box in 2x buffer for speed */
        for (int yy = by0 * WSS; yy < by1 * WSS; yy++) {
            ZeroMemory(txtPx + yy * WB + bx0 * WSS, (size_t)(bx1 - bx0) * WSS * 4);
        }

        RECT rc = { wt->x, wt->y, wt->x + wt->w, wt->y + wt->h };
        UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                   (wt->fontKind == 3 ? DT_CENTER : (wt->alignRight ? DT_RIGHT : DT_LEFT));
        DrawTextW(mdc, wt->t, lstrlenW(wt->t), &rc, fmt);
        GdiFlush();

        DWORD cr = GetRValue(wt->c), cg = GetGValue(wt->c), cb = GetBValue(wt->c);
        for (int y = by0; y < by1; y++) {
            for (int x = bx0; x < bx1; x++) {
                unsigned sum = 0;
                for (int j = 0; j < WSS; j++) {
                    const DWORD *row = txtPx + (y * WSS + j) * WB + x * WSS;
                    for (int k = 0; k < WSS; k++) sum += row[k] & 0xFF;
                }
                if (sum == 0) continue;
                DWORD a = (sum / (WSS * WSS)) * wt->al / 255;
                if (a > 0) outPx[y * W + x] = blend_over(outPx[y * W + x], cr, cg, cb, a);
            }
        }
    }

    SelectObject(mdc, outBmp);
    DeleteObject(txtBmp);

    /* Position & present via UpdateLayeredWindow */
    LONG wx = snap.widgetX, wy = snap.widgetY;
    widget_clamp_pos(&wx, &wy, W, H);
    if (wx != snap.widgetX || wy != snap.widgetY) {
        EnterCriticalSection(&g_stateCs);
        g_cfg.widgetX = wx;
        g_cfg.widgetY = wy;
        LeaveCriticalSection(&g_stateCs);
    }

    POINT dst = { wx, wy };
    POINT src = { 0, 0 };
    SIZE  sz  = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    g_wW = W;
    g_wH = H;

    UpdateLayeredWindow(g_wWnd, NULL, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(g_wWnd)) ShowWindow(g_wWnd, SW_SHOWNOACTIVATE);
    widget_apply_styles(&snap);

    SelectObject(mdc, ob);
    DeleteObject(outBmp);
    DeleteDC(mdc);
}

static WHitBox *widget_hit(int x, int y)
{
    for (int i = 0; i < g_wHitCount; i++) {
        if (x >= g_wHits[i].rc.left && x < g_wHits[i].rc.right &&
            y >= g_wHits[i].rc.top  && y < g_wHits[i].rc.bottom) {
            return &g_wHits[i];
        }
    }
    return NULL;
}

static LRESULT CALLBACK widget_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        if (g_wHoverKind != WHIT_NONE) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_cfg.widgetLocked ? IDC_ARROW : IDC_SIZEALL)));
        return TRUE;

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (g_wDragging) {
            POINT cur; GetCursorPos(&cur);
            int dx = cur.x - g_wDragStartScreen.x;
            int dy = cur.y - g_wDragStartScreen.y;
            if (!g_wDragMoved && (abs(dx) > 4 || abs(dy) > 4)) g_wDragMoved = 1;
            if (g_wDragMoved && !g_cfg.widgetLocked) {
                LONG nx = g_wDragStartWin.x + dx;
                LONG ny = g_wDragStartWin.y + dy;
                /* Soft magnetic snap to monitor work-area edges (within 12px) */
                HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(hm, &mi)) {
                    RECT wa = mi.rcWork;
                    int snapD = WSC(12);
                    if (abs(nx - wa.left) < snapD) nx = wa.left;
                    else if (abs((nx + g_wW) - wa.right) < snapD) nx = wa.right - g_wW;
                    if (abs(ny - wa.top) < snapD) ny = wa.top;
                    else if (abs((ny + g_wH) - wa.bottom) < snapD) ny = wa.bottom - g_wH;
                }
                SetWindowPos(w, NULL, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                EnterCriticalSection(&g_stateCs);
                g_cfg.widgetX = nx;
                g_cfg.widgetY = ny;
                LeaveCriticalSection(&g_stateCs);
            }
            return 0;
        }
        if (!g_wTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, w, 0 };
            TrackMouseEvent(&tme);
            g_wTracking = 1;
        }
        WHitBox *hb = widget_hit(x, y);
        WHitKind nk = hb ? hb->kind : WHIT_NONE;
        int nc = hb ? hb->coinIdx : -1;
        if (nk != g_wHoverKind || nc != g_wHoverCoin) {
            g_wHoverKind = nk;
            g_wHoverCoin = nc;
            widget_draw(0);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_wTracking = 0;
        if (g_wHoverKind != WHIT_NONE) {
            g_wHoverKind = WHIT_NONE;
            g_wHoverCoin = -1;
            widget_draw(0);
        }
        return 0;

    case WM_LBUTTONDOWN:
        g_wDragging = 1;
        g_wDragMoved = 0;
        GetCursorPos(&g_wDragStartScreen);
        {
            RECT wr; GetWindowRect(w, &wr);
            g_wDragStartWin.x = wr.left;
            g_wDragStartWin.y = wr.top;
        }
        SetCapture(w);
        return 0;

    case WM_LBUTTONUP: {
        int wasDragged = g_wDragMoved;
        if (g_wDragging) {
            g_wDragging = 0;
            g_wDragMoved = 0;
            ReleaseCapture();
        }
        if (wasDragged) {
            EnterCriticalSection(&g_stateCs);
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            return 0;
        }
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        WHitBox *hb = widget_hit(x, y);
        if (!hb) return 0;

        if (hb->kind == WHIT_REFRESH) {
            LOGI(L"WIDGET", L"Refresh clicked on widget");
            poller_trigger_refresh(0);
        } else if (hb->kind == WHIT_LAYOUT) {
            EnterCriticalSection(&g_stateCs);
            g_cfg.widgetLayout = (g_cfg.widgetLayout == WIDGET_VERTICAL) ? WIDGET_HORIZONTAL : WIDGET_VERTICAL;
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            widget_refresh(1);
            ui_settings_sync_controls();
        } else if (hb->kind == WHIT_LOCK) {
            EnterCriticalSection(&g_stateCs);
            g_cfg.widgetLocked = !g_cfg.widgetLocked;
            if (!g_cfg.widgetLocked) g_cfg.widgetClickThrough = 0;
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            widget_refresh(1);
            ui_settings_sync_controls();
        } else if (hb->kind == WHIT_SETTINGS) {
            ui_settings_open();
        } else if (hb->kind == WHIT_CLOSE) {
            EnterCriticalSection(&g_stateCs);
            g_cfg.showWidget = 0;
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            widget_refresh(1);
            ui_settings_sync_controls();
        } else if (hb->kind == WHIT_COIN) {
            wchar_t url[256] = {0};
            CoinClickAction act = CLICK_OPEN_CHART;
            EnterCriticalSection(&g_stateCs);
            act = g_cfg.clickAction;
            if (hb->coinIdx >= 0 && hb->coinIdx < g_cfg.coinCount)
                lstrcpynW(url, g_cfg.coins[hb->coinIdx].pairUrl, 256);
            LeaveCriticalSection(&g_stateCs);
            if (act == CLICK_REFRESH) poller_trigger_refresh(0);
            else if (act == CLICK_OPEN_CHART && url[0]) ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            else ui_settings_open();
        }
        return 0;
    }

    case WM_RBUTTONUP:
        taskbar_show_menu();
        return 0;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_wScroll += (delta < 0) ? 1 : -1;
        widget_draw(0);
        return 0;
    }

    case WM_DESTROY:
        g_wWnd = NULL;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void widget_init(void)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = widget_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TrayCoinWidget";
        RegisterClassW(&wc);
        reg = 1;
    }
}

void widget_refresh(int force)
{
    int want = 0;
    EnterCriticalSection(&g_stateCs);
    want = g_cfg.showWidget;
    LeaveCriticalSection(&g_stateCs);

    if (!want) {
        if (g_wWnd && IsWindow(g_wWnd)) {
            DestroyWindow(g_wWnd);
            g_wWnd = NULL;
        }
        g_wSig[0] = 0;
        return;
    }

    if (!g_wWnd || !IsWindow(g_wWnd)) {
        widget_init();
        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                   (g_cfg.widgetTopmost ? WS_EX_TOPMOST : 0) |
                   (g_cfg.widgetClickThrough ? WS_EX_TRANSPARENT : 0);
        g_wWnd = CreateWindowExW(ex, L"TrayCoinWidget", L"TrayCoin Widget", WS_POPUP,
                                 0, 0, 1, 1, NULL, NULL, g_hinst, NULL);
        g_wSig[0] = 0;
        force = 1;
        LOGI(L"WIDGET", L"Floating widget created (hwnd=%p)", g_wWnd);
    }
    if (g_wWnd) widget_draw(force);
}

void widget_reset_position(void)
{
    EnterCriticalSection(&g_stateCs);
    g_cfg.widgetX = WIDGET_POS_UNSET;
    g_cfg.widgetY = WIDGET_POS_UNSET;
    config_save(&g_cfg);
    LeaveCriticalSection(&g_stateCs);
    widget_refresh(1);
}

void widget_shutdown(void)
{
    if (g_wWnd && IsWindow(g_wWnd)) DestroyWindow(g_wWnd);
    g_wWnd = NULL;
    if (g_wFont)  { DeleteObject(g_wFont);  g_wFont  = NULL; }
    if (g_wBold)  { DeleteObject(g_wBold);  g_wBold  = NULL; }
    if (g_wSmall) { DeleteObject(g_wSmall); g_wSmall = NULL; }
    if (g_wIco)   { DeleteObject(g_wIco);   g_wIco   = NULL; }
}
