#define COBJMACROS
/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro) - Modified 2026-11
 * Based on TraySense, Copyright (C) 2026 DMTWEAKS.
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 * See LICENSE and NOTICE for full copyright and modification details.
 */
#include "taskbar.h"
#include "config.h"
#include "providers.h"
#include "poller.h"
#include "ws.h"
#include "logger.h"
#include "widget.h"
#include "ui_settings.h"
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>
#include <uiautomationclient.h>
#include <stdio.h>
#include <math.h>

#define MAX_BARS 8
#define SS 4
#define MAX_SEGS 160
#define MAX_HITS (MAX_COINS + 2)
#define BAR_MAX_W 1600

typedef enum {
    HIT_NONE = 0,
    HIT_REFRESH,
    HIT_PAGE,
    HIT_COIN,
    HIT_EMPTY
} HitKind;

typedef struct {
    HitKind kind;
    int     coinIdx;
    RECT    rc;       /* Client pixels (1x) */
} HitBox;

typedef struct {
    int      isRefresh;
    int      isPage;
    int      coinIdx;     /* -1 for refresh/page/placeholder */
    int      x, y, w;     /* Supersampled coords */
    int      al;          /* Alpha 0..255 */
    int      smallFont;   /* 1 = use bar->smFont */
    int      row;         /* -1 = full height, 0 = top half, 1 = bottom half */
    int      isLabel;     /* 1 = symbol label, 0 = price/pct */
    wchar_t  t[32];
    COLORREF c;
} SEG;

typedef struct {
    HWND      wnd, tb, tip;
    int       child, primary, W, H, compact, wideW, fontDpi, icoOK;
    int       availW, page, pageCount;
    ULONGLONG lastRotateMs;
    RECT      lastPos;
    wchar_t   sig[1024];
    HFONT     font, bold, smFont, ico;
    HitBox    hits[MAX_HITS];
    int       hitCount;
    HitKind   hoverKind;
    int       hoverCoinIdx;
    int       trackingMouse;
    wchar_t   tipText[256];
} BAR;

typedef struct {
    HWND tb;
    LONG ok, trayL, appsR;
} SCAN;

static BAR              g_bars[MAX_BARS];
static BAR             *bar = g_bars;
static int              g_dpi = 96;
static SCAN             g_scan[MAX_BARS];
static CRITICAL_SECTION g_scanCs;
static int              g_scanCsInit = 0;

static IUIAutomation             *g_uia = NULL;
static IUIAutomationCacheRequest *g_uiaCache = NULL;
static IUIAutomationCondition    *g_uiaAll = NULL;

