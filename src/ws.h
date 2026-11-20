#ifndef TRAYCOIN_WS_H
#define TRAYCOIN_WS_H

#include "types.h"

void ws_start(void);
void ws_stop(void);

/* Notify WebSocket manager that coin list, enableWebSocket, or g_paused changed. */
void ws_reload(void);

/* Abort any stale/half-open sockets, reset backoff timers, and reconnect immediately (non-blocking). */
void ws_force_reconnect(void);

/* Returns 1 if a provider supports real-time WebSocket streaming. */
int  ws_provider_supported(ProviderId prov);

#endif /* TRAYCOIN_WS_H */
