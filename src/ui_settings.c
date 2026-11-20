/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "ui_settings.h"
#include "config.h"
#include "providers.h"
#include "poller.h"
#include "ws.h"
#include "logger.h"
#include "taskbar.h"
#include "widget.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

/* Strictly unique auto-incremented Control IDs */
enum {
    ID_TAB_MAIN = 1001,

    /* Tab 0: Watchlist */
    ID_LBL_SEARCH,
    ID_EDT_SEARCH,
    ID_BTN_SEARCH_CLR,
    ID_LBL_COUNT,
    ID_BTN_SEL_ALL,
    ID_BTN_UNSEL_ALL,
    ID_LV_COINS,
    ID_BTN_ADD,
    ID_BTN_EDIT,
    ID_BTN_DEL,
    ID_BTN_UP,
    ID_BTN_DOWN,
    ID_BTN_REFRESH_ALL,
    ID_BTN_EXPORT,
    ID_BTN_IMPORT,

    /* Tab 1: Settings, Widget & APIs */
    ID_GRP_DISPLAY,
    ID_LBL_IV,
    ID_EDT_INTERVAL,
    ID_BTN_IV_5,
    ID_BTN_IV_15,
    ID_BTN_IV_30,
    ID_BTN_IV_60,
    ID_BTN_IV_300,
    ID_LBL_SHOWON,
    ID_CMB_SHOWON,
    ID_CHK_PCT,
    ID_CHK_COLOR,
    ID_LBL_CLICK,
    ID_CMB_CLICK,
    ID_CHK_REFICON,
    ID_CHK_COMPACT,
    ID_CHK_FLASH,
    ID_CHK_AUTOSTART,
    ID_LBL_ROTATE,
    ID_EDT_ROTATE,
    ID_CHK_TRAY,
    ID_CHK_WS,
    ID_CHK_ALERTSND,
    ID_CHK_LOG,
    ID_BTN_OPEN_LOG,

    ID_GRP_WIDGET,
    ID_CHK_TASKBAR,
    ID_CHK_WIDGET,
    ID_LBL_WLAYOUT,
    ID_CMB_WLAYOUT,
    ID_LBL_WOPACITY,
    ID_EDT_WOPACITY,
    ID_BTN_WRESET,
    ID_CHK_WTOPMOST,
    ID_CHK_WLOCK,
    ID_CHK_WCLICKTHRU,
    ID_CHK_SPARK,
    ID_CHK_HOTKEYS,

    ID_GRP_API,
    ID_LBL_CMC,
    ID_EDT_CMC_KEY,
    ID_BTN_CMC_HELP,
    ID_LBL_CG,
    ID_EDT_CG_KEY,
    ID_LBL_ABOUT,

    /* Bottom bar (always visible) */
    ID_LBL_CFGPATH,
    ID_BTN_APPLY,
    ID_BTN_SAVE_CLOSE,
    ID_BTN_CLOSE,

    /* Add/Edit Coin Dialog controls */
    ID_EC_PROV = 2001,
    ID_EC_QUERY,
    ID_EC_CHAIN,
    ID_EC_LABEL,
    ID_EC_INTERVAL,
    ID_EC_DECIMALS,
    ID_EC_ALERT_HI,
    ID_EC_ALERT_LO,
    ID_EC_HOLDING,
    ID_EC_TEST,
    ID_EC_STATUS,
    ID_EC_RESULTS,
    ID_EC_OK,
    ID_EC_CANCEL
};

/* Right-click ListView menu commands */
enum {
    LVCMD_REFRESH = 5001,
    LVCMD_EDIT,
    LVCMD_CHART,
    LVCMD_COPY,
    LVCMD_SHOW,
    LVCMD_HIDE,
    LVCMD_UP,
    LVCMD_DOWN,
    LVCMD_DELETE
};

#define WM_TEST_DONE   (WM_APP + 20)
#define WM_SEARCH_DONE (WM_APP + 21)

static HWND   g_setWnd     = NULL;
static HFONT  g_uiFont     = NULL;
static HFONT  g_uiBold     = NULL;
static int    g_uiDpi      = 96;
static int    g_populating = 0;
static int    g_curTab     = 0;

/* Offline search filter & row -> g_cfg.coins[] index mapping */
static wchar_t g_filterText[128] = {0};
static int     g_viewMap[MAX_COINS];
static int     g_viewCount = 0;

static int S(int px) { return MulDiv(px, g_uiDpi, 96); }

static void ensure_ui_resources(HWND w)
{
    typedef UINT (WINAPI *GDW)(HWND);
    GDW gd = (GDW)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = (gd && w) ? gd(w) : 96;
    if (d < 96) d = 96;
    if (!g_uiFont || (int)d != g_uiDpi) {
        g_uiDpi = (int)d;
        if (g_uiFont) DeleteObject(g_uiFont);
        if (g_uiBold) DeleteObject(g_uiBold);
        g_uiFont = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_uiBold = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
}

static HWND mk_ctrl(HWND parent, const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD exStyle,
                    int x, int y, int w, int h, int id, int bold)
{
    HWND c = CreateWindowExW(exStyle, cls, txt, style | WS_CHILD | WS_VISIBLE,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)(bold ? g_uiBold : g_uiFont), TRUE);
    return c;
}

static int wcsistr_match(const wchar_t *hay, const wchar_t *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay || !hay[0]) return 0;
    size_t nlen = wcslen(needle);
    for (const wchar_t *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && towlower(p[i]) == towlower(needle[i])) i++;
        if (i == nlen) return 1;
    }
    return 0;
}

static int coin_matches_filter(const CoinItem *c, const wchar_t *flt)
{
    if (!flt || !flt[0]) return 1;
    wchar_t lbl[24];
    provider_default_label(c, lbl, 24);
    if (wcsistr_match(lbl, flt)) return 1;
    if (wcsistr_match(c->query, flt)) return 1;
    if (wcsistr_match(c->chain, flt)) return 1;
    if (wcsistr_match(c->resolvedSource, flt)) return 1;
    if (wcsistr_match(provider_display_name(c->provider), flt)) return 1;
    if (wcsistr_match(c->pairUrl, flt)) return 1;
    return 0;
}

static void copy_text_to_clipboard(HWND owner, const wchar_t *text)
{
    if (!text || !text[0]) return;
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void *p = GlobalLock(hg);
        if (p) { memcpy(p, text, bytes); GlobalUnlock(hg); }
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
}

/* =========================================================================
 * Add / Edit Coin Dialog (with Alert Above/Below + Holdings Qty)
 * ========================================================================= */

static const wchar_t *CHAINS[] = {
    L"(Auto)", L"solana", L"ethereum", L"base", L"bsc", L"arbitrum",
    L"polygon", L"ton", L"sui", L"avalanche", L"optimism"
};

typedef struct {
    int          editIdx;
    CoinItem     item;
    int          saved;
    HWND         hwnd;
    DexSearchHit hits[12];
    int          hitCount;
    int          testing;
} EditCtx;

typedef struct {
    HWND         dlg;
    CoinItem     coin;
    char         cmcKey[128];
    char         cgKey[128];
    int          isDexSearch;
    FetchResult  res;
    DexSearchHit hits[12];
    int          hitCount;
} TestJob;

static DWORD WINAPI test_worker(void *arg)
{
    TestJob *job = (TestJob *)arg;
    if (job->isDexSearch) {
        job->hitCount = provider_search_dex(job->coin.query, job->coin.chain, job->hits, 12);
        if (IsWindow(job->dlg)) PostMessageW(job->dlg, WM_SEARCH_DONE, 0, (LPARAM)job);
        else HeapFree(GetProcessHeap(), 0, job);
    } else {
        provider_fetch_one(&job->coin, job->cmcKey, job->cgKey, &job->res);
        if (IsWindow(job->dlg)) PostMessageW(job->dlg, WM_TEST_DONE, 0, (LPARAM)job);
        else HeapFree(GetProcessHeap(), 0, job);
    }
    return 0;
}

static double parse_opt_double(HWND dlg, int id)
{
    wchar_t buf[48] = {0};
    GetDlgItemTextW(dlg, id, buf, 48);
    if (!buf[0]) return 0.0;
    wchar_t *end = NULL;
    double v = wcstod(buf, &end);
    return (end != buf && v > 0.0) ? v : 0.0;
}

static void fmt_opt_double(double v, wchar_t *out, int cap)
{
    if (v <= 0.0) { out[0] = 0; return; }
    _snwprintf(out, cap, L"%.10g", v);
    out[cap - 1] = 0;
}

static void read_edit_form(HWND dlg, CoinItem *out)
{
    int psel = (int)SendDlgItemMessageW(dlg, ID_EC_PROV, CB_GETCURSEL, 0, 0);
    if (psel < 0 || psel >= PROV_COUNT) psel = PROV_AUTO;
    out->provider = (ProviderId)psel;

    GetDlgItemTextW(dlg, ID_EC_QUERY, out->query, 128);
    out->inputKind = provider_detect_input(out->query);

    int csel = (int)SendDlgItemMessageW(dlg, ID_EC_CHAIN, CB_GETCURSEL, 0, 0);
    if (csel > 0 && csel < (int)(sizeof(CHAINS) / sizeof(CHAINS[0])))
        lstrcpynW(out->chain, CHAINS[csel], 32);
    else
        out->chain[0] = 0;

    GetDlgItemTextW(dlg, ID_EC_LABEL, out->label, 24);

    wchar_t ivBuf[16] = {0};
    GetDlgItemTextW(dlg, ID_EC_INTERVAL, ivBuf, 16);
    int iv = _wtoi(ivBuf);
    if (iv <= 0) out->intervalSec = 0;
    else {
        if (iv < MIN_INTERVAL_SEC) iv = MIN_INTERVAL_SEC;
        if (iv > MAX_INTERVAL_SEC) iv = MAX_INTERVAL_SEC;
        out->intervalSec = (DWORD)iv;
    }

    int dsel = (int)SendDlgItemMessageW(dlg, ID_EC_DECIMALS, CB_GETCURSEL, 0, 0);
    static const int DEC_VALS[] = { -1, 0, 2, 3, 4, 5, 6, 8 };
    if (dsel >= 0 && dsel < (int)(sizeof(DEC_VALS) / sizeof(DEC_VALS[0])))
        out->decimals = DEC_VALS[dsel];
    else
        out->decimals = -1;

    double newHi = parse_opt_double(dlg, ID_EC_ALERT_HI);
    double newLo = parse_opt_double(dlg, ID_EC_ALERT_LO);
    if (newHi != out->alertAbove) out->alertFiredAbove = 0;
    if (newLo != out->alertBelow) out->alertFiredBelow = 0;
    out->alertAbove = newHi;
    out->alertBelow = newLo;
    out->holdingQty = parse_opt_double(dlg, ID_EC_HOLDING);
}

