/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "alerts.h"
#include "providers.h"
#include "logger.h"
#include <shellapi.h>
#include <stdio.h>

static void post_alert(const CoinItem *c, const wchar_t *dirTxt, double target, double actual)
{
    if (!g_wnd) return;
    AlertNotification *n = (AlertNotification *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AlertNotification));
    if (!n) return;

    wchar_t lbl[24], tStr[32], pStr[32];
    provider_default_label(c, lbl, 24);
    format_price(target, -1, 1, 0, tStr, 32);
    format_price(actual, -1, 1, 0, pStr, 32);

    _snwprintf(n->title, 64, L"TrayCoin Price Alert: %s", lbl);
    n->title[63] = 0;
    _snwprintf(n->body, 224, L"%s %s %s (now %s)", lbl, dirTxt, tStr, pStr);
    n->body[223] = 0;
    n->playSound = g_cfg.alertSound;

    LOGI(L"ALERT", L"%s: %s", n->title, n->body);
    if (!PostMessageW(g_wnd, WM_PRICE_ALERT, 0, (LPARAM)n)) {
        HeapFree(GetProcessHeap(), 0, n);
    }
}

void alerts_check_locked(CoinItem *c, double newPrice)
{
    if (!c || newPrice <= 0.0) return;

    if (c->alertAbove > 0.0) {
        if (!c->alertFiredAbove && newPrice >= c->alertAbove) {
            c->alertFiredAbove = 1;
            post_alert(c, L"crossed above", c->alertAbove, newPrice);
        } else if (c->alertFiredAbove && newPrice < c->alertAbove * 0.995) {
            c->alertFiredAbove = 0;
        }
    } else {
        c->alertFiredAbove = 0;
    }

    if (c->alertBelow > 0.0) {
        if (!c->alertFiredBelow && newPrice <= c->alertBelow) {
            c->alertFiredBelow = 1;
            post_alert(c, L"dropped below", c->alertBelow, newPrice);
        } else if (c->alertFiredBelow && newPrice > c->alertBelow * 1.005) {
            c->alertFiredBelow = 0;
        }
    } else {
        c->alertFiredBelow = 0;
    }
}

void alerts_handle_ui_message(AlertNotification *msg)
{
    if (!msg) return;
    if (g_wnd) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_wnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        lstrcpynW(nid.szInfoTitle, msg->title, 64);
        lstrcpynW(nid.szInfo, msg->body, 256);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (msg->playSound) {
        MessageBeep(MB_ICONASTERISK);
    }
    HeapFree(GetProcessHeap(), 0, msg);
}
