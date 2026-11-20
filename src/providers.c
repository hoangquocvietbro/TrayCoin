/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "providers.h"
#include "http.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <math.h>

typedef struct {
    const char    *key;
    const wchar_t *name;
} ProvMeta;

static const ProvMeta g_provMeta[PROV_COUNT] = {
    { "auto",            L"Auto (Smart)" },
    { "binance",         L"Binance" },
    { "binance_futures", L"Binance Futures" },
    { "okx",             L"OKX" },
    { "bybit",           L"Bybit" },
    { "gate",            L"Gate.io" },
    { "mexc",            L"MEXC" },
    { "kucoin",          L"KuCoin" },
    { "bitget",          L"Bitget" },
    { "coinbase",        L"Coinbase" },
    { "kraken",          L"Kraken" },
    { "htx",             L"HTX (Huobi)" },
    { "dexscreener",     L"DexScreener (DEX/Meme)" },
    { "geckoterminal",   L"GeckoTerminal (DEX)" },
    { "jupiter",         L"Jupiter (Solana)" },
    { "coingecko",       L"CoinGecko" },
    { "coinmarketcap",   L"CoinMarketCap" }
};

const wchar_t *provider_display_name(ProviderId id)
{
    if (id < 0 || id >= PROV_COUNT) return L"Auto (Smart)";
    return g_provMeta[id].name;
}

const char *provider_key(ProviderId id)
{
    if (id < 0 || id >= PROV_COUNT) return "auto";
    return g_provMeta[id].key;
}

ProviderId provider_from_key(const char *key)
{
    if (!key) return PROV_AUTO;
    for (int i = 0; i < PROV_COUNT; i++)
        if (_stricmp(g_provMeta[i].key, key) == 0) return (ProviderId)i;
    return PROV_AUTO;
}

/* ---- Input helpers ---- */