static void populate_dex_results(HWND dlg, EditCtx *ctx)
{
    HWND lv = GetDlgItem(dlg, ID_EC_RESULTS);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < ctx->hitCount; i++) {
        const DexSearchHit *h = &ctx->hits[i];
        LVITEMW li; ZeroMemory(&li, sizeof(li));
        li.mask = LVIF_TEXT;
        li.iItem = i;
        li.pszText = (LPWSTR)h->symbol;
        ListView_InsertItem(lv, &li);

        ListView_SetItemText(lv, i, 1, (LPWSTR)h->chain);
        ListView_SetItemText(lv, i, 2, (LPWSTR)h->dex);

        wchar_t pstr[32], lstr[32];
        format_price(h->price, -1, 1, 0, pstr, 32);
        ListView_SetItemText(lv, i, 3, pstr);

        if (h->liquidityUsd >= 1000000.0) _snwprintf(lstr, 32, L"$%.1fM", h->liquidityUsd / 1000000.0);
        else if (h->liquidityUsd >= 1000.0) _snwprintf(lstr, 32, L"$%.1fK", h->liquidityUsd / 1000.0);
        else _snwprintf(lstr, 32, L"$%.0f", h->liquidityUsd);
        ListView_SetItemText(lv, i, 4, lstr);
        ListView_SetItemText(lv, i, 5, (LPWSTR)h->contract);
    }
}

