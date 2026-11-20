/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "config.h"
#include "json.h"
#include "providers.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

int utf8_to_wide(const char *src, wchar_t *dst, int dstChars)
{
    if (!dst || dstChars <= 0) return 0;
    dst[0] = 0;
    if (!src || !src[0]) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstChars);
    if (n <= 0) { dst[dstChars - 1] = 0; return 0; }
    return n - 1;
}

int wide_to_utf8(const wchar_t *src, char *dst, int dstBytes)
{
    if (!dst || dstBytes <= 0) return 0;
    dst[0] = 0;
    if (!src || !src[0]) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstBytes, NULL, NULL);
    if (n <= 0) { dst[dstBytes - 1] = 0; return 0; }
    return n - 1;
}

void config_get_path(wchar_t *outPath, int cap)
{
    /* Portable mode: if config.json sits next to the executable, use it. */
    wchar_t exeDir[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, exeDir, MAX_PATH)) {
        wchar_t *slash = wcsrchr(exeDir, L'\\');
        if (slash) {
            slash[1] = 0;
            wchar_t portable[MAX_PATH];
            _snwprintf(portable, MAX_PATH, L"%sconfig.json", exeDir);
            portable[MAX_PATH - 1] = 0;
            DWORD attr = GetFileAttributesW(portable);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                lstrcpynW(outPath, portable, cap);
                return;
            }
        }
    }

    /* Standard mode: %APPDATA%\TrayCoin\config.json */
    wchar_t appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%s\\%s", appdata, APP_NAME);
        dir[MAX_PATH - 1] = 0;
        CreateDirectoryW(dir, NULL);
        _snwprintf(outPath, cap, L"%s\\config.json", dir);
        outPath[cap - 1] = 0;
        return;
    }

    /* Fallback next to exe */
    _snwprintf(outPath, cap, L"%sconfig.json", exeDir);
    outPath[cap - 1] = 0;
}

static void add_default_coin(AppConfig *cfg, ProviderId prov, const wchar_t *query, const wchar_t *label, int enabled)
{
    if (cfg->coinCount >= MAX_COINS) return;
    CoinItem *c = &cfg->coins[cfg->coinCount++];
    ZeroMemory(c, sizeof(*c));
    c->enabled = enabled;
    c->provider = prov;
    c->inputKind = provider_detect_input(query);
    lstrcpynW(c->query, query, 128);
    lstrcpynW(c->label, label, 24);
    c->decimals = -1;
    c->intervalSec = 0;
}

void config_init_defaults(AppConfig *cfg)
{
    ZeroMemory(cfg, sizeof(*cfg));
    cfg->globalIntervalSec = 15;
    cfg->enableWebSocket   = 1;
    cfg->showChangePct     = 1;
    cfg->colorByChange     = 1;
    cfg->showRefreshIcon   = 1;
    cfg->showCurrencySign  = 1;
    cfg->compactPrices      = 0;
    cfg->showTrayIcon       = 1;
    cfg->enableLogging      = 0;
    cfg->showTaskbarBar     = 1;
    cfg->showWidget         = 0;
    cfg->widgetLayout       = WIDGET_VERTICAL;
    cfg->widgetTopmost      = 1;
    cfg->widgetLocked       = 0;
    cfg->widgetClickThrough = 0;
    cfg->widgetOpacity      = 88;
    cfg->widgetX            = WIDGET_POS_UNSET;
    cfg->widgetY            = WIDGET_POS_UNSET;
    cfg->enableHotkeys      = 1;
    cfg->enableTickFlash    = 1;
    cfg->showSparkline      = 1;
    cfg->alertSound         = 1;
    cfg->autoRotateSec      = 0;
    cfg->showOn             = SHOW_MAIN;
    cfg->clickAction       = CLICK_OPEN_CHART;
    cfg->offsetX           = 0;

    add_default_coin(cfg, PROV_BINANCE, L"BTC/USDT", L"BTC", 1);
    add_default_coin(cfg, PROV_BINANCE, L"ETH/USDT", L"ETH", 1);
    add_default_coin(cfg, PROV_OKX,     L"SOL-USDT", L"SOL", 1);
}

