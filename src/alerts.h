#ifndef TRAYCOIN_ALERTS_H
#define TRAYCOIN_ALERTS_H

#include "types.h"

typedef struct {
    wchar_t title[64];
    wchar_t body[224];
    int     playSound;
} AlertNotification;

/* Check thresholds under g_stateCs; posts WM_PRICE_ALERT to g_wnd if triggered. */
void alerts_check_locked(CoinItem *c, double newPrice);

/* Called on UI thread when WM_PRICE_ALERT arrives; frees AlertNotification*. */
void alerts_handle_ui_message(AlertNotification *msg);

#endif /* TRAYCOIN_ALERTS_H */
