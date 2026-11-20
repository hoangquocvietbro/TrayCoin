#ifndef TRAYCOIN_TASKBAR_H
#define TRAYCOIN_TASKBAR_H

#include "types.h"

void taskbar_init(void);
void taskbar_shutdown(void);

/* Called from poller background thread (COM initialized) to update taskbar geometry. */
void taskbar_uia_scan(void);
void taskbar_uia_cleanup(void);

/* Called from UI thread */
void taskbar_read_theme(void);
void taskbar_refresh(int force);
void taskbar_rotate_tick(void);
void taskbar_recreate_all(void);
void taskbar_show_menu(void);
HICON taskbar_get_app_icon(void);

#endif /* TRAYCOIN_TASKBAR_H */
