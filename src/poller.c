/*
 * TrayCoin - Taskbar & Widget Crypto Tracker
 * Copyright (C) 2026 hoangquocvietbro (@hoangquocvietbro)
 * Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
 */
#include "poller.h"
#include "providers.h"
#include "taskbar.h"
#include "ws.h"
#include "alerts.h"
#include "logger.h"
#include <objbase.h>
#include <math.h>

#define MAX_PARALLEL_WORKERS 6

static HANDLE        g_pollThread = NULL;
static HANDLE        g_uiaThread  = NULL;
static HANDLE        g_wakeEvent  = NULL;
static HANDLE        g_uiaEvent   = NULL;
static volatile LONG   g_forceMode  = 0; /* 0 = normal, 1 = enabled only, 2 = all */
static volatile LONG64 g_forceMask  = 0; /* bitmask for selective coin refresh (up to 64 coins) */

typedef struct {
    volatile LONG refs;          /* starts at jobCount + 1 (caller holds 1 ref) */
    volatile LONG remaining;     /* decremented as each job finishes */
    volatile LONG autoResolvedWs;
    HANDLE        doneEv;
} BatchSync;

typedef struct {
    int        isBatch;
    ProviderId prov;
    int        count;
    CoinItem   coins[MAX_COINS];
    int        coinIdx[MAX_COINS];
    char       cmcKey[128];
    char       cgKey[128];
    HANDLE     sem;
    BatchSync *sync;
} PollJob;

static void batch_sync_release(BatchSync *bs)
{
    if (!bs) return;
    if (InterlockedDecrement(&bs->refs) == 0) {
        if (bs->doneEv) CloseHandle(bs->doneEv);
        HeapFree(GetProcessHeap(), 0, bs);
    }
}