static LRESULT CALLBACK edit_dlg_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    EditCtx *ctx = (EditCtx *)GetWindowLongPtrW(w, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        ctx = (EditCtx *)cs->lpCreateParams;
        ctx->hwnd = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)ctx);
        ensure_ui_resources(w);

        int y = 14;
        mk_ctrl(w, L"STATIC", L"Exchange / API Source:", 0, 0, 14, y + 3, 145, 20, 0, 0);
        HWND cbProv = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 165, y, 220, 260, ID_EC_PROV, 0);
        for (int i = 0; i < PROV_COUNT; i++)
            SendMessageW(cbProv, CB_ADDSTRING, 0, (LPARAM)provider_display_name((ProviderId)i));
        SendMessageW(cbProv, CB_SETCURSEL, (WPARAM)ctx->item.provider, 0);

        mk_ctrl(w, L"STATIC", L"Chain (DEX/CG):", 0, 0, 398, y + 3, 100, 20, 0, 0);
        HWND cbChain = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 500, y, 120, 220, ID_EC_CHAIN, 0);
        int chainSel = 0;
        for (int i = 0; i < (int)(sizeof(CHAINS) / sizeof(CHAINS[0])); i++) {
            SendMessageW(cbChain, CB_ADDSTRING, 0, (LPARAM)CHAINS[i]);
            if (i > 0 && ctx->item.chain[0] && _wcsicmp(ctx->item.chain, CHAINS[i]) == 0) chainSel = i;
        }
        SendMessageW(cbChain, CB_SETCURSEL, (WPARAM)chainSel, 0);

        y += 32;
        mk_ctrl(w, L"STATIC", L"Pair / Contract / URL:", 0, 0, 14, y + 3, 145, 20, 0, 0);
        HWND edQ = mk_ctrl(w, L"EDIT", ctx->item.query, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 165, y, 330, 24, ID_EC_QUERY, 0);
        SendMessageW(edQ, EM_SETCUEBANNER, TRUE, (LPARAM)L"BTC/USDT, PEPE, 0x..., Solana mint, DexScreener URL");
        mk_ctrl(w, L"BUTTON", L"Test / Search", BS_PUSHBUTTON | WS_TABSTOP, 0, 503, y, 117, 24, ID_EC_TEST, 1);

        y += 32;
        mk_ctrl(w, L"STATIC", L"Display Label:", 0, 0, 14, y + 3, 145, 20, 0, 0);
        HWND edL = mk_ctrl(w, L"EDIT", ctx->item.label, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 165, y, 115, 24, ID_EC_LABEL, 0);
        SendMessageW(edL, EM_SETCUEBANNER, TRUE, (LPARAM)L"Auto (e.g. BTC)");

        mk_ctrl(w, L"STATIC", L"Interval (sec, 0=Global):", 0, 0, 290, y + 3, 140, 20, 0, 0);
        wchar_t ivStr[16];
        _snwprintf(ivStr, 16, L"%lu", (unsigned long)ctx->item.intervalSec);
        mk_ctrl(w, L"EDIT", ivStr, ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 434, y, 52, 24, ID_EC_INTERVAL, 0);

        mk_ctrl(w, L"STATIC", L"Dec:", 0, 0, 496, y + 3, 32, 20, 0, 0);
        HWND cbDec = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 530, y, 90, 180, ID_EC_DECIMALS, 0);
        static const wchar_t *DEC_NAMES[] = { L"Auto", L"0", L"2", L"3", L"4", L"5", L"6", L"8" };
        static const int DEC_VALS[] = { -1, 0, 2, 3, 4, 5, 6, 8 };
        int dsel = 0;
        for (int i = 0; i < 8; i++) {
            SendMessageW(cbDec, CB_ADDSTRING, 0, (LPARAM)DEC_NAMES[i]);
            if (ctx->item.decimals == DEC_VALS[i]) dsel = i;
        }
        SendMessageW(cbDec, CB_SETCURSEL, (WPARAM)dsel, 0);

        /* Alerts & Holdings row */
        y += 32;
        wchar_t hiStr[32], loStr[32], hqStr[32];
        fmt_opt_double(ctx->item.alertAbove, hiStr, 32);
        fmt_opt_double(ctx->item.alertBelow, loStr, 32);
        fmt_opt_double(ctx->item.holdingQty, hqStr, 32);

        mk_ctrl(w, L"STATIC", L"Alert >= ($):", 0, 0, 14, y + 3, 76, 20, 0, 0);
        HWND edHi = mk_ctrl(w, L"EDIT", hiStr, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 92, y, 105, 24, ID_EC_ALERT_HI, 0);
        SendMessageW(edHi, EM_SETCUEBANNER, TRUE, (LPARAM)L"Optional");

        mk_ctrl(w, L"STATIC", L"Alert <= ($):", 0, 0, 210, y + 3, 76, 20, 0, 0);
        HWND edLo = mk_ctrl(w, L"EDIT", loStr, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 288, y, 105, 24, ID_EC_ALERT_LO, 0);
        SendMessageW(edLo, EM_SETCUEBANNER, TRUE, (LPARAM)L"Optional");

        mk_ctrl(w, L"STATIC", L"Holdings (Qty):", 0, 0, 408, y + 3, 94, 20, 0, 0);
        HWND edHq = mk_ctrl(w, L"EDIT", hqStr, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 504, y, 116, 24, ID_EC_HOLDING, 0);
        SendMessageW(edHq, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. 0.5");

        y += 32;
        mk_ctrl(w, L"STATIC", L"Tip: For meme coins, choose DexScreener, type token name or contract, and click Test / Search.",
                SS_LEFT, 0, 14, y, 606, 22, ID_EC_STATUS, 0);

        y += 26;
        HWND lv = mk_ctrl(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                          0, 14, y, 606, 150, ID_EC_RESULTS, 0);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        static const struct { const wchar_t *t; int w; } COLS[] = {
            { L"Symbol", 75 }, { L"Chain", 75 }, { L"DEX", 85 }, { L"Price", 95 }, { L"Liquidity", 80 }, { L"Contract (click to select)", 185 }
        };
        for (int i = 0; i < 6; i++) {
            LVCOLUMNW col; ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)COLS[i].t;
            col.cx = S(COLS[i].w);
            ListView_InsertColumn(lv, i, &col);
        }

        y += 162;
        mk_ctrl(w, L"BUTTON", L"Save Coin", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 434, y, 90, 28, ID_EC_OK, 1);
        mk_ctrl(w, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, 530, y, 90, 28, ID_EC_CANCEL, 0);
        SetFocus(edQ);
        return 0;
    }

    case WM_COMMAND:
        if (!ctx) break;
        switch (LOWORD(wp)) {
        case ID_EC_TEST: {
            if (ctx->testing) return 0;
            CoinItem tmp = ctx->item;
            read_edit_form(w, &tmp);
            if (!tmp.query[0]) {
                SetDlgItemTextW(w, ID_EC_STATUS, L"Please enter a trading pair, token name, or contract address.");
                return 0;
            }
            TestJob *job = (TestJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TestJob));
            if (!job) return 0;
            job->dlg = w;
            job->coin = tmp;
            EnterCriticalSection(&g_stateCs);
            lstrcpynA(job->cmcKey, g_cfg.cmcApiKey, sizeof(job->cmcKey));
            lstrcpynA(job->cgKey,  g_cfg.cgApiKey,  sizeof(job->cgKey));
            LeaveCriticalSection(&g_stateCs);

            job->isDexSearch = (tmp.provider == PROV_DEXSCREENER ||
                                (tmp.provider == PROV_AUTO && provider_detect_input(tmp.query) == INPUT_CONTRACT));

            ctx->testing = 1;
            EnableWindow(GetDlgItem(w, ID_EC_TEST), FALSE);
            SetDlgItemTextW(w, ID_EC_STATUS, L"Fetching live data...");
            HANDLE th = CreateThread(NULL, 0, test_worker, job, 0, NULL);
            if (th) CloseHandle(th);
            return 0;
        }
        case ID_EC_OK: {
            read_edit_form(w, &ctx->item);
            if (!ctx->item.query[0]) {
                MessageBoxW(w, L"Please enter a trading pair or contract address.", APP_NAME, MB_ICONWARNING);
                return 0;
            }
            ctx->saved = 1;
            DestroyWindow(w);
            return 0;
        }
        case ID_EC_CANCEL:
            DestroyWindow(w);
            return 0;
        }
        break;

    case WM_TEST_DONE: {
        TestJob *job = (TestJob *)lp;
        if (ctx && job) {
            ctx->testing = 0;
            EnableWindow(GetDlgItem(w, ID_EC_TEST), TRUE);
            if (job->res.ok) {
                wchar_t pstr[32], pct[16] = {0}, msgBuf[200];
                format_price(job->res.price, -1, 1, 0, pstr, 32);
                if (job->res.hasChange24h) format_pct(job->res.change24h, pct, 16);
                _snwprintf(msgBuf, 200, L"OK: %s = %s %s (via %s)",
                           job->res.symbol[0] ? job->res.symbol : job->coin.query, pstr, pct, job->res.source);
                SetDlgItemTextW(w, ID_EC_STATUS, msgBuf);
                ctx->item.ok = 1;
                ctx->item.price = job->res.price;
                ctx->item.prevPrice = job->res.price;
                ctx->item.change24h = job->res.change24h;
                ctx->item.hasChange24h = job->res.hasChange24h;
                ctx->item.lastUpdateMs = GetTickCount64();
                GetLocalTime(&ctx->item.lastUpdateTime);
                if (job->res.source[0]) lstrcpynW(ctx->item.resolvedSource, job->res.source, 32);
                if (job->res.symbol[0]) lstrcpynW(ctx->item.resolvedSymbol, job->res.symbol, 24);
                if (job->res.url[0])    lstrcpynW(ctx->item.pairUrl, job->res.url, 256);
                ctx->item.lastError[0] = 0;
                wchar_t curLbl[24];
                GetDlgItemTextW(w, ID_EC_LABEL, curLbl, 24);
                if (!curLbl[0] && job->res.symbol[0]) SetDlgItemTextW(w, ID_EC_LABEL, job->res.symbol);
            } else {
                wchar_t msgBuf[200];
                _snwprintf(msgBuf, 200, L"Failed (%s): %s",
                           job->res.source[0] ? job->res.source : provider_display_name(job->coin.provider),
                           job->res.error[0] ? job->res.error : L"Not found");
                SetDlgItemTextW(w, ID_EC_STATUS, msgBuf);
            }
        }
        if (job) HeapFree(GetProcessHeap(), 0, job);
        return 0;
    }

    case WM_SEARCH_DONE: {
        TestJob *job = (TestJob *)lp;
        if (ctx && job) {
            ctx->testing = 0;
            EnableWindow(GetDlgItem(w, ID_EC_TEST), TRUE);
            ctx->hitCount = job->hitCount;
            if (job->hitCount > 0) {
                memcpy(ctx->hits, job->hits, sizeof(DexSearchHit) * job->hitCount);
                populate_dex_results(w, ctx);
                wchar_t pstr[32], msgBuf[200];
                format_price(ctx->hits[0].price, -1, 1, 0, pstr, 32);
                _snwprintf(msgBuf, 200, L"Found %d pool(s). Top: %s (%s/%s) = %s. Click a row below to fill.",
                           ctx->hitCount, ctx->hits[0].symbol, ctx->hits[0].chain, ctx->hits[0].dex, pstr);
                SetDlgItemTextW(w, ID_EC_STATUS, msgBuf);
                wchar_t curLbl[24];
                GetDlgItemTextW(w, ID_EC_LABEL, curLbl, 24);
                if (!curLbl[0] && ctx->hits[0].symbol[0]) SetDlgItemTextW(w, ID_EC_LABEL, ctx->hits[0].symbol);
            } else {
                SetDlgItemTextW(w, ID_EC_STATUS, L"No DexScreener pools found. Try another contract or token symbol.");
            }
        }
        if (job) HeapFree(GetProcessHeap(), 0, job);
        return 0;
    }

    case WM_NOTIFY:
        if (ctx && ((NMHDR *)lp)->idFrom == ID_EC_RESULTS &&
            (((NMHDR *)lp)->code == NM_CLICK || ((NMHDR *)lp)->code == NM_DBLCLK)) {
            NMITEMACTIVATE *na = (NMITEMACTIVATE *)lp;
            if (na->iItem >= 0 && na->iItem < ctx->hitCount) {
                const DexSearchHit *h = &ctx->hits[na->iItem];
                SetDlgItemTextW(w, ID_EC_QUERY, h->contract);
                SetDlgItemTextW(w, ID_EC_LABEL, h->symbol);
                SendDlgItemMessageW(w, ID_EC_PROV, CB_SETCURSEL, (WPARAM)PROV_DEXSCREENER, 0);
                int csel = 0;
                for (int i = 1; i < (int)(sizeof(CHAINS) / sizeof(CHAINS[0])); i++) {
                    if (_wcsicmp(h->chain, CHAINS[i]) == 0) { csel = i; break; }
                }
                SendDlgItemMessageW(w, ID_EC_CHAIN, CB_SETCURSEL, (WPARAM)csel, 0);
                wchar_t pstr[32], msgBuf[200];
                format_price(h->price, -1, 1, 0, pstr, 32);
                _snwprintf(msgBuf, 200, L"Selected: %s on %s (%s) — %s", h->symbol, h->chain, h->dex, pstr);
                SetDlgItemTextW(w, ID_EC_STATUS, msgBuf);
                ctx->item.ok = 1;
                ctx->item.price = h->price;
                ctx->item.prevPrice = h->price;
                ctx->item.change24h = h->change24h;
                ctx->item.hasChange24h = 1;
                ctx->item.lastUpdateMs = GetTickCount64();
                GetLocalTime(&ctx->item.lastUpdateTime);
                _snwprintf(ctx->item.resolvedSource, 32, L"DEX/%s", h->dex);
                lstrcpynW(ctx->item.resolvedSymbol, h->symbol, 24);
                lstrcpynW(ctx->item.pairUrl, h->url, 256);
                ctx->item.lastError[0] = 0;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int open_edit_coin_modal(HWND owner, int editIdx, CoinItem *ioItem)
{
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = edit_dlg_proc;
        wc.hInstance = g_hinst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TrayCoinEditDlg";
        RegisterClassW(&wc);
        reg = 1;
    }

    EditCtx ctx; ZeroMemory(&ctx, sizeof(ctx));
    ctx.editIdx = editIdx;
    ctx.item = *ioItem;

    ensure_ui_resources(owner);
    int W = S(652), H = S(416);
    RECT pr; GetWindowRect(owner, &pr);
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    if (x < 20) x = 20;
    if (y < 20) y = 20;

    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TrayCoinEditDlg",
                               editIdx < 0 ? L"Add Coin / Trading Pair" : L"Edit Coin / Trading Pair",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, W, H, owner, NULL, g_hinst, &ctx);

    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && (m.hwnd == dlg || IsChild(dlg, m.hwnd))) {
            DestroyWindow(dlg);
            continue;
        }
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (ctx.saved) {
        *ioItem = ctx.item;
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Main Settings & Watchlist Manager Window (2 Tabs, Resizable, Dark Mode)
 * ========================================================================= */

static const int TAB0_IDS[] = {
    ID_LBL_SEARCH, ID_EDT_SEARCH, ID_BTN_SEARCH_CLR, ID_LBL_COUNT, ID_BTN_SEL_ALL, ID_BTN_UNSEL_ALL,
    ID_LV_COINS, ID_BTN_ADD, ID_BTN_EDIT, ID_BTN_DEL, ID_BTN_UP, ID_BTN_DOWN,
    ID_BTN_REFRESH_ALL, ID_BTN_EXPORT, ID_BTN_IMPORT
};

static const int TAB1_IDS[] = {
    ID_GRP_DISPLAY, ID_LBL_IV, ID_EDT_INTERVAL, ID_BTN_IV_5, ID_BTN_IV_15, ID_BTN_IV_30, ID_BTN_IV_60, ID_BTN_IV_300,
    ID_LBL_SHOWON, ID_CMB_SHOWON, ID_CHK_PCT, ID_CHK_COLOR, ID_LBL_CLICK, ID_CMB_CLICK,
    ID_CHK_REFICON, ID_CHK_COMPACT, ID_CHK_FLASH, ID_CHK_AUTOSTART, ID_LBL_ROTATE, ID_EDT_ROTATE,
    ID_CHK_TRAY, ID_CHK_WS, ID_CHK_ALERTSND, ID_CHK_LOG, ID_BTN_OPEN_LOG,
    ID_GRP_WIDGET, ID_CHK_TASKBAR, ID_CHK_WIDGET, ID_LBL_WLAYOUT, ID_CMB_WLAYOUT,
    ID_LBL_WOPACITY, ID_EDT_WOPACITY, ID_BTN_WRESET,
    ID_CHK_WTOPMOST, ID_CHK_WLOCK, ID_CHK_WCLICKTHRU, ID_CHK_SPARK, ID_CHK_HOTKEYS,
    ID_GRP_API, ID_LBL_CMC, ID_EDT_CMC_KEY, ID_BTN_CMC_HELP, ID_LBL_CG, ID_EDT_CG_KEY, ID_LBL_ABOUT
};

static void show_tab(HWND w, int tab)
{
    g_curTab = tab;
    for (int i = 0; i < (int)(sizeof(TAB0_IDS) / sizeof(TAB0_IDS[0])); i++) {
        HWND c = GetDlgItem(w, TAB0_IDS[i]);
        if (c) ShowWindow(c, tab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int i = 0; i < (int)(sizeof(TAB1_IDS) / sizeof(TAB1_IDS[0])); i++) {
        HWND c = GetDlgItem(w, TAB1_IDS[i]);
        if (c) ShowWindow(c, tab == 1 ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(w, NULL, TRUE);
}

static void layout_settings_window(HWND w)
{
    RECT cr; GetClientRect(w, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;
    if (cw < S(600)) cw = S(600);
    if (ch < S(460)) ch = S(460);

    int pad = S(12);
    int botH = S(42);
    int tabH = ch - pad * 2 - botH;

    MoveWindow(GetDlgItem(w, ID_TAB_MAIN), pad, pad, cw - pad * 2, tabH, TRUE);

    /* Tab 0 dynamic layout */
    int tx = pad + S(12);
    int tw = cw - pad * 2 - S(24);
    int ty = pad + S(32);

    MoveWindow(GetDlgItem(w, ID_LBL_SEARCH),     tx,           ty + S(3), S(50), S(20), TRUE);
    MoveWindow(GetDlgItem(w, ID_EDT_SEARCH),     tx + S(52),   ty,        S(230), S(24), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_SEARCH_CLR), tx + S(286),  ty,        S(28),  S(24), TRUE);
    MoveWindow(GetDlgItem(w, ID_LBL_COUNT),      tx + S(322),  ty + S(3), tw - S(505), S(20), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_SEL_ALL),    tx + tw - S(176), ty,    S(84),  S(24), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_UNSEL_ALL),  tx + tw - S(88),  ty,    S(88),  S(24), TRUE);

    int lvTop = ty + S(32);
    int barY  = pad + tabH - S(38);
    int lvH   = barY - lvTop - S(8);
    if (lvH < S(120)) lvH = S(120);
    MoveWindow(GetDlgItem(w, ID_LV_COINS), tx, lvTop, tw, lvH, TRUE);

    MoveWindow(GetDlgItem(w, ID_BTN_ADD),         tx,             barY, S(86),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_EDIT),        tx + S(92),     barY, S(58),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_DEL),         tx + S(156),    barY, S(64),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_UP),          tx + S(230),    barY, S(56),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_DOWN),        tx + S(292),    barY, S(64),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_REFRESH_ALL), tx + tw - S(294), barY, S(106), S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_EXPORT),      tx + tw - S(182), barY, S(88),  S(26), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_IMPORT),      tx + tw - S(88),  barY, S(88),  S(26), TRUE);

    /* Bottom bar (shared across both tabs) */
    int by = ch - botH + S(6);
    MoveWindow(GetDlgItem(w, ID_LBL_CFGPATH),    pad,              by + S(5), cw - S(308), S(20), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_APPLY),      cw - pad - S(276), by, S(84), S(28), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_SAVE_CLOSE), cw - pad - S(186), by, S(98), S(28), TRUE);
    MoveWindow(GetDlgItem(w, ID_BTN_CLOSE),      cw - pad - S(82),  by, S(82), S(28), TRUE);
}

