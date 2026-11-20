#ifndef TRAYCOIN_CONFIG_H
#define TRAYCOIN_CONFIG_H

#include "types.h"

void config_get_path(wchar_t *outPath, int cap);
void config_init_defaults(AppConfig *cfg);
int  config_load(AppConfig *cfg);
int  config_save(const AppConfig *cfg);
int  config_load_from(AppConfig *cfg, const wchar_t *path);
int  config_save_to(const AppConfig *cfg, const wchar_t *path);

#endif /* TRAYCOIN_CONFIG_H */
