/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Based on TraySense, Copyright (C) 2026 DMTWEAKS.
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#ifndef TRAYCOIN_TYPES_H
#define TRAYCOIN_TYPES_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <wchar.h>
#include <stdint.h>

#define APP_NAME        L"TrayCoin"
#define APP_FULL_NAME   L"TrayCoin - Taskbar & Widget Crypto Tracker"
#define APP_AUTHOR      L"@hoangquocvietbro"
#define APP_VERSION     L"2.0.0"
#define APP_MUTEX       L"Local\\TrayCoin.single"
#define RUN_REG_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define WM_TRAY             (WM_APP + 1)
#define WM_PRICES_UPDATED   (WM_APP + 2)
#define WM_REFRESH_STATE    (WM_APP + 3)
#define WM_PRICE_ALERT      (WM_APP + 4)
#define WM_NET_CHANGED      (WM_APP + 5)

#define MAX_COINS           64
#define COIN_HIST_LEN       24
#define MIN_INTERVAL_SEC    2
#define MAX_INTERVAL_SEC    3600

typedef enum {
    PROV_AUTO = 0,
    PROV_BINANCE,
    PROV_BINANCE_FUTURES,
    PROV_OKX,
    PROV_BYBIT,
    PROV_GATE,
    PROV_MEXC,
    PROV_KUCOIN,
    PROV_BITGET,
    PROV_COINBASE,
    PROV_KRAKEN,
    PROV_HTX,
    PROV_DEXSCREENER,
    PROV_GECKOTERMINAL,
    PROV_JUPITER,
    PROV_COINGECKO,
    PROV_COINMARKETCAP,
    PROV_COUNT
} ProviderId;

typedef enum {
    INPUT_PAIR = 0,       /* e.g. BTC/USDT, ETHUSDT, SOL-USDT */
    INPUT_CONTRACT,       /* e.g. 0x..., Solana mint address */
    INPUT_DEX_PAIR,       /* e.g. chain/pairAddress or DexScreener URL */
    INPUT_COIN_ID         /* e.g. bitcoin (CoinGecko ID) */
} InputKind;

typedef enum {
    SHOW_MAIN = 0,
    SHOW_ALL,
    SHOW_OTHERS
} ShowWhere;

typedef enum {
    CLICK_OPEN_CHART = 0,
    CLICK_OPEN_SETTINGS,
    CLICK_REFRESH
} CoinClickAction;

typedef enum {
    WIDGET_VERTICAL = 0,
    WIDGET_HORIZONTAL
} WidgetLayout;

#define WIDGET_POS_UNSET ((LONG)-99999)

typedef struct {
    /* Persisted settings */
    int        enabled;             /* 1 = visible on taskbar, 0 = hidden */
    ProviderId provider;            /* Selected API provider */
    InputKind  inputKind;           /* Auto-detected or explicit input kind */
    wchar_t    query[128];          /* Raw pair, contract address, URL, or ID */
    wchar_t    chain[32];           /* Optional chain (solana, ethereum, base, bsc...) */
    wchar_t    label[24];           /* Display symbol on taskbar (e.g. BTC, PEPE) */
    int        decimals;            /* -1 = smart auto, or 0..8 */
    DWORD      intervalSec;         /* 0 = use globalIntervalSec, else override */
    double     alertAbove;          /* 0 = off, alert when price >= alertAbove */
    double     alertBelow;          /* 0 = off, alert when price <= alertBelow */
    double     holdingQty;          /* 0 = off, optional holdings quantity */

    /* Runtime state (not saved) */
    double     price;
    double     prevPrice;
    double     change24h;
    int        hasChange24h;
    int        ok;                  /* 1 = valid price fetched */
    int        fetching;            /* 1 = currently fetching */
    int        failCount;           /* Consecutive failures for backoff */
    ULONGLONG  lastUpdateMs;        /* GetTickCount64() of last successful fetch */
    SYSTEMTIME lastUpdateTime;      /* Local time of last successful fetch */
    ULONGLONG  nextFetchMs;         /* Scheduled next fetch tick */
    int        wsLive;              /* 1 = actively receiving WebSocket ticks */
    ULONGLONG  wsLastTickMs;        /* GetTickCount64() of last WS tick */
    int        flashDir;            /* +1 up, -1 down, 0 none */
    ULONGLONG  flashStartMs;        /* TickCount64 when flash started */
    int        alertFiredAbove;     /* 1 = already triggered above threshold */
    int        alertFiredBelow;     /* 1 = already triggered below threshold */
    double     history[COIN_HIST_LEN]; /* Ring buffer for mini sparkline */
    int        histCount;           /* 0..COIN_HIST_LEN */
    int        histHead;            /* Next write index */
    ULONGLONG  lastHistMs;          /* Last time a new point was appended */
    wchar_t    resolvedSource[32];  /* Actual provider name (useful for Auto) */
    wchar_t    resolvedSymbol[24];  /* Symbol discovered from API (e.g. PEPE) */
    wchar_t    pairUrl[256];        /* Web chart URL */
    wchar_t    lastError[96];       /* Last error message if !ok */
} CoinItem;

