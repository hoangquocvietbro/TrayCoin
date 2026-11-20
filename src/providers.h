#ifndef TRAYCOIN_PROVIDERS_H
#define TRAYCOIN_PROVIDERS_H

#include "types.h"

typedef struct {
    int     ok;
    int     rateLimited;          /* 1 if HTTP 429 received */
    double  price;
    double  change24h;
    int     hasChange24h;
    wchar_t symbol[24];           /* Resolved base symbol e.g. "PEPE" */
    wchar_t source[32];           /* Resolved source e.g. "Binance", "DexScreener" */
    wchar_t url[256];             /* Chart URL */
    wchar_t error[96];            /* Error detail if !ok */
} FetchResult;

typedef struct {
    wchar_t symbol[24];
    wchar_t name[48];
    wchar_t chain[24];
    wchar_t dex[24];
    wchar_t contract[96];         /* Base token address */
    wchar_t pairAddress[96];
    wchar_t url[256];
    double  price;
    double  change24h;
    double  liquidityUsd;
} DexSearchHit;

/* Provider metadata */
const wchar_t *provider_display_name(ProviderId id);
const char    *provider_key(ProviderId id);
ProviderId     provider_from_key(const char *key);

/* Input analysis & pair helpers */
InputKind provider_detect_input(const wchar_t *query);
void      provider_split_pair(const wchar_t *query, wchar_t *base, int baseCap, wchar_t *quote, int quoteCap);
void      provider_default_label(const CoinItem *c, wchar_t *out, int outCap);

/* Fetch a single coin quote (thread-safe; does not touch global state). */
void provider_fetch_one(const CoinItem *coin, const char *cmcApiKey, const char *cgApiKey, FetchResult *out);

/* Batch fetch coins sharing a batch-capable provider.
   Returns number of items attempted in batch (0 if provider doesn't batch). */
int  provider_fetch_batch(ProviderId prov, const CoinItem *const *coins, int count,
                          const char *cmcApiKey, const char *cgApiKey, FetchResult *results);

/* Search DexScreener for meme coins / contracts / names. Returns number of hits (<= maxHits). */
int  provider_search_dex(const wchar_t *query, const wchar_t *chainFilter, DexSearchHit *hits, int maxHits);

#endif /* TRAYCOIN_PROVIDERS_H */