void poller_trigger_refresh(int includeDisabled)
{
    /* Any explicit manual refresh guarantees the user is active -> unpause if paused */
    if (g_paused) {
        InterlockedExchange(&g_paused, 0);
        ws_reload();
    }
    LONG mode = includeDisabled ? 2 : 1;
    LONG cur = g_forceMode;
    if (mode > cur) InterlockedExchange(&g_forceMode, mode);
    LOGI(L"POLLER", L"Manual refresh triggered (mode=%ld)", mode);
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void poller_trigger_refresh_mask(uint64_t mask)
{
    if (!mask) return;
    if (g_paused) {
        InterlockedExchange(&g_paused, 0);
        ws_reload();
    }
    InterlockedOr64(&g_forceMask, (LONG64)mask);
    LOGI(L"POLLER", L"Selective refresh triggered (mask=0x%016llX)", (unsigned long long)mask);
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void poller_wake(void)
{
    if (g_wakeEvent) SetEvent(g_wakeEvent);
    if (g_uiaEvent)  SetEvent(g_uiaEvent);
}

void poller_on_network_recover(void)
{
    if (g_paused) InterlockedExchange(&g_paused, 0);
    EnterCriticalSection(&g_stateCs);
    for (int i = 0; i < g_cfg.coinCount; i++) {
        CoinItem *c = &g_cfg.coins[i];
        if (!c->enabled) continue;
        c->failCount = 0;
        c->nextFetchMs = 0;
    }
    LeaveCriticalSection(&g_stateCs);
    LOGI(L"POLLER", L"Network/power recovery: cleared error backoffs and waking poller");
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

static ULONGLONG coin_interval_ms(const AppConfig *cfg, const CoinItem *c)
{
    DWORD sec = c->intervalSec > 0 ? c->intervalSec : cfg->globalIntervalSec;
    if (sec < MIN_INTERVAL_SEC) sec = MIN_INTERVAL_SEC;
    if (sec > MAX_INTERVAL_SEC) sec = MAX_INTERVAL_SEC;
    return (ULONGLONG)sec * 1000ULL;
}

void coin_record_tick_locked(CoinItem *c, double newPrice, ULONGLONG now)
{
    if (!c || newPrice <= 0.0) return;
    if (c->ok && fabs(newPrice - c->price) > (c->price * 1e-9)) {
        c->flashDir = (newPrice > c->price) ? 1 : -1;
        c->flashStartMs = now;
    }
    if (c->histCount == 0 || (now - c->lastHistMs >= 5000ULL)) {
        c->history[c->histHead] = newPrice;
        c->histHead = (c->histHead + 1) % COIN_HIST_LEN;
        if (c->histCount < COIN_HIST_LEN) c->histCount++;
        c->lastHistMs = now;
    } else {
        int last = (c->histHead + COIN_HIST_LEN - 1) % COIN_HIST_LEN;
        c->history[last] = newPrice;
    }
    alerts_check_locked(c, newPrice);
}

static void apply_result_locked(int idx, const CoinItem *snap, const FetchResult *r, ULONGLONG now, volatile LONG *autoResolvedWs)
{
    if (idx < 0 || idx >= g_cfg.coinCount) return;
    CoinItem *c = &g_cfg.coins[idx];
    if (c->provider != snap->provider || lstrcmpW(c->query, snap->query) != 0) return;

    c->fetching = 0;
    ULONGLONG baseIv = coin_interval_ms(&g_cfg, c);

    if (r->ok) {
        int wasEmptySrc = (c->resolvedSource[0] == 0);
        coin_record_tick_locked(c, r->price, now);
        c->prevPrice    = c->ok ? c->price : r->price;
        c->price        = r->price;
        c->change24h    = r->change24h;
        c->hasChange24h = r->hasChange24h;
        c->ok           = 1;
        c->failCount    = 0;
        c->lastUpdateMs = now;
        GetLocalTime(&c->lastUpdateTime);
        c->nextFetchMs  = now + baseIv;
        if (r->source[0]) lstrcpynW(c->resolvedSource, r->source, 32);
        if (r->symbol[0]) lstrcpynW(c->resolvedSymbol, r->symbol, 24);
        if (r->url[0])    lstrcpynW(c->pairUrl, r->url, 256);
        c->lastError[0] = 0;

        if (wasEmptySrc && c->provider == PROV_AUTO && autoResolvedWs &&
            (lstrcmpW(c->resolvedSource, L"Binance") == 0 ||
             lstrcmpW(c->resolvedSource, L"OKX") == 0 ||
             lstrcmpW(c->resolvedSource, L"Bybit") == 0 ||
             lstrcmpW(c->resolvedSource, L"Gate.io") == 0)) {
            InterlockedExchange(autoResolvedWs, 1);
        }
    } else {
        c->failCount++;
        if (r->source[0]) lstrcpynW(c->resolvedSource, r->source, 32);
        lstrcpynW(c->lastError, r->error[0] ? r->error : L"Fetch failed", 96);

        ULONGLONG mult = 1ULL << (c->failCount > 5 ? 5 : c->failCount);
        ULONGLONG delay = r->rateLimited ? (baseIv * mult * 2) : (baseIv * mult);
        if (delay > 300000ULL) delay = 300000ULL;
        if (delay < 5000ULL) delay = 5000ULL;
        c->nextFetchMs = now + delay;

        LOGW(L"POLLER", L"Fetch failed for %s (%s): %s (retry in %llums)",
             c->query, provider_display_name(c->provider), c->lastError, (unsigned long long)delay);
    }
}

static VOID CALLBACK job_tp_callback(PTP_CALLBACK_INSTANCE inst, PVOID context)
{
    (void)inst;
    PollJob *job = (PollJob *)context;
    BatchSync *bs = job->sync;

    if (!g_quit) {
        if (job->isBatch && job->count >= 2) {
            const CoinItem *grp[MAX_COINS];
            for (int k = 0; k < job->count; k++) grp[k] = &job->coins[k];

            FetchResult *bres = (FetchResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                         sizeof(FetchResult) * (size_t)job->count);
            int done = 0;
            if (bres) {
                done = provider_fetch_batch(job->prov, grp, job->count, job->cmcKey, job->cgKey, bres);
                if (done > 0) {
                    ULONGLONG tnow = GetTickCount64();
                    EnterCriticalSection(&g_stateCs);
                    for (int k = 0; k < done; k++) {
                        apply_result_locked(job->coinIdx[k], &job->coins[k], &bres[k], tnow, &bs->autoResolvedWs);
                    }
                    LeaveCriticalSection(&g_stateCs);
                    if (g_wnd) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
                }
                HeapFree(GetProcessHeap(), 0, bres);
            }
            if (done == 0 && !g_quit) {
                for (int k = 0; k < job->count && !g_quit; k++) {
                    FetchResult r;
                    provider_fetch_one(&job->coins[k], job->cmcKey, job->cgKey, &r);
                    ULONGLONG tnow = GetTickCount64();
                    EnterCriticalSection(&g_stateCs);
                    apply_result_locked(job->coinIdx[k], &job->coins[k], &r, tnow, &bs->autoResolvedWs);
                    LeaveCriticalSection(&g_stateCs);
                    if (g_wnd) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
                }
            }
        } else if (job->count == 1) {
            FetchResult r;
            provider_fetch_one(&job->coins[0], job->cmcKey, job->cgKey, &r);
            ULONGLONG tnow = GetTickCount64();
            EnterCriticalSection(&g_stateCs);
            apply_result_locked(job->coinIdx[0], &job->coins[0], &r, tnow, &bs->autoResolvedWs);
            LeaveCriticalSection(&g_stateCs);
            if (g_wnd) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
        }
    }

    ReleaseSemaphore(job->sem, 1, NULL);
    if (InterlockedDecrement(&bs->remaining) == 0) {
        SetEvent(bs->doneEv);
    }
    batch_sync_release(bs);
    HeapFree(GetProcessHeap(), 0, job);
}

/* Dedicated UIAutomation taskbar scanner thread (never blocks price polling) */
static DWORD WINAPI uia_proc(void *unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (!com) {
        LOGW(L"UIA", L"CoInitializeEx failed in uia_proc");
        return 0;
    }

    for (;;) {
        if (g_quit) break;
        if (g_paused) {
            WaitForSingleObject(g_uiaEvent, INFINITE);
            continue;
        }
        taskbar_uia_scan();
        if (g_wnd) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
        WaitForSingleObject(g_uiaEvent, 2000);
    }

    taskbar_uia_cleanup();
    CoUninitialize();
    return 0;
}

static CoinItem g_dueSnap[MAX_COINS];

static DWORD WINAPI poller_proc(void *unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    HANDLE sem = CreateSemaphoreW(NULL, MAX_PARALLEL_WORKERS, MAX_PARALLEL_WORKERS, NULL);
    if (!sem) {
        LOGE(L"POLLER", L"CreateSemaphoreW failed (%lu)", (unsigned long)GetLastError());
        return 0;
    }

    LOGI(L"POLLER", L"Poller thread started (workers=%d)", MAX_PARALLEL_WORKERS);

    for (;;) {
        if (g_quit) break;

        if (g_paused) {
            WaitForSingleObject(g_wakeEvent, INFINITE);
            continue;
        }

        ULONGLONG now = GetTickCount64();
        LONG force = InterlockedExchange(&g_forceMode, 0);
        uint64_t fmask = (uint64_t)InterlockedExchange64(&g_forceMask, 0);

        int dueIdx[MAX_COINS];
        int dueCount = 0;
        char cmcKey[128] = {0}, cgKey[128] = {0};
        ULONGLONG soonestMs = now + 2000ULL;

        EnterCriticalSection(&g_stateCs);
        lstrcpynA(cmcKey, g_cfg.cmcApiKey, sizeof(cmcKey));
        lstrcpynA(cgKey,  g_cfg.cgApiKey,  sizeof(cgKey));
        int wsEnabled = g_cfg.enableWebSocket;
        for (int i = 0; i < g_cfg.coinCount; i++) {
            CoinItem *c = &g_cfg.coins[i];
            int forcedThis = (i < 64) && ((fmask & (1ULL << i)) != 0);
            if (!c->enabled && force < 2 && !forcedThis) continue;

            if (force == 0 && !forcedThis && wsEnabled && c->wsLive && (now - c->wsLastTickMs < 15000ULL)) {
                continue;
            }
            if (c->wsLive && (now - c->wsLastTickMs >= 15000ULL)) {
                c->wsLive = 0;
            }

            if (force > 0 || forcedThis || c->nextFetchMs == 0 || now >= c->nextFetchMs) {
                if (force > 0 || forcedThis) c->failCount = 0;
                c->fetching = 1;
                g_dueSnap[dueCount] = *c;
                dueIdx[dueCount] = i;
                dueCount++;
            } else if (c->nextFetchMs < soonestMs) {
                soonestMs = c->nextFetchMs;
            }
        }
        LeaveCriticalSection(&g_stateCs);

        if (dueCount > 0) {
            ULONGLONG cycleStart = GetTickCount64();
            InterlockedExchange(&g_anyFetching, 1);
            if (g_wnd) PostMessageW(g_wnd, WM_REFRESH_STATE, 1, 0);

            int handled[MAX_COINS] = {0};
            PollJob *jobs[MAX_COINS];
            int jobCount = 0;

            BatchSync *bs = (BatchSync *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BatchSync));
            if (!bs) { InterlockedExchange(&g_anyFetching, 0); continue; }
            bs->doneEv = CreateEventW(NULL, TRUE, FALSE, NULL);
            bs->refs = 1; /* caller's ref */

            static const ProviderId BATCH_PROVS[] = {
                PROV_BINANCE, PROV_DEXSCREENER, PROV_COINGECKO, PROV_COINMARKETCAP
            };
            for (int bp = 0; bp < (int)(sizeof(BATCH_PROVS) / sizeof(BATCH_PROVS[0])); bp++) {
                ProviderId p = BATCH_PROVS[bp];
                int gcnt = 0, grpPos[MAX_COINS];
                for (int i = 0; i < dueCount; i++) {
                    if (!handled[i] && g_dueSnap[i].provider == p) grpPos[gcnt++] = i;
                }
                int maxChunk = (p == PROV_DEXSCREENER) ? 25 : 30;
                for (int off = 0; gcnt - off >= 2 && jobCount < MAX_COINS; off += maxChunk) {
                    int take = gcnt - off;
                    if (take > maxChunk) take = maxChunk;
                    PollJob *j = (PollJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PollJob));
                    if (!j) continue;
                    j->isBatch = 1;
                    j->prov    = p;
                    j->count   = take;
                    for (int k = 0; k < take; k++) {
                        j->coins[k]   = g_dueSnap[grpPos[off + k]];
                        j->coinIdx[k] = dueIdx[grpPos[off + k]];
                        handled[grpPos[off + k]] = 1;
                    }
                    lstrcpynA(j->cmcKey, cmcKey, sizeof(j->cmcKey));
                    lstrcpynA(j->cgKey,  cgKey,  sizeof(j->cgKey));
                    j->sem  = sem;
                    j->sync = bs;
                    jobs[jobCount++] = j;
                }
            }

            for (int i = 0; i < dueCount && jobCount < MAX_COINS; i++) {
                if (handled[i]) continue;
                PollJob *j = (PollJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PollJob));
                if (!j) continue;
                j->isBatch    = 0;
                j->prov       = g_dueSnap[i].provider;
                j->count      = 1;
                j->coins[0]   = g_dueSnap[i];
                j->coinIdx[0] = dueIdx[i];
                lstrcpynA(j->cmcKey, cmcKey, sizeof(j->cmcKey));
                lstrcpynA(j->cgKey,  cgKey,  sizeof(j->cgKey));
                j->sem  = sem;
                j->sync = bs;
                jobs[jobCount++] = j;
            }

            bs->remaining = jobCount;
            bs->refs = jobCount + 1;
            LOGD(L"POLLER", L"Cycle start: due=%d jobs=%d force=%ld", dueCount, jobCount, force);

            for (int i = 0; i < jobCount; i++) {
                WaitForSingleObject(sem, 15000);
                if (!TrySubmitThreadpoolCallback(job_tp_callback, jobs[i], NULL)) {
                    job_tp_callback(NULL, jobs[i]);
                }
            }
            if (jobCount > 0) {
                DWORD wr = WaitForSingleObject(bs->doneEv, 15000);
                if (wr == WAIT_TIMEOUT) {
                    LOGW(L"POLLER", L"Poll cycle timed out after 15s (remaining=%ld)", bs->remaining);
                }
            }

            LONG wantWsReload = bs->autoResolvedWs;
            batch_sync_release(bs);

            InterlockedExchange(&g_anyFetching, 0);
            LOGD(L"POLLER", L"Cycle done in %llums", (unsigned long long)(GetTickCount64() - cycleStart));
            if (wantWsReload) ws_reload();
            if (g_wnd) PostMessageW(g_wnd, WM_REFRESH_STATE, 0, 0);
            if (g_wnd) PostMessageW(g_wnd, WM_PRICES_UPDATED, 0, 0);
            continue;
        }

        ULONGLONG wait = (soonestMs > now) ? (soonestMs - now) : 250ULL;
        if (wait > 2000ULL) wait = 2000ULL;
        if (wait < 100ULL) wait = 100ULL;
        WaitForSingleObject(g_wakeEvent, (DWORD)wait);
    }

    CloseHandle(sem);
    LOGI(L"POLLER", L"Poller thread stopped");
    return 0;
}

void poller_start(void)
{
    if (g_pollThread) return;
    g_wakeEvent  = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_uiaEvent   = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_pollThread = CreateThread(NULL, 0, poller_proc, NULL, 0, NULL);
    if (g_wnd) {
        g_uiaThread = CreateThread(NULL, 0, uia_proc, NULL, 0, NULL);
    }
}

void poller_stop(void)
{
    g_quit = 1;
    if (g_wakeEvent) SetEvent(g_wakeEvent);
    if (g_uiaEvent)  SetEvent(g_uiaEvent);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 4000);
        CloseHandle(g_pollThread);
        g_pollThread = NULL;
    }
    if (g_uiaThread) {
        WaitForSingleObject(g_uiaThread, 2000);
        CloseHandle(g_uiaThread);
        g_uiaThread = NULL;
    }
    if (g_wakeEvent) { CloseHandle(g_wakeEvent); g_wakeEvent = NULL; }
    if (g_uiaEvent)  { CloseHandle(g_uiaEvent);  g_uiaEvent  = NULL; }
}