typedef struct {
    DWORD           globalIntervalSec;  /* Default 15s (2..3600) */
    int             enableWebSocket;    /* 1 = real-time WS for Binance/OKX/Bybit/Gate */
    int             showChangePct;      /* Show 24h % change on taskbar */
    int             colorByChange;      /* Green/red coloring by 24h % */
    int             showRefreshIcon;    /* Show small clickable refresh icon */
    int             showCurrencySign;   /* Prefix prices with $ */
    int             compactPrices;      /* Use K/M/B abbreviations for large prices */
    int             showTrayIcon;       /* Also show a small system tray icon */
    int             enableLogging;      /* Write diagnostic log to traycoin.log */
    int             showTaskbarBar;     /* 1 = show ticker on Windows taskbar */
    int             showWidget;         /* 1 = show floating Always-on-Top widget */
    WidgetLayout    widgetLayout;       /* Vertical card or Horizontal pill */
    int             widgetTopmost;      /* 1 = HWND_TOPMOST */
    int             widgetLocked;       /* 1 = lock dragging */
    int             widgetClickThrough; /* 1 = WS_EX_TRANSPARENT mouse pass-through */
    int             widgetOpacity;      /* Card background opacity 30..100 */
    LONG            widgetX;            /* Saved X or WIDGET_POS_UNSET */
    LONG            widgetY;            /* Saved Y or WIDGET_POS_UNSET */
    int             enableHotkeys;      /* 1 = Ctrl+Shift+C/L/R global hotkeys */
    int             enableTickFlash;    /* 1 = green/red 500ms flash on price change */
    int             showSparkline;      /* 1 = 24-pt mini sparkline in Vertical Widget */
    int             alertSound;         /* 1 = play sound on price alert */
    DWORD           autoRotateSec;      /* 0 = off, else seconds to auto-rotate taskbar pages */
    ShowWhere       showOn;             /* Main / All / Others monitors */
    CoinClickAction clickAction;        /* Left-click coin action */
    LONG            offsetX;            /* Horizontal taskbar offset in DIPs */
    char            cmcApiKey[128];     /* Optional CoinMarketCap Pro API Key */
    char            cgApiKey[128];      /* Optional CoinGecko Demo API Key */
    int             coinCount;
    CoinItem        coins[MAX_COINS];
} AppConfig;

/* Global state shared between UI thread and poller thread (lock g_stateCs!) */
extern HWND             g_wnd;
extern HINSTANCE        g_hinst;
extern CRITICAL_SECTION g_stateCs;
extern AppConfig        g_cfg;
extern volatile LONG    g_quit;
extern volatile LONG    g_paused;
extern volatile LONG    g_anyFetching;
extern int              g_light;

/* Autostart, hotkey & animation helpers in main.c */
int  app_autostart_is_on(void);
void app_autostart_toggle(void);
void app_autostart_set(int enable);
void app_update_hotkeys(void);
void app_ensure_anim_timer(void);

/* Shared tick-flash & sparkline recorder (poller.c) */
void coin_record_tick_locked(CoinItem *c, double newPrice, ULONGLONG now);

/* Format helpers in taskbar.c */
void format_price(double price, int decimals, int showDollar, int compact, wchar_t *out, int outCap);
void format_pct(double pct, wchar_t *out, int outCap);

/* UTF-8 / UTF-16 helpers in config.c */
int  utf8_to_wide(const char *src, wchar_t *dst, int dstChars);
int  wide_to_utf8(const wchar_t *src, char *dst, int dstBytes);

#endif /* TRAYCOIN_TYPES_H */