static void set_lv_cell_if_changed(HWND lv, int row, int col, const wchar_t *val)
{
    wchar_t cur[128] = {0};
    ListView_GetItemText(lv, row, col, cur, 128);
    if (lstrcmpW(cur, val) != 0) {
        ListView_SetItemText(lv, row, col, (LPWSTR)val);
    }
}

static void populate_coin_list(HWND w)
{
    HWND lv = GetDlgItem(w, ID_LV_COINS);
    if (!lv) return;

    static AppConfig snap;
    EnterCriticalSection(&g_stateCs);
    snap = g_cfg;
    LeaveCriticalSection(&g_stateCs);

    /* Build filtered view mapping */
    int newMap[MAX_COINS];
    int newCount = 0;
    for (int i = 0; i < snap.coinCount; i++) {
        if (coin_matches_filter(&snap.coins[i], g_filterText)) {
            newMap[newCount++] = i;
        }
    }

    int curCount = ListView_GetItemCount(lv);
    int sameShape = (curCount == newCount && newCount == g_viewCount &&
                     (newCount == 0 || memcmp(newMap, g_viewMap, sizeof(int) * newCount) == 0));

    g_viewCount = newCount;
    if (newCount > 0) memcpy(g_viewMap, newMap, sizeof(int) * newCount);

    /* Update count label */
    wchar_t cntBuf[64];
    if (g_filterText[0]) _snwprintf(cntBuf, 64, L"Showing %d / %d", newCount, snap.coinCount);
    else _snwprintf(cntBuf, 64, L"Total: %d coin(s)", snap.coinCount);
    SetDlgItemTextW(w, ID_LBL_COUNT, cntBuf);

    g_populating = 1;
    if (!sameShape) {
        SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(lv);
    }

    for (int row = 0; row < newCount; row++) {
        const CoinItem *c = &snap.coins[newMap[row]];
        wchar_t lbl[24], ivTxt[24], pstr[32], pct[16], valAl[64] = L"", status[96];
        provider_default_label(c, lbl, 24);

        if (!sameShape) {
            LVITEMW li; ZeroMemory(&li, sizeof(li));
            li.mask = LVIF_TEXT;
            li.iItem = row;
            li.pszText = lbl;
            ListView_InsertItem(lv, &li);
            ListView_SetCheckState(lv, row, c->enabled ? TRUE : FALSE);
        } else {
            set_lv_cell_if_changed(lv, row, 0, lbl);
            BOOL wantChk = c->enabled ? TRUE : FALSE;
            if (ListView_GetCheckState(lv, row) != wantChk)
                ListView_SetCheckState(lv, row, wantChk);
        }

        set_lv_cell_if_changed(lv, row, 1, c->query);
        const wchar_t *src = c->resolvedSource[0] ? c->resolvedSource : provider_display_name(c->provider);
        set_lv_cell_if_changed(lv, row, 2, src);

        if (c->intervalSec > 0) _snwprintf(ivTxt, 24, L"%lus", (unsigned long)c->intervalSec);
        else _snwprintf(ivTxt, 24, L"Global (%lus)", (unsigned long)snap.globalIntervalSec);
        set_lv_cell_if_changed(lv, row, 3, ivTxt);

        if (c->ok) {
            format_price(c->price, c->decimals, 1, 0, pstr, 32);
            set_lv_cell_if_changed(lv, row, 4, pstr);
            if (c->hasChange24h) {
                format_pct(c->change24h, pct, 16);
                set_lv_cell_if_changed(lv, row, 5, pct);
            } else {
                set_lv_cell_if_changed(lv, row, 5, L"--");
            }
            _snwprintf(status, 96, L"%s %02u:%02u:%02u",
                       c->wsLive ? L"WS" : L"OK",
                       c->lastUpdateTime.wHour, c->lastUpdateTime.wMinute, c->lastUpdateTime.wSecond);
        } else {
            set_lv_cell_if_changed(lv, row, 4, c->fetching ? L"..." : L"--");
            set_lv_cell_if_changed(lv, row, 5, L"--");
            lstrcpynW(status, c->lastError[0] ? c->lastError : (c->enabled ? L"Loading..." : L"Hidden"), 96);
        }

        /* Value / Alert summary column */
        if (c->holdingQty > 0.0 && c->ok) {
            wchar_t vs[32]; format_price(c->price * c->holdingQty, 2, 1, 0, vs, 32);
            _snwprintf(valAl, 64, L"%s%s", vs, (c->alertAbove > 0 || c->alertBelow > 0) ? L" [A]" : L"");
        } else if (c->alertAbove > 0.0 || c->alertBelow > 0.0) {
            lstrcpyW(valAl, L"[Alert]");
        } else {
            lstrcpyW(valAl, L"-");
        }
        set_lv_cell_if_changed(lv, row, 6, valAl);
        set_lv_cell_if_changed(lv, row, 7, status);
    }

    if (!sameShape) {
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lv, NULL, FALSE);
    }
    g_populating = 0;
}

static void sync_controls_from_cfg(HWND w)
{
    static AppConfig snap;
    EnterCriticalSection(&g_stateCs);
    snap = g_cfg;
    LeaveCriticalSection(&g_stateCs);

    wchar_t ivStr[16], opStr[16], rotStr[16];
    _snwprintf(ivStr, 16, L"%lu", (unsigned long)snap.globalIntervalSec);
    SetDlgItemTextW(w, ID_EDT_INTERVAL, ivStr);
    _snwprintf(opStr, 16, L"%d", snap.widgetOpacity);
    SetDlgItemTextW(w, ID_EDT_WOPACITY, opStr);
    _snwprintf(rotStr, 16, L"%lu", (unsigned long)snap.autoRotateSec);
    SetDlgItemTextW(w, ID_EDT_ROTATE, rotStr);

    CheckDlgButton(w, ID_CHK_PCT,        snap.showChangePct      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_COLOR,      snap.colorByChange      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_REFICON,    snap.showRefreshIcon    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_COMPACT,    snap.compactPrices      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_TRAY,       snap.showTrayIcon       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_AUTOSTART,  app_autostart_is_on()   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_WS,         snap.enableWebSocket    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_ALERTSND,   snap.alertSound         ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_LOG,        snap.enableLogging      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_TASKBAR,    snap.showTaskbarBar     ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_WIDGET,     snap.showWidget         ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_WTOPMOST,   snap.widgetTopmost      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_WLOCK,      snap.widgetLocked       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_WCLICKTHRU, snap.widgetClickThrough ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_HOTKEYS,    snap.enableHotkeys      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_FLASH,      snap.enableTickFlash    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_CHK_SPARK,      snap.showSparkline      ? BST_CHECKED : BST_UNCHECKED);

    SendDlgItemMessageW(w, ID_CMB_SHOWON,  CB_SETCURSEL, (WPARAM)snap.showOn, 0);
    SendDlgItemMessageW(w, ID_CMB_CLICK,   CB_SETCURSEL, (WPARAM)snap.clickAction, 0);
    SendDlgItemMessageW(w, ID_CMB_WLAYOUT, CB_SETCURSEL, (WPARAM)snap.widgetLayout, 0);

    wchar_t cmcW[128], cgW[128];
    utf8_to_wide(snap.cmcApiKey, cmcW, 128);
    utf8_to_wide(snap.cgApiKey,  cgW,  128);
    SetDlgItemTextW(w, ID_EDT_CMC_KEY, cmcW);
    SetDlgItemTextW(w, ID_EDT_CG_KEY,  cgW);

    populate_coin_list(w);
}