int config_load_from(AppConfig *cfg, const wchar_t *path)
{
    config_init_defaults(cfg);

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (2 << 20)) {
        CloseHandle(f);
        return 0;
    }

    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz.QuadPart + 1);
    if (!buf) { CloseHandle(f); return 0; }

    DWORD got = 0;
    BOOL rd = ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL);
    CloseHandle(f);
    if (!rd || got == 0) { HeapFree(GetProcessHeap(), 0, buf); return 0; }
    buf[got] = '\0';

    JsonVal *root = json_parse(buf);
    HeapFree(GetProcessHeap(), 0, buf);
    if (!root || root->type != JSON_OBJECT) {
        if (root) json_free(root);
        return 0;
    }

    int iv = json_as_int(json_get(root, "globalIntervalSec"), (int)cfg->globalIntervalSec);
    if (iv < MIN_INTERVAL_SEC) iv = MIN_INTERVAL_SEC;
    if (iv > MAX_INTERVAL_SEC) iv = MAX_INTERVAL_SEC;
    cfg->globalIntervalSec = (DWORD)iv;

    cfg->enableWebSocket  = json_as_bool(json_get(root, "enableWebSocket"), cfg->enableWebSocket);
    cfg->showChangePct    = json_as_bool(json_get(root, "showChangePct"), cfg->showChangePct);
    cfg->colorByChange    = json_as_bool(json_get(root, "colorByChange"), cfg->colorByChange);
    cfg->showRefreshIcon  = json_as_bool(json_get(root, "showRefreshIcon"), cfg->showRefreshIcon);
    cfg->showCurrencySign = json_as_bool(json_get(root, "showCurrencySign"), cfg->showCurrencySign);
    cfg->compactPrices      = json_as_bool(json_get(root, "compactPrices"), cfg->compactPrices);
    cfg->showTrayIcon       = json_as_bool(json_get(root, "showTrayIcon"), cfg->showTrayIcon);
    cfg->enableLogging      = json_as_bool(json_get(root, "enableLogging"), cfg->enableLogging);
    cfg->showTaskbarBar     = json_as_bool(json_get(root, "showTaskbarBar"), cfg->showTaskbarBar);
    cfg->showWidget         = json_as_bool(json_get(root, "showWidget"), cfg->showWidget);
    int wlay = json_as_int(json_get(root, "widgetLayout"), (int)cfg->widgetLayout);
    if (wlay >= WIDGET_VERTICAL && wlay <= WIDGET_HORIZONTAL) cfg->widgetLayout = (WidgetLayout)wlay;
    cfg->widgetTopmost      = json_as_bool(json_get(root, "widgetTopmost"), cfg->widgetTopmost);
    cfg->widgetLocked       = json_as_bool(json_get(root, "widgetLocked"), cfg->widgetLocked);
    cfg->widgetClickThrough = json_as_bool(json_get(root, "widgetClickThrough"), cfg->widgetClickThrough);
    int wop = json_as_int(json_get(root, "widgetOpacity"), cfg->widgetOpacity);
    if (wop < 30) wop = 30;
    if (wop > 100) wop = 100;
    cfg->widgetOpacity      = wop;
    cfg->widgetX            = (LONG)json_as_int(json_get(root, "widgetX"), (int)WIDGET_POS_UNSET);
    cfg->widgetY            = (LONG)json_as_int(json_get(root, "widgetY"), (int)WIDGET_POS_UNSET);
    cfg->enableHotkeys      = json_as_bool(json_get(root, "enableHotkeys"), cfg->enableHotkeys);
    cfg->enableTickFlash    = json_as_bool(json_get(root, "enableTickFlash"), cfg->enableTickFlash);
    cfg->showSparkline      = json_as_bool(json_get(root, "showSparkline"), cfg->showSparkline);
    cfg->alertSound         = json_as_bool(json_get(root, "alertSound"), cfg->alertSound);
    int ar = json_as_int(json_get(root, "autoRotateSec"), (int)cfg->autoRotateSec);
    if (ar < 0) ar = 0;
    if (ar > 120) ar = 120;
    cfg->autoRotateSec      = (DWORD)ar;

    int showOn = json_as_int(json_get(root, "showOn"), (int)cfg->showOn);
    if (showOn >= SHOW_MAIN && showOn <= SHOW_OTHERS) cfg->showOn = (ShowWhere)showOn;

    int clickAct = json_as_int(json_get(root, "clickAction"), (int)cfg->clickAction);
    if (clickAct >= CLICK_OPEN_CHART && clickAct <= CLICK_REFRESH) cfg->clickAction = (CoinClickAction)clickAct;

    cfg->offsetX = (LONG)json_as_int(json_get(root, "offsetX"), 0);

    const char *cmc = json_as_str(json_get(root, "cmcApiKey"), "");
    lstrcpynA(cfg->cmcApiKey, cmc, (int)sizeof(cfg->cmcApiKey));
    const char *cg = json_as_str(json_get(root, "cgApiKey"), "");
    lstrcpynA(cfg->cgApiKey, cg, (int)sizeof(cfg->cgApiKey));

    const JsonVal *arr = json_get(root, "coins");
    if (arr && arr->type == JSON_ARRAY) {
        cfg->coinCount = 0;
        for (const JsonVal *el = arr->child; el && cfg->coinCount < MAX_COINS; el = el->next) {
            if (el->type != JSON_OBJECT) continue;
            CoinItem *c = &cfg->coins[cfg->coinCount];
            ZeroMemory(c, sizeof(*c));
            c->enabled = json_as_bool(json_get(el, "enabled"), 1);

            const char *pname = json_as_str(json_get(el, "provider"), "auto");
            c->provider = provider_from_key(pname);

            const char *q = json_as_str(json_get(el, "query"), "");
            if (!q[0]) continue;
            utf8_to_wide(q, c->query, 128);

            const char *ch = json_as_str(json_get(el, "chain"), "");
            utf8_to_wide(ch, c->chain, 32);

            const char *lbl = json_as_str(json_get(el, "label"), "");
            utf8_to_wide(lbl, c->label, 24);

            c->inputKind = (InputKind)json_as_int(json_get(el, "inputKind"), (int)provider_detect_input(c->query));
            if (c->inputKind < INPUT_PAIR || c->inputKind > INPUT_COIN_ID)
                c->inputKind = provider_detect_input(c->query);

            c->decimals = json_as_int(json_get(el, "decimals"), -1);
            if (c->decimals < -1 || c->decimals > 8) c->decimals = -1;

            int civ = json_as_int(json_get(el, "intervalSec"), 0);
            if (civ > 0 && civ < MIN_INTERVAL_SEC) civ = MIN_INTERVAL_SEC;
            if (civ > MAX_INTERVAL_SEC) civ = MAX_INTERVAL_SEC;
            c->intervalSec = (DWORD)(civ > 0 ? civ : 0);

            double tmpD = 0;
            if (json_as_double(json_get(el, "alertAbove"), &tmpD) && tmpD > 0) c->alertAbove = tmpD;
            if (json_as_double(json_get(el, "alertBelow"), &tmpD) && tmpD > 0) c->alertBelow = tmpD;
            if (json_as_double(json_get(el, "holdingQty"), &tmpD) && tmpD > 0) c->holdingQty = tmpD;

            cfg->coinCount++;
        }
    }

    json_free(root);
    return 1;
}