static const GUID CLSID_UIA = {0xff48dba4,0x60ef,0x4201,{0xaa,0x87,0x54,0x10,0x3e,0xef,0x59,0x4e}};
static const GUID IID_UIA   = {0x30cbe57d,0xd9d0,0x452a,{0xab,0x13,0x7a,0xc5,0xac,0x48,0x25,0xee}};
static const GUID IID_CF    = {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static HICON g_trayIcon = NULL;
static int   g_trayShown = 0;

static int SC(int v) { return MulDiv(v, g_dpi, 96); }

static HMODULE sysdll(const wchar_t *name)
{
    return LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/* ---- Formatting helpers ---- */

static void insert_commas(const wchar_t *num, wchar_t *out, int cap)
{
    const wchar_t *dot = wcschr(num, L'.');
    int intLen = dot ? (int)(dot - num) : lstrlenW(num);
    int neg = (num[0] == L'-') ? 1 : 0;
    int digits = intLen - neg;
    int o = 0;

    if (neg && o < cap - 1) out[o++] = L'-';
    for (int i = 0; i < digits && o < cap - 2; i++) {
        if (i > 0 && ((digits - i) % 3) == 0) out[o++] = L',';
        out[o++] = num[neg + i];
    }
    if (dot) {
        for (const wchar_t *p = dot; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}

void format_price(double price, int decimals, int showDollar, int compact, wchar_t *out, int outCap)
{
    if (!out || outCap <= 0) return;
    const wchar_t *pfx = showDollar ? L"$" : L"";
    if (price <= 0.0 || isnan(price) || isinf(price)) {
        _snwprintf(out, outCap, L"%s--", pfx);
        out[outCap - 1] = 0;
        return;
    }

    if (decimals >= 0 && decimals <= 8) {
        wchar_t raw[64], withC[64];
        _snwprintf(raw, 64, L"%.*f", decimals, price);
        raw[63] = 0;
        insert_commas(raw, withC, 64);
        _snwprintf(out, outCap, L"%s%s", pfx, withC);
        out[outCap - 1] = 0;
        return;
    }

    if (compact && price >= 1000000.0) {
        _snwprintf(out, outCap, L"%s%.2fM", pfx, price / 1000000.0);
    } else if (compact && price >= 10000.0) {
        _snwprintf(out, outCap, L"%s%.1fK", pfx, price / 1000.0);
    } else if (price >= 10000.0) {
        wchar_t raw[64], withC[64];
        _snwprintf(raw, 64, L"%.0f", price);
        insert_commas(raw, withC, 64);
        _snwprintf(out, outCap, L"%s%s", pfx, withC);
    } else if (price >= 1000.0) {
        wchar_t raw[64], withC[64];
        _snwprintf(raw, 64, L"%.1f", price);
        insert_commas(raw, withC, 64);
        _snwprintf(out, outCap, L"%s%s", pfx, withC);
    } else if (price >= 100.0) {
        _snwprintf(out, outCap, L"%s%.2f", pfx, price);
    } else if (price >= 1.0) {
        _snwprintf(out, outCap, L"%s%.2f", pfx, price);
    } else if (price >= 0.01) {
        _snwprintf(out, outCap, L"%s%.4f", pfx, price);
    } else if (price >= 0.0001) {
        _snwprintf(out, outCap, L"%s%.5f", pfx, price);
    } else {
        /* Meme coin subscript notation: e.g. 0.0000185 -> $0.0₄185 */
        int exp10 = (int)floor(log10(price));
        int zeros = (-exp10) - 1;
        if (zeros < 1) zeros = 1;
        double scaled = price * pow(10.0, (double)(-exp10)); /* 1.0 .. 9.999 */
        int sig = (int)(scaled * 100.0 + 0.5); /* 3 significant digits */
        if (sig >= 1000) { sig /= 10; zeros--; }
        static const wchar_t SUBS[10] = {
            0x2080, 0x2081, 0x2082, 0x2083, 0x2084, 0x2085, 0x2086, 0x2087, 0x2088, 0x2089
        };
        wchar_t zbuf[8];
        int zo = 0;
        if (zeros >= 10 && zeros < 100) {
            zbuf[zo++] = SUBS[zeros / 10];
            zbuf[zo++] = SUBS[zeros % 10];
        } else if (zeros >= 0 && zeros < 10) {
            zbuf[zo++] = SUBS[zeros];
        }
        zbuf[zo] = 0;
        _snwprintf(out, outCap, L"%s0.0%s%03d", pfx, zbuf, sig);
    }
    out[outCap - 1] = 0;
}

void format_pct(double pct, wchar_t *out, int outCap)
{
    if (!out || outCap <= 0) return;
    if (isnan(pct) || isinf(pct)) { out[0] = 0; return; }
    if (fabs(pct) >= 100.0)
        _snwprintf(out, outCap, L"%+.0f%%", pct);
    else
        _snwprintf(out, outCap, L"%+.1f%%", pct);
    out[outCap - 1] = 0;
}

/* ---- System Tray fallback icon ---- */

static HICON make_tray_coin_icon(void)
{
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16) s = 16;
    if (s > 64) s = 64;
    int S = s * SS;

    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;

    HDC dc = CreateCompatibleDC(NULL);
    DWORD *bigPx = NULL, *px = NULL;
    HBITMAP big = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bigPx, NULL, 0);
    if (!big) { DeleteDC(dc); return NULL; }

    HGDIOBJ ob = SelectObject(dc, big);
    ZeroMemory(bigPx, (SIZE_T)S * S * 4);

    /* Draw gold coin circle with "$" glyph */
    int pad = S / 10;
    HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, S / 12, RGB(255, 255, 255));
    HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op  = SelectObject(dc, pen);
    Ellipse(dc, pad, pad, S - pad, S - pad);

    HFONT f = CreateFontW(-(int)(S * 0.62), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = { 0, 0, S, S };
    DrawTextW(dc, L"$", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    SelectObject(dc, of); DeleteObject(f);
    SelectObject(dc, op); DeleteObject(pen);
    SelectObject(dc, obr); DeleteObject(br);

    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HICON ic = NULL;
    if (bmp) {
        COLORREF col = g_light ? RGB(205, 130, 0) : RGB(245, 185, 45);
        for (int y = 0; y < s; y++) {
            for (int x = 0; x < s; x++) {
                unsigned sum = 0;
                for (int j = 0; j < SS; j++) {
                    const DWORD *row = bigPx + (y * SS + j) * S + x * SS;
                    for (int i = 0; i < SS; i++) sum += row[i] & 0xFF;
                }
                DWORD a = sum / (SS * SS);
                px[y * s + x] = a ? (a << 24) | ((GetRValue(col) * a / 255) << 16)
                                              | ((GetGValue(col) * a / 255) << 8)
                                              |  (GetBValue(col) * a / 255) : 0;
            }
        }
        BYTE maskBits[512]; ZeroMemory(maskBits, sizeof(maskBits));
        HBITMAP mask = CreateBitmap(s, s, 1, 1, maskBits);
        ICONINFO ii = { TRUE, 0, 0, mask, bmp };
        ic = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(bmp);
    }

    SelectObject(dc, ob);
    DeleteObject(big);
    DeleteDC(dc);
    return ic;
}

static void tray_icon_update(int force)
{
    int want = g_cfg.showTrayIcon;
    if (!want) {
        if (g_trayShown) {
            NOTIFYICONDATAW n; ZeroMemory(&n, sizeof(n));
            n.cbSize = sizeof(n); n.hWnd = g_wnd; n.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &n);
            g_trayShown = 0;
        }
        return;
    }
    /* Never spam Shell_NotifyIconW(NIM_MODIFY) on normal price ticks (breaks Win11 tray clicks!) */
    if (g_trayShown && !force) return;
    if (!g_trayIcon || force) {
        HICON old = g_trayIcon;
        g_trayIcon = make_tray_coin_icon();
        if (old) DestroyIcon(old);
    }
    NOTIFYICONDATAW n; ZeroMemory(&n, sizeof(n));
    n.cbSize = sizeof(n); n.hWnd = g_wnd; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_trayIcon;
    _snwprintf(n.szTip, 128, L"%s\nLeft-click: Settings | Right-click: Menu", APP_FULL_NAME);
    n.szTip[127] = 0;
    Shell_NotifyIconW(g_trayShown ? NIM_MODIFY : NIM_ADD, &n);
    g_trayShown = 1;
}

HICON taskbar_get_app_icon(void)
{
    if (!g_trayIcon) g_trayIcon = make_tray_coin_icon();
    return g_trayIcon;
}

/* ---- Taskbar discovery & UIA scan (ported from TraySense) ---- */

static int find_taskbars(HWND *out, int max)
{
    int n = 0;
    HWND p = FindWindowW(L"Shell_TrayWnd", NULL), s = NULL;
    if (p) out[n++] = p;
    while (n < max && (s = FindWindowExW(NULL, s, L"Shell_SecondaryTrayWnd", NULL))) out[n++] = s;
    return n;
}

static int uia_init(void)
{
    static int tried = 0;
    if (tried) return g_uia != NULL;
    tried = 1;
    HMODULE m = sysdll(L"UIAutomationCore.dll");
    typedef HRESULT (WINAPI *DGCO)(const GUID *, const GUID *, void **);
    DGCO get = m ? (DGCO)(void *)GetProcAddress(m, "DllGetClassObject") : NULL;
    IClassFactory *cf = NULL;
    if (!get || FAILED(get(&CLSID_UIA, &IID_CF, (void **)&cf)) || !cf) return 0;
    HRESULT hr = IClassFactory_CreateInstance(cf, NULL, &IID_UIA, (void **)&g_uia);
    IClassFactory_Release(cf);
    if (FAILED(hr) || !g_uia) { g_uia = NULL; return 0; }
    if (FAILED(IUIAutomation_CreateCacheRequest(g_uia, &g_uiaCache)) ||
        FAILED(IUIAutomation_CreateTrueCondition(g_uia, &g_uiaAll))) {
        IUIAutomation_Release(g_uia); g_uia = NULL; return 0;
    }
    IUIAutomationCacheRequest_AddProperty(g_uiaCache, UIA_ClassNamePropertyId);
    IUIAutomationCacheRequest_AddProperty(g_uiaCache, UIA_BoundingRectanglePropertyId);
    IUIAutomationCacheRequest_AddProperty(g_uiaCache, UIA_IsOffscreenPropertyId);
    IUIAutomationCacheRequest_put_AutomationElementMode(g_uiaCache, AutomationElementMode_None);
    return 1;
}

static int starts_w(const wchar_t *s, const wchar_t *p)
{
    return s && !wcsncmp(s, p, wcslen(p));
}

static int scan_one(HWND tb, int primary, LONG *outTray, LONG *outApps)
{
    static RECT rc[256];
    static BYTE app[256];
    RECT tr;
    if (!GetWindowRect(tb, &tr) ||
        !FindWindowExW(tb, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL)) return 0;
    IUIAutomationElement *root = NULL;
    IUIAutomationElementArray *arr = NULL;
    int n = 0, cnt = 0;
    if (SUCCEEDED(IUIAutomation_ElementFromHandle(g_uia, (UIA_HWND)tb, &root)) && root &&
        SUCCEEDED(IUIAutomationElement_FindAllBuildCache(root, TreeScope_Descendants, g_uiaAll, g_uiaCache, &arr)) && arr)
        IUIAutomationElementArray_get_Length(arr, &cnt);
    int tbw = tr.right - tr.left;
    for (int i = 0; i < cnt && n < 256; i++) {
        IUIAutomationElement *e = NULL;
        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e) continue;
        BSTR cls = NULL; RECT r; BOOL off = FALSE;
        IUIAutomationElement_get_CachedClassName(e, &cls);
        IUIAutomationElement_get_CachedBoundingRectangle(e, &r);
        IUIAutomationElement_get_CachedIsOffscreen(e, &off);
        int w = r.right - r.left;
        if (!off && w > 0 && r.bottom > r.top && w < tbw / 3 && r.top < tr.bottom && r.bottom > tr.top &&
            !(cls && !lstrcmpW(cls, L"TrayCoinBar"))) {
            rc[n] = r;
            app[n] = (BYTE)(starts_w(cls, L"Taskbar.") || starts_w(cls, L"StartButton") || starts_w(cls, L"SearchBox"));
            n++;
        }
        if (cls) SysFreeString(cls);
        IUIAutomationElement_Release(e);
    }
    if (arr) IUIAutomationElementArray_Release(arr);
    if (root) IUIAutomationElement_Release(root);

    int appsR = tr.left, trayL = tr.right, apps = 0;
    for (int i = 0; i < n; i++)
        if (app[i] && rc[i].right > appsR) { appsR = rc[i].right; apps = 1; }
    for (int i = 0; i < n; i++)
        if (!app[i] && rc[i].left >= appsR && rc[i].left < trayL) trayL = rc[i].left;
    HWND tn = FindWindowExW(tb, NULL, L"TrayNotifyWnd", NULL);
    RECT nr;
    if (tn && GetWindowRect(tn, &nr) && nr.right > nr.left && nr.left < trayL) trayL = nr.left;
    if ((primary && !apps) || trayL <= appsR) return 0;
    *outTray = trayL;
    *outApps = appsR;
    return 1;
}

void taskbar_uia_scan(void)
{
    if (!g_scanCsInit) return;
    HWND tbs[MAX_BARS];
    SCAN res[MAX_BARS];
    ZeroMemory(res, sizeof(res));
    HWND main = FindWindowW(L"Shell_TrayWnd", NULL);
    int n = find_taskbars(tbs, MAX_BARS), ok = uia_init();
    for (int i = 0; i < n; i++) {
        res[i].tb = tbs[i];
        res[i].trayL = res[i].appsR = 0;
        res[i].ok = ok && scan_one(tbs[i], tbs[i] == main, &res[i].trayL, &res[i].appsR);
    }
    EnterCriticalSection(&g_scanCs);
    ZeroMemory(g_scan, sizeof(g_scan));
    if (n > 0) memcpy(g_scan, res, sizeof(res[0]) * n);
    LeaveCriticalSection(&g_scanCs);
}

void taskbar_uia_cleanup(void)
{
    if (g_uiaAll)   { IUIAutomationCondition_Release(g_uiaAll); g_uiaAll = NULL; }
    if (g_uiaCache) { IUIAutomationCacheRequest_Release(g_uiaCache); g_uiaCache = NULL; }
    if (g_uia)      { IUIAutomation_Release(g_uia); g_uia = NULL; }
}

static int scan_for(HWND tb, int *trayL, int *appsR)
{
    int ok = 0;
    EnterCriticalSection(&g_scanCs);
    for (int i = 0; i < MAX_BARS; i++) {
        if (g_scan[i].tb == tb && g_scan[i].ok) {
            *trayL = g_scan[i].trayL;
            *appsR = g_scan[i].appsR;
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_scanCs);
    return ok;
}

/* ---- DPI, Fonts, and Drawing ---- */

static void get_dpi(void)
{
    typedef UINT (WINAPI *GDW)(HWND);
    static GDW f = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        f = (GDW)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    UINT d = (f && bar->tb) ? f(bar->tb) : 0;
    if (!d) { HDC dc = GetDC(NULL); d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    g_dpi = d < 96 ? 96 : (int)d;
}

static HFONT mkfont(HDC dc, const wchar_t *face, int px, int weight)
{
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
    wchar_t got[LF_FACESIZE] = {0};
    HGDIOBJ o = SelectObject(dc, f);
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    if (lstrcmpiW(got, face)) { DeleteObject(f); return NULL; }
    return f;
}

static const wchar_t REFRESH_GLYPH = 0xE72C;
static const MAT2 MAT_ID = { {0,1}, {0,0}, {0,0}, {0,1} };

static void bar_fonts(HDC dc)
{
    if (bar->font && bar->fontDpi == g_dpi) return;
    if (bar->font)   DeleteObject(bar->font);
    if (bar->bold)   DeleteObject(bar->bold);
    if (bar->smFont) DeleteObject(bar->smFont);
    if (bar->ico)    DeleteObject(bar->ico);

    int tp = SC(13) * SS, sp = SC(11) * SS, ip = SC(13) * SS;
    bar->font = mkfont(dc, L"Segoe UI Variable Text", tp, FW_NORMAL);
    if (!bar->font) bar->font = mkfont(dc, L"Segoe UI", tp, FW_NORMAL);
    if (!bar->font) bar->font = CreateFontW(-tp, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    bar->bold = mkfont(dc, L"Segoe UI Variable Text", tp, FW_SEMIBOLD);
    if (!bar->bold) bar->bold = mkfont(dc, L"Segoe UI", tp, FW_SEMIBOLD);
    if (!bar->bold) bar->bold = CreateFontW(-tp, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    bar->smFont = mkfont(dc, L"Segoe UI Variable Text", sp, FW_NORMAL);
    if (!bar->smFont) bar->smFont = mkfont(dc, L"Segoe UI", sp, FW_NORMAL);
    if (!bar->smFont) bar->smFont = CreateFontW(-sp, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");

    bar->ico = mkfont(dc, L"Segoe Fluent Icons", ip, FW_NORMAL);
    if (!bar->ico) bar->ico = mkfont(dc, L"Segoe MDL2 Assets", ip, FW_NORMAL);

    bar->icoOK = 0;
    if (bar->ico) {
        WORD gi = 0xFFFF;
        HGDIOBJ o = SelectObject(dc, bar->ico);
        if (GetGlyphIndicesW(dc, &REFRESH_GLYPH, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && gi != 0xFFFF)
            bar->icoOK = 1;
        SelectObject(dc, o);
    }
    bar->fontDpi = g_dpi;
}

static void ink(HDC dc, wchar_t ch, int *top, int *h, int *left, int *w)
{
    GLYPHMETRICS gm; TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    if (GetGlyphOutlineW(dc, ch, GGO_METRICS, &gm, 0, NULL, &MAT_ID) == GDI_ERROR) {
        *top = tm.tmInternalLeading; *h = tm.tmAscent - tm.tmInternalLeading; *left = 0; *w = tm.tmAveCharWidth;
        return;
    }
    *top = tm.tmAscent - gm.gmptGlyphOrigin.y;
    *h = gm.gmBlackBoxY;
    *left = gm.gmptGlyphOrigin.x;
    *w = gm.gmBlackBoxX;
}

static int text_w(HDC dc, const wchar_t *t)
{
    SIZE s = {0, 0};
    GetTextExtentPoint32W(dc, t, lstrlenW(t), &s);
    return s.cx;
}

static COLORREF lerp_color(COLORREF a, COLORREF b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f);
    return RGB(r, g, bl);
}

/* Vector Refresh circular arrow (also supports smooth rotation angle in degrees while spinning) */
static void draw_refresh_spinner(HDC dc, int x0, int y0, int sz, float angleDeg)
{
    int pw = sz / 10; if (pw < 2) pw = 2;
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    double cx = x0 + sz * 0.5, cy = y0 + sz * 0.5, rad = sz * 0.30;
    double a0 = angleDeg * 3.14159265 / 180.0;
    double a1 = a0 + 4.71238898; /* 270 degree arc */
    int sx = (int)(cx + cos(a0) * rad), sy = (int)(cy + sin(a0) * rad);
    int ex = (int)(cx + cos(a1) * rad), ey = (int)(cy + sin(a1) * rad);
    /* GDI Arc draws counter-clockwise from (ex,ey) to (sx,sy) -> clockwise sweep from a0 to a1 */
    Arc(dc, (int)(cx - rad), (int)(cy - rad), (int)(cx + rad), (int)(cy + rad), ex, ey, sx, sy);
    /* Small arrowhead at leading tip (a1) */
    double tx = -sin(a1), ty = cos(a1); /* tangent */
    double nx = cos(a1),  ny = sin(a1); /* outward normal */
    double hl = sz * 0.16;
    MoveToEx(dc, ex, ey, NULL);
    LineTo(dc, (int)(ex - tx * hl + nx * hl * 0.65), (int)(ey - ty * hl + ny * hl * 0.65));
    MoveToEx(dc, ex, ey, NULL);
    LineTo(dc, (int)(ex - tx * hl - nx * hl * 0.65), (int)(ey - ty * hl - ny * hl * 0.65));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

static void bar_to_popup(void)
{
    SetParent(bar->wnd, NULL);
    SetWindowLongPtrW(bar->wnd, GWL_STYLE, WS_POPUP);
    SetWindowLongPtrW(bar->wnd, GWL_EXSTYLE, GetWindowLongPtrW(bar->wnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
    bar->child = 0;
    ZeroMemory(&bar->lastPos, sizeof(bar->lastPos));
}

static void bar_place(void)
{
    RECT tr, nr;
    if (!bar->wnd) return;
    if (!bar->tb || !GetWindowRect(bar->tb, &tr) || tr.bottom - tr.top > tr.right - tr.left) {
        ShowWindow(bar->wnd, SW_HIDE);
        return;
    }
    get_dpi();
    int trayL, appsR, scan = scan_for(bar->tb, &trayL, &appsR);
    if (!scan) {
        HWND tn = FindWindowExW(bar->tb, NULL, bar->primary ? L"TrayNotifyWnd" : L"ClockButton", NULL);
        if (tn && GetWindowRect(tn, &nr) && nr.right > nr.left) trayL = nr.left;
        else trayL = tr.right - (bar->primary ? SC(280) : SC(12));
        appsR = tr.left;
    }
    int h = tr.bottom - tr.top;
    int right = trayL - SC(8) + SC(g_cfg.offsetX);
    int avail = right - (scan ? appsR + SC(6) : tr.left);
    if (avail < SC(120)) avail = SC(120);
    if (avail > BAR_MAX_W) avail = BAR_MAX_W;
    int availChanged = (abs(avail - bar->availW) > SC(16));
    bar->availW = avail;
    int compact = bar->compact ? (bar->wideW > avail - SC(10)) : (bar->wideW > avail);
    if (compact != bar->compact || availChanged) {
        bar->compact = compact;
        bar->sig[0] = 0;
        PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
    }
    int x = right - bar->W, y = tr.top;
    if (bar->child) { POINT p = { x, y }; ScreenToClient(bar->tb, &p); x = p.x; y = p.y; }
    RECT want = { x, y, x + bar->W, y + h };
    int top = bar->child ? (GetWindow(bar->tb, GW_CHILD) == bar->wnd) : 1;
    if (memcmp(&want, &bar->lastPos, sizeof(want)) || !top || !IsWindowVisible(bar->wnd)) {
        SetWindowPos(bar->wnd, bar->child ? HWND_TOP : HWND_TOPMOST, x, y, bar->W, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        bar->lastPos = want;
    } else if (!bar->child) {
        SetWindowPos(bar->wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static int layout_segs(HDC dc, SEG *sg, int n, int compact, int HB, HitBox *hits, int *hitCount)
{
    int btnSz = SC(22) * SS;
    int top, h, d1, d2;
    int X = SC(4) * SS;
    int hc = 0;

    if (!compact) {
        SelectObject(dc, bar->font);
        ink(dc, L'0', &top, &h, &d1, &d2);
        int curCoin = -1, coinStartX = 0;

        for (int i = 0; i < n; i++) {
            if (sg[i].isRefresh) {
                if (i > 0) X += SC(6) * SS;
                sg[i].x = X;
                sg[i].y = (HB - btnSz) / 2;
                sg[i].w = btnSz;
                sg[i].row = -1;
                if (hits && hc < MAX_HITS) {
                    hits[hc].kind = HIT_REFRESH;
                    hits[hc].coinIdx = -1;
                    hits[hc].rc.left   = X / SS;
                    hits[hc].rc.top    = 0;
                    hits[hc].rc.right  = (X + btnSz) / SS;
                    hits[hc].rc.bottom = HB / SS;
                    hc++;
                }
                X += btnSz + SC(6) * SS;
                continue;
            }
            if (sg[i].isPage) {
                SelectObject(dc, bar->smFont);
                int ptop, ph, pd1, pd2;
                ink(dc, L'0', &ptop, &ph, &pd1, &pd2);
                sg[i].w = text_w(dc, sg[i].t);
                sg[i].x = X;
                sg[i].y = HB / 2 - (ptop + ph / 2);
                sg[i].smallFont = 1;
                sg[i].row = -1;
                if (hits && hc < MAX_HITS) {
                    hits[hc].kind = HIT_PAGE;
                    hits[hc].coinIdx = -1;
                    hits[hc].rc.left   = (X / SS) - SC(2);
                    hits[hc].rc.top    = 0;
                    hits[hc].rc.right  = ((X + sg[i].w) / SS) + SC(3);
                    hits[hc].rc.bottom = HB / SS;
                    hc++;
                }
                X += sg[i].w + SC(10) * SS;
                continue;
            }

            int newCoin = (sg[i].coinIdx != curCoin);
            if (newCoin) {
                if (curCoin >= 0 && hits && hc < MAX_HITS) {
                    hits[hc].kind = HIT_COIN;
                    hits[hc].coinIdx = curCoin;
                    hits[hc].rc.left   = coinStartX / SS;
                    hits[hc].rc.top    = 0;
                    hits[hc].rc.right  = X / SS;
                    hits[hc].rc.bottom = HB / SS;
                    hc++;
                }
                if (curCoin >= 0) X += SC(14) * SS;
                curCoin = sg[i].coinIdx;
                coinStartX = X;
            } else {
                X += SC(5) * SS;
            }

            SelectObject(dc, sg[i].isLabel ? bar->bold : bar->font);
            sg[i].w = text_w(dc, sg[i].t);
            sg[i].x = X;
            sg[i].y = HB / 2 - (top + h / 2);
            sg[i].smallFont = 0;
            sg[i].row = -1;
            X += sg[i].w;
        }
        if (curCoin >= 0 && hits && hc < MAX_HITS) {
            hits[hc].kind = HIT_COIN;
            hits[hc].coinIdx = curCoin;
            hits[hc].rc.left   = coinStartX / SS;
            hits[hc].rc.top    = 0;
            hits[hc].rc.right  = X / SS;
            hits[hc].rc.bottom = HB / SS;
            hc++;
        }
        if (hitCount) *hitCount = hc;
        return X + SC(6) * SS;
    }

    /* Compact 2-row mode: Top = Symbol (+optional pct), Bottom = Price */
    SelectObject(dc, bar->smFont);
    ink(dc, L'0', &top, &h, &d1, &d2);
    int gap = SC(3) * SS;
    int yTop = HB / 2 - gap / 2 - h - top;
    int yBot = HB / 2 + gap / 2 - top;
    int yMid = HB / 2 - h / 2 - top;

    for (int i = 0; i < n;) {
        if (sg[i].isRefresh) {
            sg[i].x = X;
            sg[i].y = (HB - btnSz) / 2;
            sg[i].w = btnSz;
            sg[i].row = -1;
            if (hits && hc < MAX_HITS) {
                hits[hc].kind = HIT_REFRESH;
                hits[hc].coinIdx = -1;
                hits[hc].rc.left   = X / SS;
                hits[hc].rc.top    = 0;
                hits[hc].rc.right  = (X + btnSz) / SS;
                hits[hc].rc.bottom = HB / SS;
                hc++;
            }
            X += btnSz + SC(6) * SS;
            i++;
            continue;
        }
        if (sg[i].isPage) {
            SelectObject(dc, bar->smFont);
            sg[i].w = text_w(dc, sg[i].t);
            sg[i].x = X;
            sg[i].y = yMid;
            sg[i].smallFont = 1;
            sg[i].row = -1;
            if (hits && hc < MAX_HITS) {
                hits[hc].kind = HIT_PAGE;
                hits[hc].coinIdx = -1;
                hits[hc].rc.left   = (X / SS) - SC(2);
                hits[hc].rc.top    = 0;
                hits[hc].rc.right  = ((X + sg[i].w) / SS) + SC(3);
                hits[hc].rc.bottom = HB / SS;
                hc++;
            }
            X += sg[i].w + SC(9) * SS;
            i++;
            continue;
        }

        int cid = sg[i].coinIdx;
        int startX = X;
        int a = i;
        int b = (i + 1 < n && !sg[i + 1].isRefresh && sg[i + 1].coinIdx == cid) ? i + 1 : -1;
        int c = (b >= 0 && b + 1 < n && !sg[b + 1].isRefresh && sg[b + 1].coinIdx == cid) ? b + 1 : -1;

        SelectObject(dc, bar->smFont);
        int wa = text_w(dc, sg[a].t);
        int wb = (b >= 0) ? text_w(dc, sg[b].t) : 0;
        int wc = (c >= 0) ? text_w(dc, sg[c].t) : 0;
        int topW = wa + (c >= 0 ? SC(4) * SS + wc : 0);
        int colW = topW > wb ? topW : wb;

        sg[a].x = X; sg[a].w = wa; sg[a].smallFont = 1;
        if (b >= 0) {
            sg[a].y = yTop; sg[a].row = 0;
            sg[b].x = X; sg[b].w = wb; sg[b].smallFont = 1; sg[b].y = yBot; sg[b].row = 1;
            if (c >= 0) {
                sg[c].x = X + wa + SC(4) * SS; sg[c].w = wc; sg[c].smallFont = 1; sg[c].y = yTop; sg[c].row = 0;
            }
        } else {
            sg[a].y = yMid; sg[a].row = -1;
        }

        X += colW;
        if (cid >= 0 && hits && hc < MAX_HITS) {
            hits[hc].kind = HIT_COIN;
            hits[hc].coinIdx = cid;
            hits[hc].rc.left   = startX / SS;
            hits[hc].rc.top    = 0;
            hits[hc].rc.right  = X / SS;
            hits[hc].rc.bottom = HB / SS;
            hc++;
        }
        i = (c >= 0) ? (c + 1) : (b >= 0 ? (b + 1) : (a + 1));
        if (i < n) X += SC(11) * SS;
    }

    if (hitCount) *hitCount = hc;
    return X + SC(6) * SS;
}

static int build_coin_segs(const AppConfig *snap, const int *enIdx, int start, int end,
                           int page, int pageCount, int fetching,
                           COLORREF fg, COLORREF cUp, COLORREF cDown, COLORREF cAcc, COLORREF cMuted,
                           ULONGLONG now, SEG *sg)
{
    int n = 0;
    if (snap->showRefreshIcon && n < MAX_SEGS) {
        ZeroMemory(&sg[n], sizeof(sg[n]));
        sg[n].isRefresh = 1;
        sg[n].coinIdx   = -1;
        sg[n].c         = fetching ? cAcc : fg;
        sg[n].al        = (bar->hoverKind == HIT_REFRESH || fetching) ? 255 : 195;
        n++;
    }
    if (pageCount > 1 && n < MAX_SEGS) {
        ZeroMemory(&sg[n], sizeof(sg[n]));
        sg[n].isPage  = 1;
        sg[n].coinIdx = -1;
        sg[n].c       = (bar->hoverKind == HIT_PAGE) ? cAcc : cMuted;
        sg[n].al      = (bar->hoverKind == HIT_PAGE) ? 255 : 210;
        _snwprintf(sg[n].t, 32, L"%d/%d", page + 1, pageCount);
        n++;
    }

    for (int e = start; e < end && n + 3 < MAX_SEGS; e++) {
        int i = enIdx[e];
        const CoinItem *c = &snap->coins[i];
        wchar_t lbl[24], ptxt[32], pctTxt[16];
        provider_default_label(c, lbl, 24);

        /* Compute smooth 500ms tick flash blend factor */
        float flashT = 0.0f;
        if (snap->enableTickFlash && c->flashDir != 0 && c->flashStartMs > 0 && now >= c->flashStartMs) {
            ULONGLONG dt = now - c->flashStartMs;
            if (dt < 500ULL) flashT = 1.0f - (float)dt / 500.0f;
        }
        COLORREF flashCol = (c->flashDir > 0) ? cUp : cDown;

        /* Label segment */
        ZeroMemory(&sg[n], sizeof(sg[n]));
        sg[n].coinIdx = i;
        sg[n].isLabel = 1;
        sg[n].c = fg;
        sg[n].al = 190;
        lstrcpynW(sg[n].t, lbl, 32);
        n++;

        /* Price segment */
        ZeroMemory(&sg[n], sizeof(sg[n]));
        sg[n].coinIdx = i;
        if (c->ok) {
            format_price(c->price, c->decimals, snap->showCurrencySign, snap->compactPrices, ptxt, 32);
            COLORREF baseC = fg;
            if (snap->colorByChange && c->hasChange24h && !snap->showChangePct)
                baseC = (c->change24h >= 0.0) ? cUp : cDown;
            sg[n].c = (flashT > 0.01f) ? lerp_color(baseC, flashCol, flashT) : baseC;
            sg[n].al = 255;
        } else {
            lstrcpyW(ptxt, c->fetching ? L"..." : L"--");
            sg[n].c = cMuted;
            sg[n].al = 180;
        }
        lstrcpynW(sg[n].t, ptxt, 32);
        n++;

        /* Optional 24h % segment */
        if (snap->showChangePct && c->ok && c->hasChange24h) {
            format_pct(c->change24h, pctTxt, 16);
            ZeroMemory(&sg[n], sizeof(sg[n]));
            sg[n].coinIdx = i;
            sg[n].c = snap->colorByChange ? ((c->change24h >= 0.0) ? cUp : cDown) : cMuted;
            sg[n].al = 235;
            lstrcpynW(sg[n].t, pctTxt, 32);
            n++;
        }
    }

    if (end <= start && n < MAX_SEGS) {
        ZeroMemory(&sg[n], sizeof(sg[n]));
        sg[n].coinIdx = -1;
        sg[n].isLabel = 1;
        sg[n].c = fg;
        sg[n].al = 210;
        lstrcpynW(sg[n].t, L"TrayCoin", 32);
        n++;
    }
    return n;
}

static void bar_draw(int force)
{
    static COLORREF colOf[2][BAR_MAX_W];
    static BYTE     alOf[2][BAR_MAX_W];
    static BYTE     lut[256];
    if (!lut[255]) for (int i = 0; i < 256; i++) lut[i] = (BYTE)(255.0 * pow(i / 255.0, 0.8) + 0.5);
    if (!bar->wnd) return;

    RECT tr; int H = 40;
    if (bar->tb && GetWindowRect(bar->tb, &tr)) H = tr.bottom - tr.top;
    if (H < 16) H = 16;
    if (H > 200) H = 200;
    get_dpi();

    COLORREF fg     = g_light ? RGB(16, 16, 16)   : RGB(255, 255, 255);
    COLORREF cUp    = g_light ? RGB(14, 138, 60)  : RGB(72, 218, 128);
    COLORREF cDown  = g_light ? RGB(200, 35, 35)  : RGB(255, 96, 96);
    COLORREF cAcc   = g_light ? RGB(0, 103, 192)  : RGB(90, 195, 255);
    COLORREF cMuted = g_light ? RGB(95, 95, 95)   : RGB(180, 185, 195);

    /* Snapshot config & prices safely (static to keep UI stack small) */
    static AppConfig snap;
    EnterCriticalSection(&g_stateCs);
    snap = g_cfg;
    LeaveCriticalSection(&g_stateCs);
    int fetching = (g_anyFetching != 0);
    ULONGLONG now = GetTickCount64();
    int spinStep = fetching ? (int)((now / 55ULL) % 12ULL) : 0;

    int enIdx[MAX_COINS], enCount = 0;
    for (int i = 0; i < snap.coinCount; i++)
        if (snap.coins[i].enabled) enIdx[enCount++] = i;

    HDC mdc = CreateCompatibleDC(NULL);
    bar_fonts(mdc);
    int HB = H * SS;
    HGDIOBJ of = SelectObject(mdc, bar->font);

    static SEG sg[MAX_SEGS];
    int n = build_coin_segs(&snap, enIdx, 0, enCount, 0, 1, fetching, fg, cUp, cDown, cAcc, cMuted, now, sg);
    int fullWideW = (layout_segs(mdc, sg, n, 0, HB, NULL, NULL) + SS - 1) / SS;
    int fullCompW = (layout_segs(mdc, sg, n, 1, HB, NULL, NULL) + SS - 1) / SS;
    bar->wideW = fullWideW;

    /* Auto-paginate if even compact 2-row mode exceeds available taskbar width */
    int maxAvail = (bar->availW > SC(140)) ? (bar->availW - SC(6)) : BAR_MAX_W;
    if (enCount > 1 && fullCompW > maxAvail) {
        int pStarts[MAX_COINS + 1];
        int pc = 0, pos = 0;
        while (pos < enCount && pc < MAX_COINS) {
            pStarts[pc++] = pos;
            int end = pos + 1;
            while (end < enCount) {
                int tn = build_coin_segs(&snap, enIdx, pos, end + 1, 0, 2, fetching, fg, cUp, cDown, cAcc, cMuted, now, sg);
                int tw = (layout_segs(mdc, sg, tn, bar->compact ? 1 : 0, HB, NULL, NULL) + SS - 1) / SS;
                if (tw > maxAvail) break;
                end++;
            }
            pos = end;
        }
        pStarts[pc] = enCount;
        bar->pageCount = pc > 0 ? pc : 1;
        if (bar->page >= bar->pageCount) bar->page = 0;
        if (bar->page < 0) bar->page = bar->pageCount - 1;
        n = build_coin_segs(&snap, enIdx, pStarts[bar->page], pStarts[bar->page + 1],
                            bar->page, bar->pageCount, fetching, fg, cUp, cDown, cAcc, cMuted, now, sg);
    } else {
        bar->pageCount = 1;
        bar->page = 0;
    }

    /* Build render signature to skip redundant redraws */
    wchar_t sig[1024];
    int p = _snwprintf(sig, 1024, L"%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|",
                       H, g_dpi, g_light, n, bar->compact, fetching, spinStep,
                       (int)bar->hoverKind, bar->hoverCoinIdx, bar->page, bar->pageCount);
    for (int i = 0; i < n && p > 0 && p < 960; i++) {
        if (!sg[i].isRefresh)
            p += _snwprintf(sig + p, 1024 - p, L"%s%06X|", sg[i].t, (unsigned)sg[i].c);
    }
    sig[1023] = 0;
    if (!force && !lstrcmpW(sig, bar->sig)) {
        SelectObject(mdc, of);
        DeleteDC(mdc);
        bar_place();
        return;
    }
    lstrcpyW(bar->sig, sig);

    HitBox tmpHits[MAX_HITS];
    int tmpHitCount = 0;
    int W = (layout_segs(mdc, sg, n, bar->compact ? 1 : 0, HB, tmpHits, &tmpHitCount) + SS - 1) / SS;
    if (W < 32) W = 32;
    if (W > BAR_MAX_W) W = BAR_MAX_W;
    int WB = W * SS;

    bar->hitCount = tmpHitCount;
    if (tmpHitCount > 0) memcpy(bar->hits, tmpHits, sizeof(HitBox) * tmpHitCount);

    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = WB; bi.bmiHeader.biHeight = -HB;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD *bp = NULL, *px = NULL;
    HBITMAP big = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&bp, NULL, 0);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!big || !bmp) {
        if (big) DeleteObject(big);
        if (bmp) DeleteObject(bmp);
        SelectObject(mdc, of); DeleteDC(mdc); return;
    }
    HGDIOBJ ob = SelectObject(mdc, big);
    ZeroMemory(bp, (SIZE_T)WB * HB * 4);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(255, 255, 255));

    for (int r = 0; r < 2; r++)
        for (int i = 0; i < W; i++) { colOf[r][i] = fg; alOf[r][i] = 255; }

    /* Draw text segments */
    for (int i = 0; i < n; i++) {
        if (sg[i].isRefresh) continue;
        SelectObject(mdc, sg[i].smallFont ? bar->smFont : (sg[i].isLabel ? bar->bold : bar->font));
        TextOutW(mdc, sg[i].x, sg[i].y, sg[i].t, lstrlenW(sg[i].t));
        for (int r = 0; r < 2; r++) {
            if (sg[i].row >= 0 && sg[i].row != r) continue;
            for (int k = sg[i].x / SS - 1; k <= (sg[i].x + sg[i].w) / SS + 1 && k < W; k++) {
                if (k >= 0) { colOf[r][k] = sg[i].c; alOf[r][k] = (BYTE)sg[i].al; }
            }
        }
    }

    /* Draw Refresh icon segment (animated rotating spinner while fetching!) */
    for (int i = 0; i < n; i++) {
        if (!sg[i].isRefresh) continue;
        if (fetching) {
            draw_refresh_spinner(mdc, sg[i].x, sg[i].y, sg[i].w, (float)(spinStep * 30));
        } else if (bar->icoOK && bar->ico) {
            SelectObject(mdc, bar->ico);
            int top, h, left, w;
            ink(mdc, REFRESH_GLYPH, &top, &h, &left, &w);
            TextOutW(mdc, sg[i].x + (sg[i].w - w) / 2 - left, HB / 2 - (top + h / 2), &REFRESH_GLYPH, 1);
        } else {
            draw_refresh_spinner(mdc, sg[i].x, sg[i].y, sg[i].w, -45.0f);
        }
        for (int r = 0; r < 2; r++) {
            for (int k = sg[i].x / SS - 1; k <= (sg[i].x + sg[i].w) / SS + 1 && k < W; k++) {
                if (k >= 0) { colOf[r][k] = sg[i].c; alOf[r][k] = (BYTE)sg[i].al; }
            }
        }
    }
    GdiFlush();

    /* Find hover highlight rect (1x coords) */
    int hasHoverBg = 0;
    RECT hoverRc = {0, 0, 0, 0};
    if (bar->hoverKind == HIT_REFRESH || bar->hoverKind == HIT_PAGE || bar->hoverKind == HIT_COIN) {
        for (int i = 0; i < bar->hitCount; i++) {
            if (bar->hits[i].kind == bar->hoverKind &&
                (bar->hoverKind != HIT_COIN || bar->hits[i].coinIdx == bar->hoverCoinIdx)) {
                hoverRc = bar->hits[i].rc;
                hoverRc.top = SC(4);
                hoverRc.bottom = H - SC(4);
                if (hoverRc.bottom <= hoverRc.top) { hoverRc.top = 1; hoverRc.bottom = H - 1; }
                hasHoverBg = 1;
                break;
            }
        }
    }

    /* Downsample 4x SS to 1x premultiplied ARGB */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned sum = 0;
            for (int j = 0; j < SS; j++) {
                const DWORD *row = bp + (y * SS + j) * WB + x * SS;
                for (int i = 0; i < SS; i++) sum += row[i] & 0xFF;
            }
            int r = (y >= H / 2) ? 1 : 0;
            DWORD a = lut[sum / (SS * SS)] * alOf[r][x] / 255;
            COLORREF c = colOf[r][x];

            /* Optional subtle pill highlight behind hovered button/coin */
            DWORD bgA = 0;
            if (hasHoverBg && x >= hoverRc.left && x < hoverRc.right && y >= hoverRc.top && y < hoverRc.bottom) {
                /* Round 1px corners */
                int corner = ((x == hoverRc.left || x == hoverRc.right - 1) &&
                              (y == hoverRc.top || y == hoverRc.bottom - 1));
                if (!corner) bgA = g_light ? 22 : 34;
            }

            if (a > 0 || bgA > 0) {
                DWORD fgR = GetRValue(c), fgG = GetGValue(c), fgB = GetBValue(c);
                DWORD bgC = g_light ? 0 : 255;
                /* Porter-Duff source-over: fg over bg */
                DWORD invA = 255 - a;
                DWORD outA = a + (bgA * invA) / 255;
                DWORD outR = (fgR * a) / 255 + (bgC * bgA * invA) / (255 * 255);
                DWORD outG = (fgG * a) / 255 + (bgC * bgA * invA) / (255 * 255);
                DWORD outB = (fgB * a) / 255 + (bgC * bgA * invA) / (255 * 255);
                if (outA > 255) outA = 255;
                if (outR > outA) outR = outA;
                if (outG > outA) outG = outA;
                if (outB > outA) outB = outA;
                px[y * W + x] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
            } else {
                /* alpha=1 keeps window hit-testable for mouse clicks across gaps */
                px[y * W + x] = 0x01000000;
            }
        }
    }

    SelectObject(mdc, bmp);
    DeleteObject(big);

    POINT src = { 0, 0 };
    SIZE sz = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    bar->W = W;
    bar->H = H;
    if (!UpdateLayeredWindow(bar->wnd, NULL, NULL, &sz, mdc, &src, 0, &bf, ULW_ALPHA) && bar->child) {
        bar_to_popup();
        UpdateLayeredWindow(bar->wnd, NULL, NULL, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    }

    SelectObject(mdc, ob);
    SelectObject(mdc, of);
    DeleteObject(bmp);
    DeleteDC(mdc);
    bar_place();
}

/* ---- Tooltip & Hit-test helpers ---- */

static BAR *bar_from_hwnd(HWND w)
{
    for (int i = 0; i < MAX_BARS; i++)
        if (g_bars[i].wnd == w) return &g_bars[i];
    return NULL;
}

static HitBox *hit_test(BAR *b, int x, int y)
{
    for (int i = 0; i < b->hitCount; i++) {
        if (x >= b->hits[i].rc.left && x < b->hits[i].rc.right &&
            y >= b->hits[i].rc.top  && y < b->hits[i].rc.bottom) {
            return &b->hits[i];
        }
    }
    return NULL;
}

static void update_bar_tooltip(BAR *b)
{
    if (!b || !b->tip) return;
    wchar_t txt[256] = {0};

    EnterCriticalSection(&g_stateCs);
    if (b->hoverKind == HIT_REFRESH) {
        _snwprintf(txt, 256, L"Click to refresh all coins now (Auto: %lus)", (unsigned long)g_cfg.globalIntervalSec);
    } else if (b->hoverKind == HIT_PAGE) {
        _snwprintf(txt, 256, L"Page %d of %d — Click or scroll mouse wheel to switch", b->page + 1, b->pageCount);
    } else if (b->hoverKind == HIT_COIN && b->hoverCoinIdx >= 0 && b->hoverCoinIdx < g_cfg.coinCount) {
        const CoinItem *c = &g_cfg.coins[b->hoverCoinIdx];
        wchar_t lbl[24], pstr[32], pct[16] = {0};
        provider_default_label(c, lbl, 24);
        const wchar_t *src = c->resolvedSource[0] ? c->resolvedSource : provider_display_name(c->provider);
        if (c->ok) {
            format_price(c->price, -1, 1, 0, pstr, 32);
            if (c->hasChange24h) format_pct(c->change24h, pct, 16);
            wchar_t extra[96] = L"";
            if (c->holdingQty > 0.0) {
                wchar_t vstr[32];
                format_price(c->price * c->holdingQty, 2, 1, 0, vstr, 32);
                _snwprintf(extra, 96, L"  |  Hold: %s", vstr);
            }
            _snwprintf(txt, 256, L"%s (%s%s): %s %s%s  [%02u:%02u:%02u]",
                       lbl, src, c->wsLive ? L" WS" : L"", pstr, pct, extra,
                       c->lastUpdateTime.wHour, c->lastUpdateTime.wMinute, c->lastUpdateTime.wSecond);
        } else {
            _snwprintf(txt, 256, L"%s (%s): %s", lbl, src, c->lastError[0] ? c->lastError : L"Loading...");
        }
    } else {
        lstrcpynW(txt, L"TrayCoin — Right-click for settings", 256);
    }
    LeaveCriticalSection(&g_stateCs);
    txt[255] = 0;

    if (lstrcmpW(txt, b->tipText) != 0) {
        lstrcpynW(b->tipText, txt, 256);
        TTTOOLINFOW ti; ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd = b->wnd;
        ti.uId = (UINT_PTR)b->wnd;
        ti.lpszText = b->tipText;
        SendMessageW(b->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

static LRESULT CALLBACK barproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    BAR *b = bar_from_hwnd(w);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (b && (b->hoverKind == HIT_REFRESH || b->hoverKind == HIT_PAGE || b->hoverKind == HIT_COIN)) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE:
        if (b) {
            if (!b->trackingMouse) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, w, 0 };
                TrackMouseEvent(&tme);
                b->trackingMouse = 1;
            }
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            HitBox *hb = hit_test(b, x, y);
            HitKind nk = hb ? hb->kind : HIT_NONE;
            int nidx = hb ? hb->coinIdx : -1;
            if (nk != b->hoverKind || nidx != b->hoverCoinIdx) {
                b->hoverKind = nk;
                b->hoverCoinIdx = nidx;
                update_bar_tooltip(b);
                bar = b;
                bar_draw(0);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (b) {
            b->trackingMouse = 0;
            if (b->hoverKind != HIT_NONE) {
                b->hoverKind = HIT_NONE;
                b->hoverCoinIdx = -1;
                bar = b;
                bar_draw(0);
            }
        }
        return 0;
    case WM_RBUTTONUP:
        taskbar_show_menu();
        return 0;
    case WM_LBUTTONUP:
        if (b) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            HitBox *hb = hit_test(b, x, y);
            if (hb && hb->kind == HIT_REFRESH) {
                LOGI(L"TASKBAR", L"Refresh icon clicked on taskbar");
                poller_trigger_refresh(0);
                return 0;
            }
            if (hb && hb->kind == HIT_PAGE) {
                if (b->pageCount > 1) {
                    b->page = (b->page + 1) % b->pageCount;
                    b->lastRotateMs = GetTickCount64();
                    bar = b;
                    bar_draw(1);
                }
                return 0;
            }
            if (hb && hb->kind == HIT_COIN) {
                wchar_t url[256] = {0};
                CoinClickAction act = CLICK_OPEN_CHART;
                EnterCriticalSection(&g_stateCs);
                act = g_cfg.clickAction;
                if (hb->coinIdx >= 0 && hb->coinIdx < g_cfg.coinCount)
                    lstrcpynW(url, g_cfg.coins[hb->coinIdx].pairUrl, 256);
                LeaveCriticalSection(&g_stateCs);

                if (act == CLICK_REFRESH) {
                    poller_trigger_refresh(0);
                } else if (act == CLICK_OPEN_CHART && url[0]) {
                    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
                } else {
                    ui_settings_open();
                }
                return 0;
            }
            ui_settings_open();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (b && b->pageCount > 1) {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);
            b->page = (b->page + (delta < 0 ? 1 : (b->pageCount - 1))) % b->pageCount;
            b->lastRotateMs = GetTickCount64();
            bar = b;
            bar_draw(1);
            return 0;
        }
        return 0;
    case WM_NCDESTROY:
        if (b) {
            if (b->tip && IsWindow(b->tip)) DestroyWindow(b->tip);
            b->tip = NULL;
            b->wnd = NULL;
        }
        break;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void bar_create(HWND tb, int primary)
{
    if (bar->font)  DeleteObject(bar->font);
    if (bar->bold)  DeleteObject(bar->bold);
    if (bar->smFont) DeleteObject(bar->smFont);
    if (bar->ico)   DeleteObject(bar->ico);
    ZeroMemory(bar, sizeof(*bar));
    bar->tb = tb;
    bar->primary = primary;
    bar->hoverCoinIdx = -1;

    bar->wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"TrayCoinBar", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, g_hinst, NULL);
    if (bar->wnd) {
        SetWindowLongPtrW(bar->wnd, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
        if (SetParent(bar->wnd, tb)) bar->child = 1;
        else SetWindowLongPtrW(bar->wnd, GWL_STYLE, WS_POPUP);

        bar->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                   WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   g_wnd, NULL, g_hinst, NULL);
        if (bar->tip) {
            TTTOOLINFOW ti; ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = bar->wnd;
            ti.uId = (UINT_PTR)bar->wnd;
            ti.lpszText = L"TrayCoin";
            SendMessageW(bar->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(bar->tip, TTM_SETMAXTIPWIDTH, 0, 400);
        }
    }
}

static void bar_destroy(BAR *b)
{
    HWND w = b->wnd;
    b->wnd = NULL;
    if (b->tip && IsWindow(b->tip)) DestroyWindow(b->tip);
    if (w && IsWindow(w)) DestroyWindow(w);
    if (b->font)  DeleteObject(b->font);
    if (b->bold)  DeleteObject(b->bold);
    if (b->smFont) DeleteObject(b->smFont);
    if (b->ico)   DeleteObject(b->ico);
    ZeroMemory(b, sizeof(*b));
}

static void bars_destroy(void)
{
    for (int i = 0; i < MAX_BARS; i++) bar_destroy(&g_bars[i]);
}

static int wanted_tb(HWND tb, HWND main, int others)
{
    if (g_cfg.showOn == SHOW_ALL) return 1;
    if (g_cfg.showOn == SHOW_OTHERS && others) return tb != main;
    return tb == main;
}

/* ---- Context Menu ---- */

void taskbar_show_menu(void)
{
    static const DWORD IVS[6] = { 5, 10, 15, 30, 60, 300 };
    static const wchar_t *IV_NAMES[6] = { L"5 seconds", L"10 seconds", L"15 seconds", L"30 seconds", L"60 seconds", L"5 minutes" };

    HMENU h    = CreatePopupMenu();
    HMENU mIv  = CreatePopupMenu();
    HMENU mDis = CreatePopupMenu();
    HMENU mOn  = CreatePopupMenu();
    HMENU mWdg = CreatePopupMenu();
    HMENU mOp  = CreatePopupMenu();
    HMENU mRot = CreatePopupMenu();

    AppendMenuW(h, MF_STRING, 10, L"Refresh Now\tCtrl+Shift+R");
    AppendMenuW(h, MF_STRING, 11, L"Manage Coins && Settings...");
    AppendMenuW(h, MF_SEPARATOR, 0, NULL);

    EnterCriticalSection(&g_stateCs);
    int ccount = g_cfg.coinCount;
    HMENU coinTarget = h;
    HMENU mCoins = NULL;
    if (ccount > 16) {
        mCoins = CreatePopupMenu();
        coinTarget = mCoins;
    }
    for (int i = 0; i < ccount && i < MAX_COINS; i++) {
        const CoinItem *c = &g_cfg.coins[i];
        wchar_t lbl[24], ptxt[32], item[96];
        provider_default_label(c, lbl, 24);
        if (c->ok) {
            format_price(c->price, c->decimals, 1, 0, ptxt, 32);
            _snwprintf(item, 96, L"%s  (%s)", lbl, ptxt);
        } else {
            _snwprintf(item, 96, L"%s  (%s)", lbl, provider_display_name(c->provider));
        }
        item[95] = 0;
        UINT flags = MF_STRING | (c->enabled ? MF_CHECKED : 0);
        if (mCoins && i > 0 && (i % 20) == 0) flags |= MF_MENUBARBREAK;
        AppendMenuW(coinTarget, flags, 100 + i, item);
    }
    if (mCoins) {
        wchar_t clbl[48];
        _snwprintf(clbl, 48, L"Watchlist (%d coins)", ccount);
        AppendMenuW(h, MF_POPUP, (UINT_PTR)mCoins, clbl);
    }
    DWORD curIv = g_cfg.globalIntervalSec;
    int wsOn    = g_cfg.enableWebSocket;
    int showPct = g_cfg.showChangePct;
    int colChg  = g_cfg.colorByChange;
    int showRef = g_cfg.showRefreshIcon;
    int compPr  = g_cfg.compactPrices;
    int showTb  = g_cfg.showTaskbarBar;
    int showWg  = g_cfg.showWidget;
    WidgetLayout wlay = g_cfg.widgetLayout;
    int wTop    = g_cfg.widgetTopmost;
    int wLock   = g_cfg.widgetLocked;
    int wCT     = g_cfg.widgetClickThrough;
    int wOp     = g_cfg.widgetOpacity;
    int flashOn = g_cfg.enableTickFlash;
    int spkOn   = g_cfg.showSparkline;
    DWORD rotIv = g_cfg.autoRotateSec;
    ShowWhere sw = g_cfg.showOn;
    LeaveCriticalSection(&g_stateCs);

    if (ccount > 0) AppendMenuW(h, MF_SEPARATOR, 0, NULL);

    /* Floating Widget submenu */
    AppendMenuW(mWdg, MF_STRING | (showWg ? MF_CHECKED : 0), 400, L"Show Floating Widget\tCtrl+Shift+C");
    AppendMenuW(mWdg, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mWdg, MF_STRING | (wlay == WIDGET_VERTICAL   ? MF_CHECKED : 0), 401, L"Vertical Card");
    AppendMenuW(mWdg, MF_STRING | (wlay == WIDGET_HORIZONTAL ? MF_CHECKED : 0), 402, L"Horizontal Pill");
    AppendMenuW(mWdg, MF_STRING | (spkOn ? MF_CHECKED : 0), 406, L"Show Mini Sparklines");
    AppendMenuW(mWdg, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mWdg, MF_STRING | (wTop  ? MF_CHECKED : 0), 403, L"Always on Top");
    AppendMenuW(mWdg, MF_STRING | (wLock ? MF_CHECKED : 0), 404, L"Lock Position");
    AppendMenuW(mWdg, MF_STRING | (wCT   ? MF_CHECKED : 0), 405, L"Click-Through (Pass Mouse)\tCtrl+Shift+L");
    static const int OPS[5] = { 40, 60, 75, 88, 100 };
    for (int i = 0; i < 5; i++) {
        wchar_t ol[16]; _snwprintf(ol, 16, L"%d%%", OPS[i]);
        AppendMenuW(mOp, MF_STRING | (wOp == OPS[i] ? MF_CHECKED : 0), 410 + i, ol);
    }
    AppendMenuW(mWdg, MF_POPUP, (UINT_PTR)mOp, L"Opacity");
    AppendMenuW(mWdg, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mWdg, MF_STRING, 420, L"Reset Widget Position");
    AppendMenuW(h, MF_POPUP, (UINT_PTR)mWdg, L"Floating Widget");

    int matchedIv = 0;
    for (int i = 0; i < 6; i++) {
        int chk = (curIv == IVS[i]);
        if (chk) matchedIv = 1;
        AppendMenuW(mIv, MF_STRING | (chk ? MF_CHECKED : 0), 200 + i, IV_NAMES[i]);
    }
    wchar_t customLbl[48];
    if (!matchedIv) _snwprintf(customLbl, 48, L"Custom (%lus)...", (unsigned long)curIv);
    else lstrcpyW(customLbl, L"Custom interval...");
    AppendMenuW(mIv, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mIv, MF_STRING | (!matchedIv ? MF_CHECKED : 0), 210, customLbl);
    AppendMenuW(h, MF_POPUP, (UINT_PTR)mIv, L"Auto-Refresh Interval");

    AppendMenuW(mDis, MF_STRING | (showTb  ? MF_CHECKED : 0), 306, L"Show on Taskbar");
    AppendMenuW(mDis, MF_STRING | (wsOn    ? MF_CHECKED : 0), 305, L"Real-Time WebSocket (Binance/OKX/Bybit/Gate)");
    AppendMenuW(mDis, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mDis, MF_STRING | (showPct ? MF_CHECKED : 0), 301, L"Show 24h % change");
    AppendMenuW(mDis, MF_STRING | (colChg  ? MF_CHECKED : 0), 302, L"Color by 24h change");
    AppendMenuW(mDis, MF_STRING | (flashOn ? MF_CHECKED : 0), 307, L"Tick flash animation (green/red)");
    AppendMenuW(mDis, MF_STRING | (showRef ? MF_CHECKED : 0), 303, L"Show manual refresh icon");
    AppendMenuW(mDis, MF_STRING | (compPr  ? MF_CHECKED : 0), 304, L"Compact large prices (K/M)");
    AppendMenuW(mDis, MF_SEPARATOR, 0, NULL);
    static const DWORD ROTS[5] = { 0, 5, 10, 15, 30 };
    static const wchar_t *ROT_NAMES[5] = { L"Off (scroll wheel only)", L"Every 5 seconds", L"Every 10 seconds", L"Every 15 seconds", L"Every 30 seconds" };
    for (int i = 0; i < 5; i++)
        AppendMenuW(mRot, MF_STRING | (rotIv == ROTS[i] ? MF_CHECKED : 0), 340 + i, ROT_NAMES[i]);
    AppendMenuW(mDis, MF_POPUP, (UINT_PTR)mRot, L"Auto-Rotate Pages");
    AppendMenuW(mOn, MF_STRING | (sw == SHOW_MAIN   ? MF_CHECKED : 0), 320, L"Main taskbar");
    AppendMenuW(mOn, MF_STRING | (sw == SHOW_ALL    ? MF_CHECKED : 0), 321, L"All taskbars");
    AppendMenuW(mOn, MF_STRING | (sw == SHOW_OTHERS ? MF_CHECKED : 0), 322, L"Other monitors only");
    AppendMenuW(mDis, MF_POPUP, (UINT_PTR)mOn, L"Show on");
    AppendMenuW(mDis, MF_STRING, 311, L"Move left");
    AppendMenuW(mDis, MF_STRING, 312, L"Move right");
    AppendMenuW(mDis, MF_STRING, 313, L"Reset position");
    AppendMenuW(h, MF_POPUP, (UINT_PTR)mDis, L"Display");

    AppendMenuW(h, MF_STRING | (app_autostart_is_on() ? MF_CHECKED : 0), 350, L"Start with Windows");
    AppendMenuW(h, MF_STRING | (log_is_enabled() ? MF_CHECKED : 0), 351, L"Enable Debug Log (traycoin.log)");
    if (log_is_enabled()) AppendMenuW(h, MF_STRING, 352, L"Open Log File...");
    AppendMenuW(h, MF_SEPARATOR, 0, NULL);
    AppendMenuW(h, MF_STRING, 399, L"Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    int cmd = TrackPopupMenu(h, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(h);

    if (cmd == 10) {
        poller_trigger_refresh(0);
    } else if (cmd == 11 || cmd == 210) {
        ui_settings_open();
    } else if (cmd >= 100 && cmd < 100 + MAX_COINS) {
        int idx = cmd - 100;
        EnterCriticalSection(&g_stateCs);
        if (idx < g_cfg.coinCount) {
            g_cfg.coins[idx].enabled = !g_cfg.coins[idx].enabled;
            if (g_cfg.coins[idx].enabled) g_cfg.coins[idx].nextFetchMs = 0;
            config_save(&g_cfg);
        }
        LeaveCriticalSection(&g_stateCs);
        poller_wake();
        ws_reload();
        taskbar_refresh(1);
        widget_refresh(1);
        ui_settings_notify_updated();
    } else if (cmd >= 200 && cmd < 206) {
        EnterCriticalSection(&g_stateCs);
        g_cfg.globalIntervalSec = IVS[cmd - 200];
        for (int i = 0; i < g_cfg.coinCount; i++) g_cfg.coins[i].nextFetchMs = 0;
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        poller_wake();
        ui_settings_notify_updated();
    } else if (cmd >= 301 && cmd <= 307) {
        EnterCriticalSection(&g_stateCs);
        if (cmd == 301) g_cfg.showChangePct   = !g_cfg.showChangePct;
        if (cmd == 302) g_cfg.colorByChange   = !g_cfg.colorByChange;
        if (cmd == 303) g_cfg.showRefreshIcon = !g_cfg.showRefreshIcon;
        if (cmd == 304) g_cfg.compactPrices   = !g_cfg.compactPrices;
        if (cmd == 305) g_cfg.enableWebSocket = !g_cfg.enableWebSocket;
        if (cmd == 306) g_cfg.showTaskbarBar  = !g_cfg.showTaskbarBar;
        if (cmd == 307) g_cfg.enableTickFlash = !g_cfg.enableTickFlash;
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        if (cmd == 305) { ws_reload(); poller_wake(); }
        taskbar_refresh(1);
        widget_refresh(1);
        ui_settings_sync_controls();
    } else if (cmd >= 340 && cmd < 345) {
        static const DWORD ROTS[5] = { 0, 5, 10, 15, 30 };
        EnterCriticalSection(&g_stateCs);
        g_cfg.autoRotateSec = ROTS[cmd - 340];
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        ui_settings_sync_controls();
    } else if (cmd >= 400 && cmd <= 420) {
        if (cmd == 420) {
            widget_reset_position();
        } else {
            static const int OPS[5] = { 40, 60, 75, 88, 100 };
            EnterCriticalSection(&g_stateCs);
            if (cmd == 400) g_cfg.showWidget         = !g_cfg.showWidget;
            if (cmd == 401) g_cfg.widgetLayout       = WIDGET_VERTICAL;
            if (cmd == 402) g_cfg.widgetLayout       = WIDGET_HORIZONTAL;
            if (cmd == 403) g_cfg.widgetTopmost      = !g_cfg.widgetTopmost;
            if (cmd == 404) { g_cfg.widgetLocked = !g_cfg.widgetLocked; if (!g_cfg.widgetLocked) g_cfg.widgetClickThrough = 0; }
            if (cmd == 405) { g_cfg.widgetClickThrough = !g_cfg.widgetClickThrough; if (g_cfg.widgetClickThrough) g_cfg.widgetLocked = 1; }
            if (cmd == 406) g_cfg.showSparkline      = !g_cfg.showSparkline;
            if (cmd >= 410 && cmd < 415) g_cfg.widgetOpacity = OPS[cmd - 410];
            config_save(&g_cfg);
            LeaveCriticalSection(&g_stateCs);
            widget_refresh(1);
            ui_settings_sync_controls();
        }
    } else if (cmd >= 311 && cmd <= 313) {
        EnterCriticalSection(&g_stateCs);
        g_cfg.offsetX = (cmd == 313) ? 0 : (g_cfg.offsetX + (cmd == 311 ? -12 : 12));
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        for (int i = 0; i < MAX_BARS; i++) {
            if (g_bars[i].wnd) {
                bar = &g_bars[i];
                ZeroMemory(&bar->lastPos, sizeof(bar->lastPos));
                bar_place();
            }
        }
    } else if (cmd >= 320 && cmd <= 322) {
        EnterCriticalSection(&g_stateCs);
        g_cfg.showOn = (ShowWhere)(cmd - 320);
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        taskbar_refresh(1);
    } else if (cmd == 350) {
        app_autostart_toggle();
        ui_settings_sync_controls();
    } else if (cmd == 351) {
        int en = 0;
        EnterCriticalSection(&g_stateCs);
        g_cfg.enableLogging = !g_cfg.enableLogging;
        en = g_cfg.enableLogging;
        config_save(&g_cfg);
        LeaveCriticalSection(&g_stateCs);
        log_set_enabled(en);
        ui_settings_sync_controls();
    } else if (cmd == 352) {
        wchar_t lp[MAX_PATH];
        log_get_path(lp, MAX_PATH);
        ShellExecuteW(NULL, L"open", lp, NULL, NULL, SW_SHOWNORMAL);
    } else if (cmd == 399) {
        DestroyWindow(g_wnd);
    }
}

/* ---- Public Lifecycle API ---- */

void taskbar_read_theme(void)
{
    DWORD v = 0, sz = sizeof(v);
    g_light = !RegGetValueW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) && v;
}

void taskbar_init(void)
{
    if (!g_scanCsInit) {
        InitializeCriticalSection(&g_scanCs);
        g_scanCsInit = 1;
    }
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = barproc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"TrayCoinBar";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
}

void taskbar_refresh(int force)
{
    tray_icon_update(force);

    if (!g_cfg.showTaskbarBar) {
        bars_destroy();
        return;
    }

    HWND tbs[MAX_BARS], main = FindWindowW(L"Shell_TrayWnd", NULL);
    int n = find_taskbars(tbs, MAX_BARS), others = n > (main != NULL);

    for (int i = 0; i < MAX_BARS; i++) {
        BAR *b = &g_bars[i];
        if (!b->wnd) continue;
        int keep = IsWindow(b->wnd) && IsWindow(b->tb) && wanted_tb(b->tb, main, others);
        for (int k = 0; keep && k < n; k++) if (tbs[k] == b->tb) keep = 2;
        if (keep != 2) bar_destroy(b);
    }
    for (int k = 0; k < n; k++) {
        if (!wanted_tb(tbs[k], main, others)) continue;
        int have = 0, freeIdx = -1;
        for (int i = 0; i < MAX_BARS; i++) {
            if (g_bars[i].wnd && g_bars[i].tb == tbs[k]) have = 1;
            else if (!g_bars[i].wnd && freeIdx < 0) freeIdx = i;
        }
        if (have || freeIdx < 0) continue;
        bar = &g_bars[freeIdx];
        bar_create(tbs[k], tbs[k] == main);
    }
    for (int i = 0; i < MAX_BARS; i++) {
        if (g_bars[i].wnd) {
            bar = &g_bars[i];
            bar_draw(force);
        }
    }
}

void taskbar_rotate_tick(void)
{
    DWORD rotSec = g_cfg.autoRotateSec;
    if (rotSec == 0 || !g_cfg.showTaskbarBar) return;
    ULONGLONG now = GetTickCount64();
    ULONGLONG ivMs = (ULONGLONG)rotSec * 1000ULL;
    for (int i = 0; i < MAX_BARS; i++) {
        BAR *b = &g_bars[i];
        if (!b->wnd || b->pageCount <= 1) continue;
        if (b->hoverKind != HIT_NONE) continue; /* Pause rotation while hovering */
        if (b->lastRotateMs == 0) { b->lastRotateMs = now; continue; }
        if (now - b->lastRotateMs >= ivMs) {
            b->page = (b->page + 1) % b->pageCount;
            b->lastRotateMs = now;
            bar = b;
            bar_draw(1);
        }
    }
}

void taskbar_recreate_all(void)
{
    g_trayShown = 0;
    bars_destroy();
    taskbar_refresh(1);
}

void taskbar_shutdown(void)
{
    if (g_trayShown) {
        NOTIFYICONDATAW n; ZeroMemory(&n, sizeof(n));
        n.cbSize = sizeof(n); n.hWnd = g_wnd; n.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &n);
        g_trayShown = 0;
    }
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
    bars_destroy();
    if (g_scanCsInit) {
        DeleteCriticalSection(&g_scanCs);
        g_scanCsInit = 0;
    }
}