static void apply_global_settings(HWND w)
{
    wchar_t ivBuf[16] = {0}, opBuf[16] = {0}, rotBuf[16] = {0}, cmcW[128] = {0}, cgW[128] = {0};
    GetDlgItemTextW(w, ID_EDT_INTERVAL, ivBuf, 16);
    int iv = _wtoi(ivBuf);
    if (iv < MIN_INTERVAL_SEC) iv = MIN_INTERVAL_SEC;
    if (iv > MAX_INTERVAL_SEC) iv = MAX_INTERVAL_SEC;
    _snwprintf(ivBuf, 16, L"%d", iv);
    SetDlgItemTextW(w, ID_EDT_INTERVAL, ivBuf);

    GetDlgItemTextW(w, ID_EDT_WOPACITY, opBuf, 16);
    int op = _wtoi(opBuf);
    if (op < 30) op = 30;
    if (op > 100) op = 100;
    _snwprintf(opBuf, 16, L"%d", op);
    SetDlgItemTextW(w, ID_EDT_WOPACITY, opBuf);

    GetDlgItemTextW(w, ID_EDT_ROTATE, rotBuf, 16);
    int rot = _wtoi(rotBuf);
    if (rot < 0) rot = 0;
    if (rot > 120) rot = 120;
    _snwprintf(rotBuf, 16, L"%d", rot);
    SetDlgItemTextW(w, ID_EDT_ROTATE, rotBuf);

    GetDlgItemTextW(w, ID_EDT_CMC_KEY, cmcW, 128);
    GetDlgItemTextW(w, ID_EDT_CG_KEY,  cgW,  128);

    EnterCriticalSection(&g_stateCs);
    DWORD oldIv = g_cfg.globalIntervalSec;
    g_cfg.globalIntervalSec  = (DWORD)iv;
    g_cfg.showChangePct      = (IsDlgButtonChecked(w, ID_CHK_PCT)        == BST_CHECKED);
    g_cfg.colorByChange      = (IsDlgButtonChecked(w, ID_CHK_COLOR)      == BST_CHECKED);
    g_cfg.showRefreshIcon    = (IsDlgButtonChecked(w, ID_CHK_REFICON)    == BST_CHECKED);
    g_cfg.compactPrices      = (IsDlgButtonChecked(w, ID_CHK_COMPACT)    == BST_CHECKED);
    g_cfg.showTrayIcon       = (IsDlgButtonChecked(w, ID_CHK_TRAY)       == BST_CHECKED);
    g_cfg.enableWebSocket    = (IsDlgButtonChecked(w, ID_CHK_WS)         == BST_CHECKED);
    g_cfg.alertSound         = (IsDlgButtonChecked(w, ID_CHK_ALERTSND)   == BST_CHECKED);
    g_cfg.enableLogging      = (IsDlgButtonChecked(w, ID_CHK_LOG)        == BST_CHECKED);
    g_cfg.showTaskbarBar     = (IsDlgButtonChecked(w, ID_CHK_TASKBAR)    == BST_CHECKED);
    g_cfg.showWidget         = (IsDlgButtonChecked(w, ID_CHK_WIDGET)     == BST_CHECKED);
    g_cfg.widgetTopmost      = (IsDlgButtonChecked(w, ID_CHK_WTOPMOST)   == BST_CHECKED);
    g_cfg.widgetLocked       = (IsDlgButtonChecked(w, ID_CHK_WLOCK)      == BST_CHECKED);
    g_cfg.widgetClickThrough = (IsDlgButtonChecked(w, ID_CHK_WCLICKTHRU) == BST_CHECKED);
    if (g_cfg.widgetClickThrough) g_cfg.widgetLocked = 1;
    g_cfg.enableHotkeys      = (IsDlgButtonChecked(w, ID_CHK_HOTKEYS)    == BST_CHECKED);
    g_cfg.enableTickFlash    = (IsDlgButtonChecked(w, ID_CHK_FLASH)      == BST_CHECKED);
    g_cfg.showSparkline      = (IsDlgButtonChecked(w, ID_CHK_SPARK)      == BST_CHECKED);
    g_cfg.widgetOpacity      = op;
    g_cfg.autoRotateSec      = (DWORD)rot;

    int sw = (int)SendDlgItemMessageW(w, ID_CMB_SHOWON, CB_GETCURSEL, 0, 0);
    if (sw >= SHOW_MAIN && sw <= SHOW_OTHERS) g_cfg.showOn = (ShowWhere)sw;
    int ca = (int)SendDlgItemMessageW(w, ID_CMB_CLICK, CB_GETCURSEL, 0, 0);
    if (ca >= CLICK_OPEN_CHART && ca <= CLICK_REFRESH) g_cfg.clickAction = (CoinClickAction)ca;
    int wl = (int)SendDlgItemMessageW(w, ID_CMB_WLAYOUT, CB_GETCURSEL, 0, 0);
    if (wl >= WIDGET_VERTICAL && wl <= WIDGET_HORIZONTAL) g_cfg.widgetLayout = (WidgetLayout)wl;

    wide_to_utf8(cmcW, g_cfg.cmcApiKey, sizeof(g_cfg.cmcApiKey));
    wide_to_utf8(cgW,  g_cfg.cgApiKey,  sizeof(g_cfg.cgApiKey));

    if (oldIv != g_cfg.globalIntervalSec) {
        for (int i = 0; i < g_cfg.coinCount; i++) g_cfg.coins[i].nextFetchMs = 0;
    }
    config_save(&g_cfg);
    LeaveCriticalSection(&g_stateCs);

    app_autostart_set(IsDlgButtonChecked(w, ID_CHK_AUTOSTART) == BST_CHECKED);
    log_set_enabled(IsDlgButtonChecked(w, ID_CHK_LOG) == BST_CHECKED);
    app_update_hotkeys();
    poller_wake();
    ws_reload();
    taskbar_refresh(1);
    widget_refresh(1);
    populate_coin_list(w);
}

/* Collect selected coin indices (mapped through g_viewMap) */
static int collect_selected_coins(HWND lv, int *outIdx, int maxOut)
{
    int n = 0, row = -1;
    while (n < maxOut && (row = ListView_GetNextItem(lv, row, LVNI_SELECTED)) >= 0) {
        if (row < g_viewCount) outIdx[n++] = g_viewMap[row];
    }
    return n;
}