int config_load(AppConfig *cfg)
{
    wchar_t path[MAX_PATH];
    config_get_path(path, MAX_PATH);
    return config_load_from(cfg, path);
}

int config_save_to(const AppConfig *cfg, const wchar_t *path)
{
    JsonVal *root = json_new_obj();
    if (!root) return 0;

    json_obj_add(root, "globalIntervalSec", json_new_num((double)cfg->globalIntervalSec));
    json_obj_add(root, "enableWebSocket",   json_new_bool(cfg->enableWebSocket));
    json_obj_add(root, "showChangePct",     json_new_bool(cfg->showChangePct));
    json_obj_add(root, "colorByChange",     json_new_bool(cfg->colorByChange));
    json_obj_add(root, "showRefreshIcon",   json_new_bool(cfg->showRefreshIcon));
    json_obj_add(root, "showCurrencySign",  json_new_bool(cfg->showCurrencySign));
    json_obj_add(root, "compactPrices",      json_new_bool(cfg->compactPrices));
    json_obj_add(root, "showTrayIcon",       json_new_bool(cfg->showTrayIcon));
    json_obj_add(root, "enableLogging",      json_new_bool(cfg->enableLogging));
    json_obj_add(root, "showTaskbarBar",     json_new_bool(cfg->showTaskbarBar));
    json_obj_add(root, "showWidget",         json_new_bool(cfg->showWidget));
    json_obj_add(root, "widgetLayout",       json_new_num((double)cfg->widgetLayout));
    json_obj_add(root, "widgetTopmost",      json_new_bool(cfg->widgetTopmost));
    json_obj_add(root, "widgetLocked",       json_new_bool(cfg->widgetLocked));
    json_obj_add(root, "widgetClickThrough", json_new_bool(cfg->widgetClickThrough));
    json_obj_add(root, "widgetOpacity",      json_new_num((double)cfg->widgetOpacity));
    json_obj_add(root, "widgetX",            json_new_num((double)cfg->widgetX));
    json_obj_add(root, "widgetY",            json_new_num((double)cfg->widgetY));
    json_obj_add(root, "enableHotkeys",      json_new_bool(cfg->enableHotkeys));
    json_obj_add(root, "enableTickFlash",    json_new_bool(cfg->enableTickFlash));
    json_obj_add(root, "showSparkline",      json_new_bool(cfg->showSparkline));
    json_obj_add(root, "alertSound",         json_new_bool(cfg->alertSound));
    json_obj_add(root, "autoRotateSec",      json_new_num((double)cfg->autoRotateSec));
    json_obj_add(root, "showOn",            json_new_num((double)cfg->showOn));
    json_obj_add(root, "clickAction",       json_new_num((double)cfg->clickAction));
    json_obj_add(root, "offsetX",           json_new_num((double)cfg->offsetX));
    json_obj_add(root, "cmcApiKey",         json_new_str(cfg->cmcApiKey));
    json_obj_add(root, "cgApiKey",          json_new_str(cfg->cgApiKey));

    JsonVal *arr = json_new_arr();
    for (int i = 0; i < cfg->coinCount; i++) {
        const CoinItem *c = &cfg->coins[i];
        JsonVal *o = json_new_obj();
        char q[256], ch[64], lbl[64];
        wide_to_utf8(c->query, q, sizeof(q));
        wide_to_utf8(c->chain, ch, sizeof(ch));
        wide_to_utf8(c->label, lbl, sizeof(lbl));

        json_obj_add(o, "enabled",     json_new_bool(c->enabled));
        json_obj_add(o, "provider",    json_new_str(provider_key(c->provider)));
        json_obj_add(o, "inputKind",   json_new_num((double)c->inputKind));
        json_obj_add(o, "query",       json_new_str(q));
        json_obj_add(o, "chain",       json_new_str(ch));
        json_obj_add(o, "label",       json_new_str(lbl));
        json_obj_add(o, "decimals",    json_new_num((double)c->decimals));
        json_obj_add(o, "intervalSec", json_new_num((double)c->intervalSec));
        if (c->alertAbove > 0.0) json_obj_add(o, "alertAbove", json_new_num(c->alertAbove));
        if (c->alertBelow > 0.0) json_obj_add(o, "alertBelow", json_new_num(c->alertBelow));
        if (c->holdingQty > 0.0) json_obj_add(o, "holdingQty", json_new_num(c->holdingQty));
        json_arr_add(arr, o);
    }
    json_obj_add(root, "coins", arr);

    char *text = json_stringify(root, 1);
    json_free(root);
    if (!text) return 0;

    wchar_t tmpPath[MAX_PATH];
    _snwprintf(tmpPath, MAX_PATH, L"%s.tmp", path);
    tmpPath[MAX_PATH - 1] = 0;

    int ok = 0;
    HANDLE f = CreateFileW(tmpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0, len = (DWORD)strlen(text);
        if (WriteFile(f, text, len, &w, NULL) && w == len) ok = 1;
        CloseHandle(f);
        if (ok) {
            if (!MoveFileExW(tmpPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(tmpPath);
                ok = 0;
            }
        } else {
            DeleteFileW(tmpPath);
        }
    }
    json_str_free(text);
    return ok;
}

int config_save(const AppConfig *cfg)
{
    wchar_t path[MAX_PATH];
    config_get_path(path, MAX_PATH);
    return config_save_to(cfg, path);
}