static void trim_ws(const wchar_t *in, wchar_t *out, int cap)
{
    while (*in && iswspace(*in)) in++;
    int n = lstrlenW(in);
    while (n > 0 && iswspace(in[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(out, in, (size_t)n * sizeof(wchar_t));
    out[n] = 0;
}

static int is_hex_str(const wchar_t *s, int len)
{
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
            return 0;
    }
    return 1;
}

static int is_base58_addr(const wchar_t *s)
{
    int n = lstrlenW(s);
    if (n < 32 || n > 48) return 0;
    int hasLower = 0, hasUpper = 0, hasDigit = 0;
    for (int i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (c == L'0' || c == L'O' || c == L'I' || c == L'l') return 0;
        if (c >= L'1' && c <= L'9') hasDigit = 1;
        else if (c >= L'A' && c <= L'Z') hasUpper = 1;
        else if (c >= L'a' && c <= L'z') hasLower = 1;
        else return 0;
    }
    return (hasLower && hasUpper) || (hasDigit && (hasLower || hasUpper));
}

/* Parse "https://dexscreener.com/{chain}/{pair}" or "chain/pair" (where pair is long) */
static int parse_dexscreener_url(const wchar_t *in, wchar_t *chain, int chainCap, wchar_t *pair, int pairCap)
{
    wchar_t buf[256];
    trim_ws(in, buf, 256);
    const wchar_t *p = wcsstr(buf, L"dexscreener.com/");
    if (p) {
        p += 16;
    } else {
        /* Also allow "solana/PairAddressHere..." when second segment >= 26 chars */
        const wchar_t *sl = wcschr(buf, L'/');
        if (!sl || sl == buf || lstrlenW(sl + 1) < 26) return 0;
        p = buf;
    }
    const wchar_t *sl = wcschr(p, L'/');
    if (!sl || sl == p) return 0;
    int clen = (int)(sl - p);
    if (clen >= chainCap) clen = chainCap - 1;
    memcpy(chain, p, (size_t)clen * sizeof(wchar_t));
    chain[clen] = 0;

    p = sl + 1;
    int plen = 0;
    while (p[plen] && p[plen] != L'?' && p[plen] != L'#' && p[plen] != L'/' && plen < pairCap - 1)
        plen++;
    memcpy(pair, p, (size_t)plen * sizeof(wchar_t));
    pair[plen] = 0;
    return chain[0] && pair[0];
}

InputKind provider_detect_input(const wchar_t *query)
{
    if (!query) return INPUT_PAIR;
    wchar_t q[128];
    trim_ws(query, q, 128);
    int len = lstrlenW(q);
    wchar_t ctmp[32], ptmp[96];
    if (parse_dexscreener_url(q, ctmp, 32, ptmp, 96)) return INPUT_DEX_PAIR;
    if (len == 42 && q[0] == L'0' && (q[1] == L'x' || q[1] == L'X') && is_hex_str(q + 2, 40))
        return INPUT_CONTRACT;
    /* Sui / Aptos 0x... 66-char address or ::coin type */
    if (len >= 50 && q[0] == L'0' && (q[1] == L'x' || q[1] == L'X'))
        return INPUT_CONTRACT;
    /* TON EQ.../UQ... contract */
    if (len == 48 && ((q[0] == L'E' && q[1] == L'Q') || (q[0] == L'U' && q[1] == L'Q')))
        return INPUT_CONTRACT;
    if (is_base58_addr(q))
        return INPUT_CONTRACT;
    return INPUT_PAIR;
}

static void str_upper_w(wchar_t *s)
{
    for (; *s; s++) *s = towupper(*s);
}

static void str_lower_w(wchar_t *s)
{
    for (; *s; s++) *s = towlower(*s);
}

void provider_split_pair(const wchar_t *query, wchar_t *base, int baseCap, wchar_t *quote, int quoteCap)
{
    wchar_t q[128];
    trim_ws(query, q, 128);
    str_upper_w(q);
    base[0] = 0;
    quote[0] = 0;

    /* Look for explicit delimiter '/', '-', '_' */
    const wchar_t *sep = NULL;
    for (const wchar_t *p = q; *p; p++) {
        if (*p == L'/' || *p == L'-' || *p == L'_') { sep = p; break; }
    }
    if (sep) {
        int bl = (int)(sep - q);
        if (bl >= baseCap) bl = baseCap - 1;
        memcpy(base, q, (size_t)bl * sizeof(wchar_t));
        base[bl] = 0;
        lstrcpynW(quote, sep + 1, quoteCap);
        /* Strip trailing -SWAP etc. if present */
        wchar_t *extra = wcschr(quote, L'-');
        if (extra) *extra = 0;
        if (!quote[0]) lstrcpynW(quote, L"USDT", quoteCap);
        return;
    }

    /* No delimiter: check known quote suffixes */
    static const wchar_t *QUOTES[] = { L"USDT", L"USDC", L"FDUSD", L"BUSD", L"TUSD", L"USD", L"EUR", L"BTC", L"ETH", L"BNB", L"SOL" };
    int len = lstrlenW(q);
    for (int i = 0; i < (int)(sizeof(QUOTES) / sizeof(QUOTES[0])); i++) {
        int ql = lstrlenW(QUOTES[i]);
        if (len > ql && lstrcmpW(q + len - ql, QUOTES[i]) == 0) {
            /* Guard Kraken XBTUSD/XBTUSDT from matching TUSD */
            if (lstrcmpW(QUOTES[i], L"TUSD") == 0 && len == 6 && wcsncmp(q, L"XBT", 3) == 0)
                continue;
            int bl = len - ql;
            if (bl >= baseCap) bl = baseCap - 1;
            memcpy(base, q, (size_t)bl * sizeof(wchar_t));
            base[bl] = 0;
            lstrcpynW(quote, QUOTES[i], quoteCap);
            return;
        }
    }

    /* Single ticker like "BTC" -> base="BTC", quote="USDT" */
    lstrcpynW(base, q, baseCap);
    lstrcpynW(quote, L"USDT", quoteCap);
}

void provider_default_label(const CoinItem *c, wchar_t *out, int outCap)
{
    if (c->label[0]) { lstrcpynW(out, c->label, outCap); return; }
    if (c->resolvedSymbol[0]) { lstrcpynW(out, c->resolvedSymbol, outCap); return; }
    InputKind k = provider_detect_input(c->query);
    if (k == INPUT_CONTRACT || k == INPUT_DEX_PAIR) {
        int len = lstrlenW(c->query);
        if (len > 8) _snwprintf(out, outCap, L"%.4s..%s", c->query, c->query + len - 3);
        else lstrcpynW(out, c->query, outCap);
        out[outCap - 1] = 0;
        return;
    }
    wchar_t base[32], quote[16];
    provider_split_pair(c->query, base, 32, quote, 16);
    lstrcpynW(out, base[0] ? base : c->query, outCap);
}

static void set_http_err(FetchResult *out, const HttpResponse *res, const wchar_t *src)
{
    lstrcpynW(out->source, src, 32);
    if (res->statusCode == 429) {
        out->rateLimited = 1;
        lstrcpynW(out->error, L"Rate limited (HTTP 429)", 96);
    } else if (res->statusCode > 0) {
        _snwprintf(out->error, 96, L"HTTP %lu", (unsigned long)res->statusCode);
        out->error[95] = 0;
    } else {
        _snwprintf(out->error, 96, L"Network error (%lu)", (unsigned long)res->winError);
        out->error[95] = 0;
    }
}

/* ---- CEX Providers ---- */

static void fetch_binance_like(const CoinItem *c, const wchar_t *host, const wchar_t *fallbackHost,
                               const wchar_t *prefix, const wchar_t *srcName, int futures, int pctIsRatio, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(sym, 48, L"%s%s", base, quote);
    _snwprintf(path, 160, L"%s?symbol=%s", prefix, sym);

    HttpResponse res;
    int ok = http_get(host, path, NULL, &res);
    if ((!ok || res.statusCode == 451 || res.statusCode == 403 || res.statusCode == 0) && fallbackHost) {
        http_free(&res);
        ok = http_get(fallbackHost, path, NULL, &res);
    }
    if (!ok || res.statusCode != 200) {
        set_http_err(out, &res, srcName);
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    double price = 0, pct = 0;
    if (json_as_double(json_get(j, "lastPrice"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(j, "priceChangePercent"), &pct)) {
            out->change24h = pctIsRatio ? (pct * 100.0) : pct;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, srcName, 32);
        if (futures)
            _snwprintf(out->url, 256, L"https://www.binance.com/en/futures/%s", sym);
        else if (lstrcmpW(srcName, L"MEXC") == 0)
            _snwprintf(out->url, 256, L"https://www.mexc.com/exchange/%s_%s", base, quote);
        else
            _snwprintf(out->url, 256, L"https://www.binance.com/en/trade/%s_%s", base, quote);
    } else {
        const char *msg = json_as_str(json_get(j, "msg"), "Pair not found");
        utf8_to_wide(msg, out->error, 96);
    }
    json_free(j);
}

static void fetch_okx(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], inst[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(inst, 48, L"%s-%s", base, quote);
    _snwprintf(path, 160, L"/api/v5/market/ticker?instId=%s", inst);

    HttpResponse res;
    if (!http_get(L"www.okx.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"OKX");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *d0 = json_at(json_get(j, "data"), 0);
    double last = 0, open = 0;
    if (d0 && json_as_double(json_get(d0, "last"), &last) && last > 0) {
        out->ok = 1;
        out->price = last;
        if (json_as_double(json_get(d0, "open24h"), &open) && open > 0) {
            out->change24h = ((last - open) / open) * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"OKX", 32);
        wchar_t bl[32], ql[16];
        lstrcpynW(bl, base, 32); lstrcpynW(ql, quote, 16);
        str_lower_w(bl); str_lower_w(ql);
        _snwprintf(out->url, 256, L"https://www.okx.com/trade-spot/%s-%s", bl, ql);
    } else {
        const char *msg = json_as_str(json_get(j, "msg"), "Pair not found");
        utf8_to_wide(msg, out->error, 96);
    }
    json_free(j);
}

static void fetch_bybit(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(sym, 48, L"%s%s", base, quote);
    _snwprintf(path, 160, L"/v5/market/tickers?category=spot&symbol=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.bybit.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"Bybit");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *item = json_at(json_get(json_get(j, "result"), "list"), 0);
    double price = 0, pcnt = 0;
    if (item && json_as_double(json_get(item, "lastPrice"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(item, "price24hPcnt"), &pcnt)) {
            out->change24h = pcnt * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"Bybit", 32);
        _snwprintf(out->url, 256, L"https://www.bybit.com/trade/spot/%s/%s", base, quote);
    } else {
        const char *msg = json_as_str(json_get(j, "retMsg"), "Pair not found");
        utf8_to_wide(msg, out->error, 96);
    }
    json_free(j);
}

static void fetch_gate(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(sym, 48, L"%s_%s", base, quote);
    _snwprintf(path, 160, L"/api/v4/spot/tickers?currency_pair=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.gateio.ws", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"Gate.io");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *item = (j->type == JSON_ARRAY) ? json_at(j, 0) : j;
    double price = 0, pct = 0;
    if (item && json_as_double(json_get(item, "last"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(item, "change_percentage"), &pct)) {
            out->change24h = pct;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"Gate.io", 32);
        _snwprintf(out->url, 256, L"https://www.gate.io/trade/%s", sym);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

static void fetch_kucoin(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(sym, 48, L"%s-%s", base, quote);
    _snwprintf(path, 160, L"/api/v1/market/stats?symbol=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.kucoin.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"KuCoin");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *d = json_get(j, "data");
    double price = 0, rate = 0;
    if (d && json_as_double(json_get(d, "last"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(d, "changeRate"), &rate)) {
            out->change24h = rate * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"KuCoin", 32);
        _snwprintf(out->url, 256, L"https://www.kucoin.com/trade/%s", sym);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

static void fetch_bitget(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    _snwprintf(sym, 48, L"%s%s", base, quote);
    _snwprintf(path, 160, L"/api/v2/spot/market/tickers?symbol=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.bitget.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"Bitget");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *d0 = json_at(json_get(j, "data"), 0);
    double price = 0, chg = 0;
    if (d0 && json_as_double(json_get(d0, "lastPr"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(d0, "change24h"), &chg)) {
            out->change24h = chg * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"Bitget", 32);
        _snwprintf(out->url, 256, L"https://www.bitget.com/spot/%s", sym);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

static void fetch_coinbase(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    /* Coinbase predominantly uses USD instead of USDT for major pairs if user wrote BTC */
    if (lstrcmpW(quote, L"USDT") == 0 && !wcsstr(c->query, L"USDT") && !wcsstr(c->query, L"usdt"))
        lstrcpyW(quote, L"USD");
    _snwprintf(sym, 48, L"%s-%s", base, quote);
    _snwprintf(path, 160, L"/products/%s/stats", sym);

    HttpResponse res;
    int ok = http_get(L"api.exchange.coinbase.com", path, NULL, &res);
    if ((!ok || res.statusCode == 404) && lstrcmpW(quote, L"USDT") == 0) {
        http_free(&res);
        lstrcpyW(quote, L"USD");
        _snwprintf(sym, 48, L"%s-%s", base, quote);
        _snwprintf(path, 160, L"/products/%s/stats", sym);
        ok = http_get(L"api.exchange.coinbase.com", path, NULL, &res);
    }
    if (!ok || res.statusCode != 200) {
        set_http_err(out, &res, L"Coinbase");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    double last = 0, open = 0;
    if (json_as_double(json_get(j, "last"), &last) && last > 0) {
        out->ok = 1;
        out->price = last;
        if (json_as_double(json_get(j, "open"), &open) && open > 0) {
            out->change24h = ((last - open) / open) * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"Coinbase", 32);
        _snwprintf(out->url, 256, L"https://www.coinbase.com/advanced-trade/spot/%s", sym);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

static void fetch_kraken(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    if (lstrcmpW(quote, L"USDT") == 0 && !wcsstr(c->query, L"USDT") && !wcsstr(c->query, L"usdt"))
        lstrcpyW(quote, L"USD");
    _snwprintf(sym, 48, L"%s%s", base, quote);
    _snwprintf(path, 160, L"/0/public/Ticker?pair=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.kraken.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"Kraken");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *r = json_get(j, "result");
    const JsonVal *first = json_first_child(r);
    double last = 0, open = 0;
    if (first && json_as_double(json_at(json_get(first, "c"), 0), &last) && last > 0) {
        out->ok = 1;
        out->price = last;
        if (json_as_double(json_get(first, "o"), &open) && open > 0) {
            out->change24h = ((last - open) / open) * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"Kraken", 32);
        wchar_t bl[32], ql[16];
        lstrcpynW(bl, base, 32); lstrcpynW(ql, quote, 16);
        str_lower_w(bl); str_lower_w(ql);
        _snwprintf(out->url, 256, L"https://pro.kraken.com/app/trade/%s-%s", bl, ql);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

static void fetch_htx(const CoinItem *c, FetchResult *out)
{
    wchar_t base[32], quote[16], sym[48], path[160];
    provider_split_pair(c->query, base, 32, quote, 16);
    wchar_t bl[32], ql[16];
    lstrcpynW(bl, base, 32); lstrcpynW(ql, quote, 16);
    str_lower_w(bl); str_lower_w(ql);
    _snwprintf(sym, 48, L"%s%s", bl, ql);
    _snwprintf(path, 160, L"/market/detail/merged?symbol=%s", sym);

    HttpResponse res;
    if (!http_get(L"api.huobi.pro", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"HTX");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *tick = json_get(j, "tick");
    double close = 0, open = 0;
    if (tick && json_as_double(json_get(tick, "close"), &close) && close > 0) {
        out->ok = 1;
        out->price = close;
        if (json_as_double(json_get(tick, "open"), &open) && open > 0) {
            out->change24h = ((close - open) / open) * 100.0;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"HTX", 32);
        _snwprintf(out->url, 256, L"https://www.htx.com/trade/%s_%s", bl, ql);
    } else {
        lstrcpynW(out->error, L"Pair not found", 96);
    }
    json_free(j);
}

/* ---- DEX & Meme Coin Providers ---- */

static const JsonVal *pick_best_dex_pair(const JsonVal *pairs, const wchar_t *chainFilter, const wchar_t *addrFilter)
{
    if (!pairs || pairs->type != JSON_ARRAY) return NULL;
    char chUtf8[32] = {0}, addrUtf8[128] = {0};
    if (chainFilter && chainFilter[0]) wide_to_utf8(chainFilter, chUtf8, sizeof(chUtf8));
    if (addrFilter && addrFilter[0]) wide_to_utf8(addrFilter, addrUtf8, sizeof(addrUtf8));

    const JsonVal *best = NULL;
    double bestLiq = -1.0;

    for (const JsonVal *p = pairs->child; p; p = p->next) {
        double price = 0;
        if (!json_as_double(json_get(p, "priceUsd"), &price) || price <= 0) continue;

        if (chUtf8[0]) {
            const char *cid = json_as_str(json_get(p, "chainId"), "");
            if (_stricmp(cid, chUtf8) != 0) continue;
        }
        if (addrUtf8[0]) {
            const JsonVal *bt = json_get(p, "baseToken");
            const char *baddr = json_as_str(json_get(bt, "address"), "");
            const char *paddr = json_as_str(json_get(p, "pairAddress"), "");
            if (_stricmp(baddr, addrUtf8) != 0 && _stricmp(paddr, addrUtf8) != 0) continue;
        }

        double liq = 0;
        const JsonVal *lq = json_get(p, "liquidity");
        if (lq) json_as_double(json_get(lq, "usd"), &liq);
        if (!best || liq > bestLiq) {
            best = p;
            bestLiq = liq;
        }
    }
    return best;
}

static void fill_from_dex_pair(const JsonVal *p, FetchResult *out)
{
    double price = 0, h24 = 0;
    if (!p || !json_as_double(json_get(p, "priceUsd"), &price) || price <= 0) {
        lstrcpynW(out->error, L"No price on DexScreener", 96);
        return;
    }
    out->ok = 1;
    out->price = price;
    const JsonVal *pc = json_get(p, "priceChange");
    if (pc && json_as_double(json_get(pc, "h24"), &h24)) {
        out->change24h = h24;
        out->hasChange24h = 1;
    }
    const JsonVal *bt = json_get(p, "baseToken");
    const char *sym = json_as_str(json_get(bt, "symbol"), "");
    if (sym[0]) utf8_to_wide(sym, out->symbol, 24);
    const char *url = json_as_str(json_get(p, "url"), "");
    if (url[0]) utf8_to_wide(url, out->url, 256);

    const char *dex = json_as_str(json_get(p, "dexId"), "");
    if (dex[0]) {
        wchar_t dw[24];
        utf8_to_wide(dex, dw, 24);
        _snwprintf(out->source, 32, L"DEX/%s", dw);
        out->source[31] = 0;
    } else {
        lstrcpynW(out->source, L"DexScreener", 32);
    }
}

static void fetch_dexscreener(const CoinItem *c, FetchResult *out)
{
    wchar_t q[128], chain[32] = {0}, pair[96] = {0}, path[256];
    trim_ws(c->query, q, 128);

    if (parse_dexscreener_url(q, chain, 32, pair, 96)) {
        _snwprintf(path, 256, L"/latest/dex/pairs/%s/%s", chain, pair);
    } else if (c->inputKind == INPUT_DEX_PAIR && c->chain[0]) {
        _snwprintf(path, 256, L"/latest/dex/pairs/%s/%s", c->chain, q);
    } else if (provider_detect_input(q) == INPUT_CONTRACT || c->inputKind == INPUT_CONTRACT) {
        _snwprintf(path, 256, L"/latest/dex/tokens/%s", q);
    } else {
        wchar_t enc[200];
        http_url_encode_w(q, enc, 200);
        _snwprintf(path, 256, L"/latest/dex/search?q=%s", enc);
    }
    path[255] = 0;

    HttpResponse res;
    if (!http_get(L"api.dexscreener.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"DexScreener");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *pairs = json_get(j, "pairs");
    const JsonVal *best = pick_best_dex_pair(pairs, c->chain, NULL);
    if (!best) best = json_get(j, "pair");
    if (best) {
        fill_from_dex_pair(best, out);
    } else {
        lstrcpynW(out->error, L"Token/pair not found", 96);
    }
    json_free(j);
}

int provider_search_dex(const wchar_t *query, const wchar_t *chainFilter, DexSearchHit *hits, int maxHits)
{
    if (!query || !query[0] || !hits || maxHits <= 0) return 0;
    wchar_t q[128], chain[32] = {0}, pair[96] = {0}, path[256];
    trim_ws(query, q, 128);

    if (parse_dexscreener_url(q, chain, 32, pair, 96)) {
        _snwprintf(path, 256, L"/latest/dex/pairs/%s/%s", chain, pair);
    } else if (provider_detect_input(q) == INPUT_CONTRACT) {
        _snwprintf(path, 256, L"/latest/dex/tokens/%s", q);
    } else {
        wchar_t enc[200];
        http_url_encode_w(q, enc, 200);
        _snwprintf(path, 256, L"/latest/dex/search?q=%s", enc);
    }
    path[255] = 0;

    HttpResponse res;
    if (!http_get(L"api.dexscreener.com", path, NULL, &res) || res.statusCode != 200) {
        http_free(&res);
        return 0;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) return 0;

    char chUtf8[32] = {0};
    if (chainFilter && chainFilter[0]) wide_to_utf8(chainFilter, chUtf8, sizeof(chUtf8));

    const JsonVal *pairs = json_get(j, "pairs");
    int count = 0;
    if (pairs && pairs->type == JSON_ARRAY) {
        for (const JsonVal *p = pairs->child; p && count < maxHits; p = p->next) {
            double price = 0;
            if (!json_as_double(json_get(p, "priceUsd"), &price) || price <= 0) continue;
            const char *cid = json_as_str(json_get(p, "chainId"), "");
            if (chUtf8[0] && _stricmp(cid, chUtf8) != 0) continue;

            DexSearchHit *h = &hits[count++];
            ZeroMemory(h, sizeof(*h));
            h->price = price;
            const JsonVal *pc = json_get(p, "priceChange");
            if (pc) json_as_double(json_get(pc, "h24"), &h->change24h);
            const JsonVal *lq = json_get(p, "liquidity");
            if (lq) json_as_double(json_get(lq, "usd"), &h->liquidityUsd);

            const JsonVal *bt = json_get(p, "baseToken");
            utf8_to_wide(json_as_str(json_get(bt, "symbol"), ""), h->symbol, 24);
            utf8_to_wide(json_as_str(json_get(bt, "name"), ""), h->name, 48);
            utf8_to_wide(json_as_str(json_get(bt, "address"), ""), h->contract, 96);
            utf8_to_wide(cid, h->chain, 24);
            utf8_to_wide(json_as_str(json_get(p, "dexId"), ""), h->dex, 24);
            utf8_to_wide(json_as_str(json_get(p, "pairAddress"), ""), h->pairAddress, 96);
            utf8_to_wide(json_as_str(json_get(p, "url"), ""), h->url, 256);
        }
    }
    json_free(j);

    /* Sort hits by liquidity descending */
    for (int i = 0; i < count - 1; i++) {
        for (int k = i + 1; k < count; k++) {
            if (hits[k].liquidityUsd > hits[i].liquidityUsd) {
                DexSearchHit tmp = hits[i];
                hits[i] = hits[k];
                hits[k] = tmp;
            }
        }
    }
    return count;
}

static const wchar_t *geckoterminal_net(const wchar_t *chain)
{
    if (!chain || !chain[0]) return L"";
    if (_wcsicmp(chain, L"ethereum") == 0 || _wcsicmp(chain, L"eth") == 0) return L"eth";
    if (_wcsicmp(chain, L"bsc") == 0 || _wcsicmp(chain, L"bnb") == 0) return L"bsc";
    if (_wcsicmp(chain, L"solana") == 0 || _wcsicmp(chain, L"sol") == 0) return L"solana";
    if (_wcsicmp(chain, L"base") == 0) return L"base";
    if (_wcsicmp(chain, L"arbitrum") == 0) return L"arbitrum";
    if (_wcsicmp(chain, L"polygon") == 0) return L"polygon_pos";
    if (_wcsicmp(chain, L"optimism") == 0) return L"optimism";
    if (_wcsicmp(chain, L"avalanche") == 0) return L"avax";
    if (_wcsicmp(chain, L"ton") == 0) return L"ton";
    if (_wcsicmp(chain, L"sui") == 0) return L"sui-network";
    return chain;
}

static void fetch_geckoterminal(const CoinItem *c, FetchResult *out)
{
    wchar_t q[128], path[256];
    trim_ws(c->query, q, 128);

    const wchar_t *net = geckoterminal_net(c->chain);
    if (net[0] && provider_detect_input(q) == INPUT_CONTRACT) {
        _snwprintf(path, 256, L"/api/v2/simple/networks/%s/token_price/%s?include_24hr_price_change=true", net, q);
        HttpResponse res;
        if (!http_get(L"api.geckoterminal.com", path, NULL, &res) || res.statusCode != 200) {
            set_http_err(out, &res, L"GeckoTerminal");
            http_free(&res);
            return;
        }
        JsonVal *j = json_parse(res.body);
        http_free(&res);
        if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

        const JsonVal *attrs = json_get(json_get(j, "data"), "attributes");
        const JsonVal *tp = json_get(attrs, "token_prices");
        const JsonVal *ch = json_get(attrs, "h24_price_change_percentage");
        char qUtf8[128]; wide_to_utf8(q, qUtf8, sizeof(qUtf8));

        double price = 0, pct = 0;
        const JsonVal *pv = json_get_ci(tp, qUtf8);
        if (!pv) pv = json_first_child(tp);
        if (pv && json_as_double(pv, &price) && price > 0) {
            out->ok = 1;
            out->price = price;
            const JsonVal *cv = json_get_ci(ch, qUtf8);
            if (!cv) cv = json_first_child(ch);
            if (cv && json_as_double(cv, &pct)) {
                out->change24h = pct;
                out->hasChange24h = 1;
            }
            lstrcpynW(out->source, L"GeckoTerminal", 32);
            _snwprintf(out->url, 256, L"https://www.geckoterminal.com/%s/pools", net);
        } else {
            lstrcpynW(out->error, L"Token not found", 96);
        }
        json_free(j);
        return;
    }

    /* Fallback: GeckoTerminal pool search */
    wchar_t enc[200];
    http_url_encode_w(q, enc, 200);
    _snwprintf(path, 256, L"/api/v2/search/pools?query=%s", enc);
    HttpResponse res;
    if (!http_get(L"api.geckoterminal.com", path, NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"GeckoTerminal");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *p0 = json_at(json_get(j, "data"), 0);
    const JsonVal *attrs = json_get(p0, "attributes");
    double price = 0, pct = 0;
    if (attrs && json_as_double(json_get(attrs, "base_token_price_usd"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        const JsonVal *pcp = json_get(attrs, "price_change_percentage");
        if (pcp && json_as_double(json_get(pcp, "h24"), &pct)) {
            out->change24h = pct;
            out->hasChange24h = 1;
        }
        const char *nm = json_as_str(json_get(attrs, "name"), "");
        if (nm[0]) {
            wchar_t wnm[48]; utf8_to_wide(nm, wnm, 48);
            wchar_t *sp = wcschr(wnm, L' ');
            if (sp) *sp = 0;
            wchar_t *sl = wcschr(wnm, L'/');
            if (sl) *sl = 0;
            lstrcpynW(out->symbol, wnm, 24);
        }
        lstrcpynW(out->source, L"GeckoTerminal", 32);
        const char *addr = json_as_str(json_get(attrs, "address"), "");
        if (addr[0]) {
            wchar_t waddr[96]; utf8_to_wide(addr, waddr, 96);
            _snwprintf(out->url, 256, L"https://www.geckoterminal.com/search?q=%s", waddr);
        }
    } else {
        lstrcpynW(out->error, L"Pool not found", 96);
    }
    json_free(j);
}

static void fetch_jupiter(const CoinItem *c, FetchResult *out)
{
    wchar_t q[128], path[256];
    trim_ws(c->query, q, 128);
    _snwprintf(path, 256, L"/price/v2?ids=%s", q);

    HttpResponse res;
    int ok = http_get(L"api.jup.ag", path, NULL, &res);
    if (!ok || res.statusCode != 200) {
        http_free(&res);
        ok = http_get(L"lite-api.jup.ag", path, NULL, &res);
    }
    if (!ok || res.statusCode != 200) {
        set_http_err(out, &res, L"Jupiter");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    char qUtf8[128]; wide_to_utf8(q, qUtf8, sizeof(qUtf8));
    const JsonVal *d = json_get(json_get(j, "data"), qUtf8);
    if (!d) d = json_first_child(json_get(j, "data"));
    double price = 0;
    if (d && json_as_double(json_get(d, "price"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        lstrcpynW(out->source, L"Jupiter", 32);
        _snwprintf(out->url, 256, L"https://jup.ag/swap/USDC-%s", q);
    } else {
        lstrcpynW(out->error, L"Solana mint not found", 96);
    }
    json_free(j);
}

/* ---- Aggregator Providers ---- */

static const wchar_t *coingecko_id_for(const wchar_t *query, wchar_t *buf, int cap)
{
    static const struct { const wchar_t *sym; const wchar_t *id; } MAP[] = {
        { L"BTC", L"bitcoin" }, { L"ETH", L"ethereum" }, { L"SOL", L"solana" },
        { L"BNB", L"binancecoin" }, { L"XRP", L"ripple" }, { L"DOGE", L"dogecoin" },
        { L"ADA", L"cardano" }, { L"TRX", L"tron" }, { L"TON", L"the-open-network" },
        { L"AVAX", L"avalanche-2" }, { L"SHIB", L"shiba-inu" }, { L"DOT", L"polkadot" },
        { L"LINK", L"chainlink" }, { L"SUI", L"sui" }, { L"PEPE", L"pepe" },
        { L"WIF", L"dogwifcoin" }, { L"BONK", L"bonk" }, { L"LTC", L"litecoin" },
        { L"UNI", L"uniswap" }, { L"ARB", L"arbitrum" }, { L"OP", L"optimism" },
        { L"NEAR", L"near" }, { L"APT", L"aptos" }, { L"MATIC", L"matic-network" },
        { L"POL", L"polygon-ecosystem-token" }, { L"HYPE", L"hyperliquid" },
        { L"FLOKI", L"floki" }, { L"POPCAT", L"popcat" }, { L"TRUMP", L"official-trump" }
    };
    wchar_t base[32], quote[16];
    /* If lowercase slug with hyphens or long word, treat directly as CoinGecko ID */
    int hasDash = wcschr(query, L'-') != NULL && !wcsstr(query, L"USDT") && !wcsstr(query, L"USD");
    if (hasDash) {
        trim_ws(query, buf, cap);
        str_lower_w(buf);
        return buf;
    }
    provider_split_pair(query, base, 32, quote, 16);
    for (int i = 0; i < (int)(sizeof(MAP) / sizeof(MAP[0])); i++) {
        if (_wcsicmp(base, MAP[i].sym) == 0) {
            lstrcpynW(buf, MAP[i].id, cap);
            return buf;
        }
    }
    lstrcpynW(buf, base, cap);
    str_lower_w(buf);
    return buf;
}

static const wchar_t *coingecko_platform(const wchar_t *chain)
{
    if (!chain || !chain[0]) return L"ethereum";
    if (_wcsicmp(chain, L"eth") == 0 || _wcsicmp(chain, L"ethereum") == 0) return L"ethereum";
    if (_wcsicmp(chain, L"bsc") == 0 || _wcsicmp(chain, L"bnb") == 0) return L"binance-smart-chain";
    if (_wcsicmp(chain, L"solana") == 0 || _wcsicmp(chain, L"sol") == 0) return L"solana";
    if (_wcsicmp(chain, L"base") == 0) return L"base";
    if (_wcsicmp(chain, L"arbitrum") == 0) return L"arbitrum-one";
    if (_wcsicmp(chain, L"polygon") == 0) return L"polygon-pos";
    if (_wcsicmp(chain, L"optimism") == 0) return L"optimistic-ethereum";
    if (_wcsicmp(chain, L"avalanche") == 0) return L"avalanche";
    if (_wcsicmp(chain, L"ton") == 0) return L"the-open-network";
    return chain;
}

static void fetch_coingecko(const CoinItem *c, const char *cgApiKey, FetchResult *out)
{
    wchar_t hdr[200] = {0};
    if (cgApiKey && cgApiKey[0]) {
        wchar_t kw[128]; utf8_to_wide(cgApiKey, kw, 128);
        _snwprintf(hdr, 200, L"x-cg-demo-api-key: %s\r\n", kw);
    }

    wchar_t q[128], path[320];
    trim_ws(c->query, q, 128);

    if (provider_detect_input(q) == INPUT_CONTRACT) {
        const wchar_t *plat = coingecko_platform(c->chain);
        _snwprintf(path, 320, L"/api/v3/simple/token_price/%s?contract_addresses=%s&vs_currencies=usd&include_24hr_change=true", plat, q);
    } else {
        wchar_t id[64];
        coingecko_id_for(q, id, 64);
        _snwprintf(path, 320, L"/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true", id);
    }
    path[319] = 0;

    HttpResponse res;
    if (!http_get(L"api.coingecko.com", path, hdr[0] ? hdr : NULL, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"CoinGecko");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    const JsonVal *entry = json_first_child(j);
    double price = 0, pct = 0;
    if (entry && json_as_double(json_get(entry, "usd"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(entry, "usd_24h_change"), &pct)) {
            out->change24h = pct;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->source, L"CoinGecko", 32);
        if (entry->key) {
            wchar_t ek[64]; utf8_to_wide(entry->key, ek, 64);
            _snwprintf(out->url, 256, L"https://www.coingecko.com/en/coins/%s", ek);
        }
    } else {
        lstrcpynW(out->error, L"Coin ID not found", 96);
    }
    json_free(j);
}

static void fetch_cmc(const CoinItem *c, const char *cmcApiKey, FetchResult *out)
{
    if (!cmcApiKey || !cmcApiKey[0]) {
        lstrcpynW(out->source, L"CoinMarketCap", 32);
        lstrcpynW(out->error, L"Missing CMC API Key in Settings", 96);
        return;
    }
    wchar_t base[32], quote[16];
    provider_split_pair(c->query, base, 32, quote, 16);

    wchar_t kw[128], hdr[200], path[160];
    utf8_to_wide(cmcApiKey, kw, 128);
    _snwprintf(hdr, 200, L"X-CMC_PRO_API_KEY: %s\r\n", kw);
    _snwprintf(path, 160, L"/v2/cryptocurrency/quotes/latest?symbol=%s&convert=USD", base);

    HttpResponse res;
    if (!http_get(L"pro-api.coinmarketcap.com", path, hdr, &res) || res.statusCode != 200) {
        set_http_err(out, &res, L"CoinMarketCap");
        http_free(&res);
        return;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) { lstrcpynW(out->error, L"Invalid JSON", 96); return; }

    char symA[32]; wide_to_utf8(base, symA, sizeof(symA));
    const JsonVal *node = json_get_ci(json_get(j, "data"), symA);
    if (node && node->type == JSON_ARRAY) node = json_at(node, 0);
    const JsonVal *usd = json_get(json_get(node, "quote"), "USD");
    double price = 0, pct = 0;
    if (usd && json_as_double(json_get(usd, "price"), &price) && price > 0) {
        out->ok = 1;
        out->price = price;
        if (json_as_double(json_get(usd, "percent_change_24h"), &pct)) {
            out->change24h = pct;
            out->hasChange24h = 1;
        }
        lstrcpynW(out->symbol, base, 24);
        lstrcpynW(out->source, L"CoinMarketCap", 32);
        const char *slug = json_as_str(json_get(node, "slug"), "");
        if (slug[0]) {
            wchar_t slw[64]; utf8_to_wide(slug, slw, 64);
            _snwprintf(out->url, 256, L"https://coinmarketcap.com/currencies/%s/", slw);
        }
    } else {
        lstrcpynW(out->error, L"Symbol not found on CMC", 96);
    }
    json_free(j);
}

/* ---- Auto Router ---- */

static void fetch_auto(const CoinItem *c, const char *cmcApiKey, const char *cgApiKey, FetchResult *out)
{
    InputKind k = provider_detect_input(c->query);
    if (k == INPUT_CONTRACT || k == INPUT_DEX_PAIR) {
        fetch_dexscreener(c, out);
        if (out->ok) return;
        ZeroMemory(out, sizeof(*out));
        fetch_geckoterminal(c, out);
        return;
    }

    /* Try major public CEXs in order of speed & coverage, then aggregators/DEX */
    static const ProviderId ORDER[] = {
        PROV_BINANCE, PROV_OKX, PROV_MEXC, PROV_GATE, PROV_BYBIT, PROV_COINGECKO, PROV_DEXSCREENER
    };
    for (int i = 0; i < (int)(sizeof(ORDER) / sizeof(ORDER[0])); i++) {
        ZeroMemory(out, sizeof(*out));
        CoinItem tmp = *c;
        tmp.provider = ORDER[i];
        provider_fetch_one(&tmp, cmcApiKey, cgApiKey, out);
        if (out->ok) return;
    }
}

void provider_fetch_one(const CoinItem *coin, const char *cmcApiKey, const char *cgApiKey, FetchResult *out)
{
    ZeroMemory(out, sizeof(*out));
    if (!coin || !coin->query[0]) {
        lstrcpynW(out->error, L"Empty query", 96);
        return;
    }
    switch (coin->provider) {
    case PROV_AUTO:            fetch_auto(coin, cmcApiKey, cgApiKey, out); break;
    case PROV_BINANCE:         fetch_binance_like(coin, L"api.binance.com", L"data-api.binance.vision", L"/api/v3/ticker/24hr", L"Binance", 0, 0, out); break;
    case PROV_BINANCE_FUTURES: fetch_binance_like(coin, L"fapi.binance.com", NULL, L"/fapi/v1/ticker/24hr", L"Binance Perp", 1, 0, out); break;
    case PROV_OKX:             fetch_okx(coin, out); break;
    case PROV_BYBIT:           fetch_bybit(coin, out); break;
    case PROV_GATE:            fetch_gate(coin, out); break;
    case PROV_MEXC:            fetch_binance_like(coin, L"api.mexc.com", NULL, L"/api/v3/ticker/24hr", L"MEXC", 0, 1, out); break;
    case PROV_KUCOIN:          fetch_kucoin(coin, out); break;
    case PROV_BITGET:          fetch_bitget(coin, out); break;
    case PROV_COINBASE:        fetch_coinbase(coin, out); break;
    case PROV_KRAKEN:          fetch_kraken(coin, out); break;
    case PROV_HTX:             fetch_htx(coin, out); break;
    case PROV_DEXSCREENER:     fetch_dexscreener(coin, out); break;
    case PROV_GECKOTERMINAL:   fetch_geckoterminal(coin, out); break;
    case PROV_JUPITER:         fetch_jupiter(coin, out); break;
    case PROV_COINGECKO:       fetch_coingecko(coin, cgApiKey, out); break;
    case PROV_COINMARKETCAP:   fetch_cmc(coin, cmcApiKey, out); break;
    default:                   fetch_auto(coin, cmcApiKey, cgApiKey, out); break;
    }
}

/* ---- Batch Fetchers (conserves rate limits & cuts latency) ---- */

static int batch_binance(const CoinItem *const *coins, int count, FetchResult *results)
{
    if (count < 2) return 0;
    wchar_t syms[MAX_COINS][48];
    wchar_t bases[MAX_COINS][32];
    wchar_t quotes[MAX_COINS][16];
    wchar_t jsonArr[1024] = L"[";
    int pos = 1;

    for (int i = 0; i < count; i++) {
        provider_split_pair(coins[i]->query, bases[i], 32, quotes[i], 16);
        _snwprintf(syms[i], 48, L"%s%s", bases[i], quotes[i]);
        int n = _snwprintf(jsonArr + pos, 1024 - pos, L"%s\"%s\"", i ? L"," : L"", syms[i]);
        if (n <= 0 || pos + n >= 1000) return 0;
        pos += n;
    }
    lstrcatW(jsonArr, L"]");

    wchar_t enc[2048], path[2200];
    http_url_encode_w(jsonArr, enc, 2048);
    _snwprintf(path, 2200, L"/api/v3/ticker/24hr?symbols=%s", enc);

    HttpResponse res;
    int ok = http_get(L"api.binance.com", path, NULL, &res);
    if (!ok || res.statusCode == 451 || res.statusCode == 403 || res.statusCode == 0) {
        http_free(&res);
        ok = http_get(L"data-api.binance.vision", path, NULL, &res);
    }
    if (!ok || res.statusCode != 200) {
        /* If one bad symbol broke the batch (HTTP 400), return 0 so caller falls back per-coin */
        if (res.statusCode == 400) { http_free(&res); return 0; }
        for (int i = 0; i < count; i++) set_http_err(&results[i], &res, L"Binance");
        http_free(&res);
        return count;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j || j->type != JSON_ARRAY) { if (j) json_free(j); return 0; }

    for (int i = 0; i < count; i++) {
        char symA[48]; wide_to_utf8(syms[i], symA, sizeof(symA));
        for (const JsonVal *el = j->child; el; el = el->next) {
            const char *s = json_as_str(json_get(el, "symbol"), "");
            if (_stricmp(s, symA) == 0) {
                double price = 0, pct = 0;
                if (json_as_double(json_get(el, "lastPrice"), &price) && price > 0) {
                    results[i].ok = 1;
                    results[i].price = price;
                    if (json_as_double(json_get(el, "priceChangePercent"), &pct)) {
                        results[i].change24h = pct;
                        results[i].hasChange24h = 1;
                    }
                    lstrcpynW(results[i].symbol, bases[i], 24);
                    lstrcpynW(results[i].source, L"Binance", 32);
                    _snwprintf(results[i].url, 256, L"https://www.binance.com/en/trade/%s_%s", bases[i], quotes[i]);
                }
                break;
            }
        }
        if (!results[i].ok) lstrcpynW(results[i].error, L"Pair not found", 96);
    }
    json_free(j);
    return count;
}

static int batch_dexscreener_tokens(const CoinItem *const *coins, int count, FetchResult *results)
{
    if (count < 2) return 0;
    /* Only batch pure contract addresses (up to 30) */
    for (int i = 0; i < count; i++)
        if (provider_detect_input(coins[i]->query) != INPUT_CONTRACT) return 0;
    if (count > 30) count = 30;

    wchar_t path[2400] = L"/latest/dex/tokens/";
    int pos = lstrlenW(path);
    for (int i = 0; i < count; i++) {
        wchar_t q[128]; trim_ws(coins[i]->query, q, 128);
        int n = _snwprintf(path + pos, 2400 - pos, L"%s%s", i ? L"," : L"", q);
        if (n <= 0 || pos + n >= 2380) return 0;
        pos += n;
    }

    HttpResponse res;
    if (!http_get(L"api.dexscreener.com", path, NULL, &res) || res.statusCode != 200) {
        for (int i = 0; i < count; i++) set_http_err(&results[i], &res, L"DexScreener");
        http_free(&res);
        return count;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) return 0;

    const JsonVal *pairs = json_get(j, "pairs");
    for (int i = 0; i < count; i++) {
        wchar_t q[128]; trim_ws(coins[i]->query, q, 128);
        const JsonVal *best = pick_best_dex_pair(pairs, coins[i]->chain, q);
        if (best) fill_from_dex_pair(best, &results[i]);
        else lstrcpynW(results[i].error, L"Token not found", 96);
    }
    json_free(j);
    return count;
}

static int batch_coingecko_ids(const CoinItem *const *coins, int count, const char *cgApiKey, FetchResult *results)
{
    if (count < 2) return 0;
    for (int i = 0; i < count; i++)
        if (provider_detect_input(coins[i]->query) == INPUT_CONTRACT) return 0;

    wchar_t ids[MAX_COINS][64];
    wchar_t path[1500] = L"/api/v3/simple/price?ids=";
    int pos = lstrlenW(path);
    for (int i = 0; i < count; i++) {
        coingecko_id_for(coins[i]->query, ids[i], 64);
        int n = _snwprintf(path + pos, 1400 - pos, L"%s%s", i ? L"," : L"", ids[i]);
        if (n <= 0 || pos + n >= 1350) return 0;
        pos += n;
    }
    lstrcatW(path, L"&vs_currencies=usd&include_24hr_change=true");

    wchar_t hdr[200] = {0};
    if (cgApiKey && cgApiKey[0]) {
        wchar_t kw[128]; utf8_to_wide(cgApiKey, kw, 128);
        _snwprintf(hdr, 200, L"x-cg-demo-api-key: %s\r\n", kw);
    }

    HttpResponse res;
    if (!http_get(L"api.coingecko.com", path, hdr[0] ? hdr : NULL, &res) || res.statusCode != 200) {
        for (int i = 0; i < count; i++) set_http_err(&results[i], &res, L"CoinGecko");
        http_free(&res);
        return count;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) return 0;

    for (int i = 0; i < count; i++) {
        char idA[64]; wide_to_utf8(ids[i], idA, sizeof(idA));
        const JsonVal *entry = json_get_ci(j, idA);
        double price = 0, pct = 0;
        if (entry && json_as_double(json_get(entry, "usd"), &price) && price > 0) {
            results[i].ok = 1;
            results[i].price = price;
            if (json_as_double(json_get(entry, "usd_24h_change"), &pct)) {
                results[i].change24h = pct;
                results[i].hasChange24h = 1;
            }
            lstrcpynW(results[i].source, L"CoinGecko", 32);
            _snwprintf(results[i].url, 256, L"https://www.coingecko.com/en/coins/%s", ids[i]);
        } else {
            lstrcpynW(results[i].error, L"Coin ID not found", 96);
        }
    }
    json_free(j);
    return count;
}

static int batch_cmc(const CoinItem *const *coins, int count, const char *cmcApiKey, FetchResult *results)
{
    if (count < 2) return 0;
    if (!cmcApiKey || !cmcApiKey[0]) {
        for (int i = 0; i < count; i++) {
            lstrcpynW(results[i].source, L"CoinMarketCap", 32);
            lstrcpynW(results[i].error, L"Missing CMC API Key in Settings", 96);
        }
        return count;
    }
    wchar_t bases[MAX_COINS][32], quote[16];
    wchar_t path[1024] = L"/v2/cryptocurrency/quotes/latest?symbol=";
    int pos = lstrlenW(path);
    for (int i = 0; i < count; i++) {
        provider_split_pair(coins[i]->query, bases[i], 32, quote, 16);
        int n = _snwprintf(path + pos, 950 - pos, L"%s%s", i ? L"," : L"", bases[i]);
        if (n <= 0 || pos + n >= 940) return 0;
        pos += n;
    }
    lstrcatW(path, L"&convert=USD");

    wchar_t kw[128], hdr[200];
    utf8_to_wide(cmcApiKey, kw, 128);
    _snwprintf(hdr, 200, L"X-CMC_PRO_API_KEY: %s\r\n", kw);

    HttpResponse res;
    if (!http_get(L"pro-api.coinmarketcap.com", path, hdr, &res) || res.statusCode != 200) {
        for (int i = 0; i < count; i++) set_http_err(&results[i], &res, L"CoinMarketCap");
        http_free(&res);
        return count;
    }
    JsonVal *j = json_parse(res.body);
    http_free(&res);
    if (!j) return 0;

    const JsonVal *data = json_get(j, "data");
    for (int i = 0; i < count; i++) {
        char symA[32]; wide_to_utf8(bases[i], symA, sizeof(symA));
        const JsonVal *node = json_get_ci(data, symA);
        if (node && node->type == JSON_ARRAY) node = json_at(node, 0);
        const JsonVal *usd = json_get(json_get(node, "quote"), "USD");
        double price = 0, pct = 0;
        if (usd && json_as_double(json_get(usd, "price"), &price) && price > 0) {
            results[i].ok = 1;
            results[i].price = price;
            if (json_as_double(json_get(usd, "percent_change_24h"), &pct)) {
                results[i].change24h = pct;
                results[i].hasChange24h = 1;
            }
            lstrcpynW(results[i].symbol, bases[i], 24);
            lstrcpynW(results[i].source, L"CoinMarketCap", 32);
            const char *slug = json_as_str(json_get(node, "slug"), "");
            if (slug[0]) {
                wchar_t slw[64]; utf8_to_wide(slug, slw, 64);
                _snwprintf(results[i].url, 256, L"https://coinmarketcap.com/currencies/%s/", slw);
            }
        } else {
            lstrcpynW(results[i].error, L"Symbol not found on CMC", 96);
        }
    }
    json_free(j);
    return count;
}

int provider_fetch_batch(ProviderId prov, const CoinItem *const *coins, int count,
                         const char *cmcApiKey, const char *cgApiKey, FetchResult *results)
{
    if (!coins || count < 2 || !results) return 0;
    for (int i = 0; i < count; i++) ZeroMemory(&results[i], sizeof(results[i]));
    switch (prov) {
    case PROV_BINANCE:       return batch_binance(coins, count, results);
    case PROV_DEXSCREENER:   return batch_dexscreener_tokens(coins, count, results);
    case PROV_COINGECKO:     return batch_coingecko_ids(coins, count, cgApiKey, results);
    case PROV_COINMARKETCAP: return batch_cmc(coins, count, cmcApiKey, results);
    default:                 return 0;
    }
}