static void delete_coins_by_indices(HWND w, const int *indices, int count)
{
    if (count <= 0) return;
    if (count > 1) {
        wchar_t msg[96];
        _snwprintf(msg, 96, L"Delete %d selected coins from your watchlist?", count);
        if (MessageBoxW(w, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    BYTE mark[MAX_COINS] = {0};
    for (int i = 0; i < count; i++)
        if (indices[i] >= 0 && indices[i] < MAX_COINS) mark[indices[i]] = 1;

    EnterCriticalSection(&g_stateCs);
    int dst = 0;
    for (int i = 0; i < g_cfg.coinCount; i++) {
        if (!mark[i]) {
            if (dst != i) g_cfg.coins[dst] = g_cfg.coins[i];
            dst++;
        }
    }
    g_cfg.coinCount = dst;
    config_save(&g_cfg);
    LeaveCriticalSection(&g_stateCs);

    ws_reload();
    taskbar_refresh(1);
    widget_refresh(1);
    populate_coin_list(w);
}

static void set_coins_enabled_by_indices(HWND w, const int *indices, int count, int enable)
{
    if (count <= 0) return;
    EnterCriticalSection(&g_stateCs);
    for (int i = 0; i < count; i++) {
        int idx = indices[i];
        if (idx >= 0 && idx < g_cfg.coinCount) {
            g_cfg.coins[idx].enabled = enable;
            if (enable) g_cfg.coins[idx].nextFetchMs = 0;
        }
    }
    config_save(&g_cfg);
    LeaveCriticalSection(&g_stateCs);
    poller_wake();
    ws_reload();
    taskbar_refresh(1);
    widget_refresh(1);
    populate_coin_list(w);
}

static void edit_coin_at(HWND w, int coinIdx)
{
    CoinItem item;
    int valid = 0;
    EnterCriticalSection(&g_stateCs);
    if (coinIdx >= 0 && coinIdx < g_cfg.coinCount) { item = g_cfg.coins[coinIdx]; valid = 1; }
    LeaveCriticalSection(&g_stateCs);
    if (!valid || !open_edit_coin_modal(w, coinIdx, &item)) return;

    EnterCriticalSection(&g_stateCs);
    if (coinIdx < g_cfg.coinCount) {
        int changed = (g_cfg.coins[coinIdx].provider != item.provider ||
                       lstrcmpW(g_cfg.coins[coinIdx].query, item.query) != 0 ||
                       lstrcmpW(g_cfg.coins[coinIdx].chain, item.chain) != 0);
        if (changed && item.lastUpdateMs == g_cfg.coins[coinIdx].lastUpdateMs) {
            item.ok = 0;
            item.resolvedSource[0] = 0;
            item.resolvedSymbol[0] = 0;
        }
        item.nextFetchMs = 0;
        item.failCount = 0;
        item.lastError[0] = 0;
        g_cfg.coins[coinIdx] = item;
        config_save(&g_cfg);
    }
    LeaveCriticalSection(&g_stateCs);
    poller_wake();
    ws_reload();
    taskbar_refresh(1);
    widget_refresh(1);
    populate_coin_list(w);
}

static void move_coin(HWND w, int coinIdx, int dir)
{
    int dst = coinIdx + dir;
    int moved = 0;
    EnterCriticalSection(&g_stateCs);
    if (coinIdx >= 0 && dst >= 0 && coinIdx < g_cfg.coinCount && dst < g_cfg.coinCount) {
        CoinItem tmp = g_cfg.coins[coinIdx];
        g_cfg.coins[coinIdx] = g_cfg.coins[dst];
        g_cfg.coins[dst] = tmp;
        config_save(&g_cfg);
        moved = 1;
    }
    LeaveCriticalSection(&g_stateCs);
    if (moved) {
        taskbar_refresh(1);
        widget_refresh(1);
        populate_coin_list(w);
        HWND lv = GetDlgItem(w, ID_LV_COINS);
        for (int r = 0; r < g_viewCount; r++) {
            if (g_viewMap[r] == dst) {
                ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(lv, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(lv, r, FALSE);
                break;
            }
        }
    }
}

static void show_listview_context_menu(HWND w, HWND lv)
{
    int sel[MAX_COINS];
    int cnt = collect_selected_coins(lv, sel, MAX_COINS);
    if (cnt <= 0) return;

    HMENU m = CreatePopupMenu();
    if (cnt == 1) {
        int idx = sel[0];
        int isEn = 0;
        wchar_t url[256] = {0}, qcopy[128] = {0};
        EnterCriticalSection(&g_stateCs);
        if (idx < g_cfg.coinCount) {
            isEn = g_cfg.coins[idx].enabled;
            lstrcpynW(url, g_cfg.coins[idx].pairUrl, 256);
            lstrcpynW(qcopy, g_cfg.coins[idx].query, 128);
        }
        LeaveCriticalSection(&g_stateCs);

        AppendMenuW(m, MF_STRING, LVCMD_REFRESH, L"Refresh This Coin");
        AppendMenuW(m, MF_STRING, LVCMD_EDIT,    L"Edit Coin...");
        if (url[0]) AppendMenuW(m, MF_STRING, LVCMD_CHART, L"Open Chart in Browser");
        AppendMenuW(m, MF_STRING, LVCMD_COPY,    L"Copy Pair / Contract");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, isEn ? LVCMD_HIDE : LVCMD_SHOW,
                    isEn ? L"Hide from Taskbar / Widget" : L"Show on Taskbar / Widget");
        AppendMenuW(m, MF_STRING, LVCMD_UP,      L"Move Up");
        AppendMenuW(m, MF_STRING, LVCMD_DOWN,    L"Move Down");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, LVCMD_DELETE,  L"Delete Coin");

        POINT pt; GetCursorPos(&pt);
        int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, w, NULL);
        DestroyMenu(m);

        if (cmd == LVCMD_REFRESH)     poller_trigger_refresh_mask(1ULL << idx);
        else if (cmd == LVCMD_EDIT)   edit_coin_at(w, idx);
        else if (cmd == LVCMD_CHART && url[0]) ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        else if (cmd == LVCMD_COPY)   copy_text_to_clipboard(w, qcopy);
        else if (cmd == LVCMD_SHOW)   set_coins_enabled_by_indices(w, sel, 1, 1);
        else if (cmd == LVCMD_HIDE)   set_coins_enabled_by_indices(w, sel, 1, 0);
        else if (cmd == LVCMD_UP)     move_coin(w, idx, -1);
        else if (cmd == LVCMD_DOWN)   move_coin(w, idx, +1);
        else if (cmd == LVCMD_DELETE) delete_coins_by_indices(w, sel, 1);
    } else {
        wchar_t sRef[64], sShow[64], sHide[64], sDel[64];
        _snwprintf(sRef,  64, L"Refresh Selected (%d)", cnt);
        _snwprintf(sShow, 64, L"Show / Check Selected (%d)", cnt);
        _snwprintf(sHide, 64, L"Hide / Uncheck Selected (%d)", cnt);
        _snwprintf(sDel,  64, L"Delete Selected (%d)...", cnt);

        AppendMenuW(m, MF_STRING, LVCMD_REFRESH, sRef);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, LVCMD_SHOW, sShow);
        AppendMenuW(m, MF_STRING, LVCMD_HIDE, sHide);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, LVCMD_DELETE, sDel);

        POINT pt; GetCursorPos(&pt);
        int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, w, NULL);
        DestroyMenu(m);

        if (cmd == LVCMD_REFRESH) {
            uint64_t mask = 0;
            for (int i = 0; i < cnt; i++) if (sel[i] >= 0 && sel[i] < MAX_COINS && sel[i] < 64) mask |= (1ULL << sel[i]);
            poller_trigger_refresh_mask(mask);
        } else if (cmd == LVCMD_SHOW)   set_coins_enabled_by_indices(w, sel, cnt, 1);
        else if (cmd == LVCMD_HIDE)     set_coins_enabled_by_indices(w, sel, cnt, 0);
        else if (cmd == LVCMD_DELETE)   delete_coins_by_indices(w, sel, cnt);
    }
}

