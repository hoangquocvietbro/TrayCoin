#ifndef TRAYCOIN_POLLER_H
#define TRAYCOIN_POLLER_H

#include "types.h"

void poller_start(void);
void poller_stop(void);

/* Immediately fetch all enabled coins (called by Refresh icon or menu). */
void poller_trigger_refresh(int includeDisabled);

/* Immediately fetch only coins whose bit (1ULL << idx) is set in mask. */
void poller_trigger_refresh_mask(uint64_t mask);

/* Wake the poller after config/interval/watchlist changes. */
void poller_wake(void);

/* Reset error backoffs and immediately re-poll enabled coins after Sleep/Wi-Fi recovery. */
void poller_on_network_recover(void);

#endif /* TRAYCOIN_POLLER_H */
