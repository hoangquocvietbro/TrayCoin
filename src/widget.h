#ifndef TRAYCOIN_WIDGET_H
#define TRAYCOIN_WIDGET_H

#include "types.h"

void widget_init(void);
void widget_shutdown(void);
void widget_refresh(int force);
void widget_reset_position(void);

#endif /* TRAYCOIN_WIDGET_H */