static LRESULT CALLBACK settings_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        ensure_ui_resources(w);

        /* Main 2-Tab Control */
        HWND tab = mk_ctrl(w, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, 0, 12, 12, 770, 500, ID_TAB_MAIN, 0);
        TCITEMW ti; ZeroMemory(&ti, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = L"  Watchlist && Coins  ";
        TabCtrl_InsertItem(tab, 0, &ti);
        ti.pszText = L"  Settings, Widget && APIs  ";
        TabCtrl_InsertItem(tab, 1, &ti);

        /* ---- TAB 0: Watchlist & Offline Search ---- */
        mk_ctrl(w, L"STATIC", L"Search:", 0, 0, 24, 47, 50, 20, ID_LBL_SEARCH, 0);
        HWND edSrch = mk_ctrl(w, L"EDIT", g_filterText, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 76, 44, 230, 24, ID_EDT_SEARCH, 0);
        SendMessageW(edSrch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter label, contract, exchange, URL...");
        mk_ctrl(w, L"BUTTON", L"x", BS_PUSHBUTTON | WS_TABSTOP, 0, 310, 44, 28, 24, ID_BTN_SEARCH_CLR, 0);
        mk_ctrl(w, L"STATIC", L"", 0, 0, 346, 47, 220, 20, ID_LBL_COUNT, 0);
        mk_ctrl(w, L"BUTTON", L"Select All",   BS_PUSHBUTTON | WS_TABSTOP, 0, 594, 44, 84, 24, ID_BTN_SEL_ALL, 0);
        mk_ctrl(w, L"BUTTON", L"Unselect All", BS_PUSHBUTTON | WS_TABSTOP, 0, 682, 44, 88, 24, ID_BTN_UNSEL_ALL, 0);

        /* Multi-select ListView (NO LVS_SINGLESEL!) */
        HWND lv = mk_ctrl(w, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                          0, 24, 76, 746, 360, ID_LV_COINS, 0);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        static const struct { const wchar_t *t; int w; } COLS[] = {
            { L"Show / Label", 102 }, { L"Pair / Contract", 168 }, { L"Source", 98 },
            { L"Interval", 76 }, { L"Price", 90 }, { L"24h %", 60 }, { L"Value / Alert", 86 }, { L"Status", 94 }
        };
        for (int i = 0; i < 8; i++) {
            LVCOLUMNW col; ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)COLS[i].t;
            col.cx = S(COLS[i].w);
            ListView_InsertColumn(lv, i, &col);
        }

        mk_ctrl(w, L"BUTTON", L"+ Add Coin",  BS_PUSHBUTTON | WS_TABSTOP, 0, 24,  448, 86, 26, ID_BTN_ADD, 1);
        mk_ctrl(w, L"BUTTON", L"Edit",        BS_PUSHBUTTON | WS_TABSTOP, 0, 116, 448, 58, 26, ID_BTN_EDIT, 0);
        mk_ctrl(w, L"BUTTON", L"Delete",      BS_PUSHBUTTON | WS_TABSTOP, 0, 180, 448, 64, 26, ID_BTN_DEL, 0);
        mk_ctrl(w, L"BUTTON", L"Up",          BS_PUSHBUTTON | WS_TABSTOP, 0, 254, 448, 56, 26, ID_BTN_UP, 0);
        mk_ctrl(w, L"BUTTON", L"Down",        BS_PUSHBUTTON | WS_TABSTOP, 0, 316, 448, 64, 26, ID_BTN_DOWN, 0);
        mk_ctrl(w, L"BUTTON", L"Refresh All", BS_PUSHBUTTON | WS_TABSTOP, 0, 476, 448, 106, 26, ID_BTN_REFRESH_ALL, 1);
        mk_ctrl(w, L"BUTTON", L"Export JSON", BS_PUSHBUTTON | WS_TABSTOP, 0, 588, 448, 88, 26, ID_BTN_EXPORT, 0);
        mk_ctrl(w, L"BUTTON", L"Import JSON", BS_PUSHBUTTON | WS_TABSTOP, 0, 682, 448, 88, 26, ID_BTN_IMPORT, 0);

        /* ---- TAB 1: Settings, Floating Widget & API Keys ---- */
        int y = 46;
        mk_ctrl(w, L"BUTTON", L"Auto-Refresh && Taskbar Display", BS_GROUPBOX, 0, 24, y, 746, 152, ID_GRP_DISPLAY, 1);

        y += 24;
        mk_ctrl(w, L"STATIC", L"Global Interval (sec):", 0, 0, 38, y + 3, 130, 20, ID_LBL_IV, 0);
        mk_ctrl(w, L"EDIT", L"15", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 170, y, 54, 22, ID_EDT_INTERVAL, 0);
        mk_ctrl(w, L"BUTTON", L"5s",  BS_PUSHBUTTON | WS_TABSTOP, 0, 232, y, 36, 22, ID_BTN_IV_5, 0);
        mk_ctrl(w, L"BUTTON", L"15s", BS_PUSHBUTTON | WS_TABSTOP, 0, 272, y, 38, 22, ID_BTN_IV_15, 0);
        mk_ctrl(w, L"BUTTON", L"30s", BS_PUSHBUTTON | WS_TABSTOP, 0, 314, y, 38, 22, ID_BTN_IV_30, 0);
        mk_ctrl(w, L"BUTTON", L"60s", BS_PUSHBUTTON | WS_TABSTOP, 0, 356, y, 38, 22, ID_BTN_IV_60, 0);
        mk_ctrl(w, L"BUTTON", L"5m",  BS_PUSHBUTTON | WS_TABSTOP, 0, 398, y, 36, 22, ID_BTN_IV_300, 0);

        mk_ctrl(w, L"STATIC", L"Show on:", 0, 0, 466, y + 3, 62, 20, ID_LBL_SHOWON, 0);
        HWND cbOn = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 532, y, 224, 120, ID_CMB_SHOWON, 0);
        SendMessageW(cbOn, CB_ADDSTRING, 0, (LPARAM)L"Main taskbar");
        SendMessageW(cbOn, CB_ADDSTRING, 0, (LPARAM)L"All taskbars");
        SendMessageW(cbOn, CB_ADDSTRING, 0, (LPARAM)L"Other monitors only");

        y += 32;
        mk_ctrl(w, L"BUTTON", L"Show 24h % change",       BS_AUTOCHECKBOX | WS_TABSTOP, 0, 38,  y, 172, 20, ID_CHK_PCT, 0);
        mk_ctrl(w, L"BUTTON", L"Color by 24h change",     BS_AUTOCHECKBOX | WS_TABSTOP, 0, 220, y, 172, 20, ID_CHK_COLOR, 0);
        mk_ctrl(w, L"STATIC", L"Coin click:", 0, 0, 466, y + 2, 65, 20, ID_LBL_CLICK, 0);
        HWND cbCk = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 532, y - 2, 224, 120, ID_CMB_CLICK, 0);
        SendMessageW(cbCk, CB_ADDSTRING, 0, (LPARAM)L"Open Chart in Browser");
        SendMessageW(cbCk, CB_ADDSTRING, 0, (LPARAM)L"Open Settings");
        SendMessageW(cbCk, CB_ADDSTRING, 0, (LPARAM)L"Refresh Now");

        y += 28;
        mk_ctrl(w, L"BUTTON", L"Manual refresh icon",     BS_AUTOCHECKBOX | WS_TABSTOP, 0, 38,  y, 172, 20, ID_CHK_REFICON, 0);
        mk_ctrl(w, L"BUTTON", L"Compact prices (K/M)",    BS_AUTOCHECKBOX | WS_TABSTOP, 0, 220, y, 152, 20, ID_CHK_COMPACT, 0);
        mk_ctrl(w, L"BUTTON", L"Tick flash",              BS_AUTOCHECKBOX | WS_TABSTOP, 0, 378, y, 86,  20, ID_CHK_FLASH, 0);
        mk_ctrl(w, L"BUTTON", L"Start with Windows",      BS_AUTOCHECKBOX | WS_TABSTOP, 0, 466, y, 146, 20, ID_CHK_AUTOSTART, 0);
        mk_ctrl(w, L"STATIC", L"Rotate(s):", 0, 0, 622, y + 2, 64, 20, ID_LBL_ROTATE, 0);
        mk_ctrl(w, L"EDIT",   L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 690, y - 1, 66, 22, ID_EDT_ROTATE, 0);

        y += 28;
        mk_ctrl(w, L"BUTTON", L"System Tray icon",        BS_AUTOCHECKBOX | WS_TABSTOP, 0, 38,  y, 148, 20, ID_CHK_TRAY, 0);
        mk_ctrl(w, L"BUTTON", L"Real-Time WebSocket",     BS_AUTOCHECKBOX | WS_TABSTOP, 0, 192, y, 168, 20, ID_CHK_WS, 0);
        mk_ctrl(w, L"BUTTON", L"Alert sound",             BS_AUTOCHECKBOX | WS_TABSTOP, 0, 366, y, 96,  20, ID_CHK_ALERTSND, 0);
        mk_ctrl(w, L"BUTTON", L"Debug Log",               BS_AUTOCHECKBOX | WS_TABSTOP, 0, 466, y, 92,  20, ID_CHK_LOG, 0);
        mk_ctrl(w, L"BUTTON", L"Open Log",                BS_PUSHBUTTON   | WS_TABSTOP, 0, 566, y - 2, 86, 22, ID_BTN_OPEN_LOG, 0);

        /* Section: Floating Widget & Hotkeys */
        y = 210;
        mk_ctrl(w, L"BUTTON", L"Always-on-Top Floating Widget && Global Hotkeys", BS_GROUPBOX, 0, 24, y, 746, 92, ID_GRP_WIDGET, 1);

        y += 24;
        mk_ctrl(w, L"BUTTON", L"Show on Taskbar",         BS_AUTOCHECKBOX | WS_TABSTOP, 0, 38,  y, 130, 20, ID_CHK_TASKBAR, 0);
        mk_ctrl(w, L"BUTTON", L"Show Floating Widget",    BS_AUTOCHECKBOX | WS_TABSTOP, 0, 174, y, 168, 20, ID_CHK_WIDGET, 1);
        mk_ctrl(w, L"STATIC", L"Layout:", 0, 0, 350, y + 3, 46, 20, ID_LBL_WLAYOUT, 0);
        HWND cbWl = mk_ctrl(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 398, y - 1, 122, 100, ID_CMB_WLAYOUT, 0);
        SendMessageW(cbWl, CB_ADDSTRING, 0, (LPARAM)L"Vertical Card");
        SendMessageW(cbWl, CB_ADDSTRING, 0, (LPARAM)L"Horizontal Pill");
        mk_ctrl(w, L"STATIC", L"Opacity %:", 0, 0, 530, y + 3, 66, 20, ID_LBL_WOPACITY, 0);
        mk_ctrl(w, L"EDIT", L"88", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 598, y, 44, 22, ID_EDT_WOPACITY, 0);
        mk_ctrl(w, L"BUTTON", L"Reset Pos", BS_PUSHBUTTON | WS_TABSTOP, 0, 654, y - 1, 102, 22, ID_BTN_WRESET, 0);

        y += 30;
        mk_ctrl(w, L"BUTTON", L"Always on Top",               BS_AUTOCHECKBOX | WS_TABSTOP, 0, 38,  y, 114, 20, ID_CHK_WTOPMOST, 0);
        mk_ctrl(w, L"BUTTON", L"Lock Pos",                    BS_AUTOCHECKBOX | WS_TABSTOP, 0, 156, y, 76,  20, ID_CHK_WLOCK, 0);
        mk_ctrl(w, L"BUTTON", L"Click-Through (Ctrl+Shift+L)",BS_AUTOCHECKBOX | WS_TABSTOP, 0, 236, y, 192, 20, ID_CHK_WCLICKTHRU, 0);
        mk_ctrl(w, L"BUTTON", L"Sparklines",                  BS_AUTOCHECKBOX | WS_TABSTOP, 0, 434, y, 88,  20, ID_CHK_SPARK, 0);
        mk_ctrl(w, L"BUTTON", L"Hotkeys (Ctrl+Shift+C/L/R)",  BS_AUTOCHECKBOX | WS_TABSTOP, 0, 526, y, 226, 20, ID_CHK_HOTKEYS, 0);

        /* Section: Optional API Keys */
        y = 314;
        mk_ctrl(w, L"BUTTON", L"Optional API Keys (CEX && DexScreener need NO key)", BS_GROUPBOX, 0, 24, y, 746, 86, ID_GRP_API, 1);

        y += 24;
        mk_ctrl(w, L"STATIC", L"CoinMarketCap Key:", 0, 0, 38, y + 3, 128, 20, ID_LBL_CMC, 0);
        mk_ctrl(w, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 170, y, 476, 22, ID_EDT_CMC_KEY, 0);
        mk_ctrl(w, L"BUTTON", L"Get Free Key", BS_PUSHBUTTON | WS_TABSTOP, 0, 654, y, 102, 22, ID_BTN_CMC_HELP, 0);

        y += 28;
        mk_ctrl(w, L"STATIC", L"CoinGecko Demo Key:", 0, 0, 38, y + 3, 128, 20, ID_LBL_CG, 0);
        mk_ctrl(w, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 170, y, 476, 22, ID_EDT_CG_KEY, 0);

        /* Author & License credit line (GNU AGPL-3.0) */
        wchar_t aboutTxt[220];
        _snwprintf(aboutTxt, 220, L"%s v%s | by %s (2026) | Based on TraySense (DMTWEAKS) | GNU AGPL-3.0",
                   APP_FULL_NAME, APP_VERSION, APP_AUTHOR);
        mk_ctrl(w, L"STATIC", aboutTxt, SS_NOPREFIX, 0, 26, 412, 744, 20, ID_LBL_ABOUT, 0);

        /* Bottom Bar */
        wchar_t cfgPath[MAX_PATH], hint[MAX_PATH + 32];
        config_get_path(cfgPath, MAX_PATH);
        _snwprintf(hint, MAX_PATH + 32, L"Config: %s", cfgPath);
        mk_ctrl(w, L"STATIC", hint, SS_PATHELLIPSIS, 0, 12, 520, 460, 20, ID_LBL_CFGPATH, 0);
        mk_ctrl(w, L"BUTTON", L"Apply",        BS_PUSHBUTTON | WS_TABSTOP,    0, 500, 516, 84, 28, ID_BTN_APPLY, 0);
        mk_ctrl(w, L"BUTTON", L"Save && Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 590, 516, 98, 28, ID_BTN_SAVE_CLOSE, 1);
        mk_ctrl(w, L"BUTTON", L"Close",        BS_PUSHBUTTON | WS_TABSTOP,    0, 694, 516, 82, 28, ID_BTN_CLOSE, 0);

        TabCtrl_SetCurSel(tab, g_curTab);
        show_tab(w, g_curTab);
        layout_settings_window(w);
        sync_controls_from_cfg(w);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = S(808);
        mmi->ptMinTrackSize.y = S(570);
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) layout_settings_window(w);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == ID_EDT_SEARCH && code == EN_CHANGE) {
            GetDlgItemTextW(w, ID_EDT_SEARCH, g_filterText, 128);
            populate_coin_list(w);
            return 0;
        }
        if (id == ID_BTN_SEARCH_CLR) {
            g_filterText[0] = 0;
            SetDlgItemTextW(w, ID_EDT_SEARCH, L"");
            populate_coin_list(w);
            return 0;
        }
        if (id == ID_BTN_SEL_ALL || id == ID_BTN_UNSEL_ALL) {
            set_coins_enabled_by_indices(w, g_viewMap, g_viewCount, (id == ID_BTN_SEL_ALL) ? 1 : 0);
            return 0;
        }
        if (id == ID_BTN_IV_5)   { SetDlgItemTextW(w, ID_EDT_INTERVAL, L"5");   return 0; }
        if (id == ID_BTN_IV_15)  { SetDlgItemTextW(w, ID_EDT_INTERVAL, L"15");  return 0; }
        if (id == ID_BTN_IV_30)  { SetDlgItemTextW(w, ID_EDT_INTERVAL, L"30");  return 0; }
        if (id == ID_BTN_IV_60)  { SetDlgItemTextW(w, ID_EDT_INTERVAL, L"60");  return 0; }
        if (id == ID_BTN_IV_300) { SetDlgItemTextW(w, ID_EDT_INTERVAL, L"300"); return 0; }
        if (id == ID_BTN_CMC_HELP) {
            ShellExecuteW(NULL, L"open", L"https://pro.coinmarketcap.com/signup", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (id == ID_BTN_OPEN_LOG) {
            wchar_t lpPath[MAX_PATH];
            log_get_path(lpPath, MAX_PATH);
            HANDLE f = CreateFileW(lpPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            ShellExecuteW(NULL, L"open", lpPath, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (id == ID_BTN_WRESET) { widget_reset_position(); return 0; }
        if (id == ID_BTN_REFRESH_ALL) {
            LOGI(L"UI", L"Refresh All clicked in Settings");
            poller_trigger_refresh(1);
            return 0;
        }
        if (id == ID_BTN_APPLY)      { apply_global_settings(w); return 0; }
        if (id == ID_BTN_SAVE_CLOSE) { apply_global_settings(w); DestroyWindow(w); return 0; }
        if (id == ID_BTN_CLOSE)      { DestroyWindow(w); return 0; }

        if (id == ID_BTN_EXPORT) {
            wchar_t path[MAX_PATH] = L"traycoin_backup.json";
            OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = w;
            ofn.lpstrFilter = L"JSON Config (*.json)\0*.json\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"json";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                int ok = 0;
                EnterCriticalSection(&g_stateCs);
                ok = config_save_to(&g_cfg, path);
                LeaveCriticalSection(&g_stateCs);
                MessageBoxW(w, ok ? L"Exported configuration successfully." : L"Failed to export file.",
                            APP_NAME, ok ? MB_ICONINFORMATION : MB_ICONERROR);
            }
            return 0;
        }
        if (id == ID_BTN_IMPORT) {
            wchar_t path[MAX_PATH] = L"";
            OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = w;
            ofn.lpstrFilter = L"JSON Config (*.json)\0*.json\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                static AppConfig loaded;
                if (config_load_from(&loaded, path)) {
                    EnterCriticalSection(&g_stateCs);
                    g_cfg = loaded;
                    config_save(&g_cfg);
                    LeaveCriticalSection(&g_stateCs);
                    log_set_enabled(g_cfg.enableLogging);
                    app_update_hotkeys();
                    poller_wake();
                    ws_reload();
                    taskbar_refresh(1);
                    widget_refresh(1);
                    sync_controls_from_cfg(w);
                    MessageBoxW(w, L"Imported configuration successfully.", APP_NAME, MB_ICONINFORMATION);
                } else {
                    MessageBoxW(w, L"Invalid or unreadable JSON config file.", APP_NAME, MB_ICONERROR);
                }
            }
            return 0;
        }

        if (id == ID_BTN_ADD) {
            int count = 0;
            EnterCriticalSection(&g_stateCs); count = g_cfg.coinCount; LeaveCriticalSection(&g_stateCs);
            if (count >= MAX_COINS) {
                MessageBoxW(w, L"Maximum number of tracked coins reached (64).", APP_NAME, MB_ICONINFORMATION);
                return 0;
            }
            CoinItem item; ZeroMemory(&item, sizeof(item));
            item.enabled = 1;
            item.provider = PROV_AUTO;
            item.decimals = -1;
            /* Convenience: if user searched for something not found in watchlist, pre-fill it! */
            if (g_filterText[0] && g_viewCount == 0) {
                lstrcpynW(item.query, g_filterText, 128);
            }
            if (open_edit_coin_modal(w, -1, &item)) {
                EnterCriticalSection(&g_stateCs);
                if (g_cfg.coinCount < MAX_COINS) {
                    g_cfg.coins[g_cfg.coinCount++] = item;
                    config_save(&g_cfg);
                }
                LeaveCriticalSection(&g_stateCs);
                poller_wake();
                ws_reload();
                taskbar_refresh(1);
                widget_refresh(1);
                populate_coin_list(w);
            }
            return 0;
        }
        if (id == ID_BTN_EDIT) {
            int sel[MAX_COINS];
            if (collect_selected_coins(GetDlgItem(w, ID_LV_COINS), sel, MAX_COINS) >= 1)
                edit_coin_at(w, sel[0]);
            return 0;
        }
        if (id == ID_BTN_DEL) {
            int sel[MAX_COINS];
            int cnt = collect_selected_coins(GetDlgItem(w, ID_LV_COINS), sel, MAX_COINS);
            if (cnt > 0) delete_coins_by_indices(w, sel, cnt);
            return 0;
        }
        if (id == ID_BTN_UP || id == ID_BTN_DOWN) {
            int sel[MAX_COINS];
            if (collect_selected_coins(GetDlgItem(w, ID_LV_COINS), sel, MAX_COINS) == 1)
                move_coin(w, sel[0], (id == ID_BTN_UP) ? -1 : +1);
            return 0;
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == ID_TAB_MAIN && nm->code == TCN_SELCHANGE) {
            int t = TabCtrl_GetCurSel(nm->hwndFrom);
            show_tab(w, t == 1 ? 1 : 0);
            return 0;
        }
        if (nm->idFrom == ID_LV_COINS) {
            if (nm->code == LVN_ITEMCHANGED && !g_populating) {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
                    int checked = ((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                    if (lv->iItem >= 0 && lv->iItem < g_viewCount) {
                        int idx = g_viewMap[lv->iItem];
                        EnterCriticalSection(&g_stateCs);
                        if (idx >= 0 && idx < g_cfg.coinCount && g_cfg.coins[idx].enabled != checked) {
                            g_cfg.coins[idx].enabled = checked;
                            if (checked) g_cfg.coins[idx].nextFetchMs = 0;
                            config_save(&g_cfg);
                        }
                        LeaveCriticalSection(&g_stateCs);
                        poller_wake();
                        ws_reload();
                        taskbar_refresh(1);
                        widget_refresh(1);
                    }
                }
            } else if (nm->code == NM_DBLCLK) {
                int sel[MAX_COINS];
                if (collect_selected_coins(nm->hwndFrom, sel, MAX_COINS) >= 1)
                    edit_coin_at(w, sel[0]);
            } else if (nm->code == NM_RCLICK) {
                show_listview_context_menu(w, nm->hwndFrom);
            } else if (nm->code == LVN_KEYDOWN) {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
                if (kd->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    ListView_SetItemState(nm->hwndFrom, -1, LVIS_SELECTED, LVIS_SELECTED);
                } else if (kd->wVKey == VK_DELETE) {
                    SendMessageW(w, WM_COMMAND, ID_BTN_DEL, 0);
                }
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        g_setWnd = NULL;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void ui_settings_open(void)
{
    LOGI(L"UI", L"Opening Settings window");
    SetForegroundWindow(g_wnd);
    if (g_setWnd && IsWindow(g_setWnd)) {
        if (IsIconic(g_setWnd)) ShowWindow(g_setWnd, SW_RESTORE);
        else ShowWindow(g_setWnd, SW_SHOWNORMAL);
        SetWindowPos(g_setWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(g_setWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        BringWindowToTop(g_setWnd);
        SetForegroundWindow(g_setWnd);
        return;
    }
    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = settings_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TrayCoinSettings";
        RegisterClassW(&wc);
        reg = 1;
    }

    ensure_ui_resources(NULL);
    int W = S(824), H = S(590);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int x = (sx - W) / 2, y = (sy - H) / 2;
    if (x < 20) x = 20;
    if (y < 20) y = 20;

    g_setWnd = CreateWindowExW(0, L"TrayCoinSettings", APP_FULL_NAME,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               x, y, W, H, NULL, NULL, g_hinst, NULL);
    if (g_setWnd) {
        HICON appIc = taskbar_get_app_icon();
        if (appIc) {
            SendMessageW(g_setWnd, WM_SETICON, ICON_SMALL, (LPARAM)appIc);
            SendMessageW(g_setWnd, WM_SETICON, ICON_BIG,   (LPARAM)appIc);
        }
        ShowWindow(g_setWnd, SW_SHOWNORMAL);
        BringWindowToTop(g_setWnd);
        SetForegroundWindow(g_setWnd);
    }
}

void ui_settings_close(void)
{
    if (g_setWnd && IsWindow(g_setWnd)) DestroyWindow(g_setWnd);
    g_setWnd = NULL;
    if (g_uiFont) { DeleteObject(g_uiFont); g_uiFont = NULL; }
    if (g_uiBold) { DeleteObject(g_uiBold); g_uiBold = NULL; }
}

void ui_settings_notify_updated(void)
{
    if (g_setWnd && IsWindow(g_setWnd)) {
        populate_coin_list(g_setWnd);
    }
}

void ui_settings_sync_controls(void)
{
    if (g_setWnd && IsWindow(g_setWnd)) {
        sync_controls_from_cfg(g_setWnd);
    }
}
