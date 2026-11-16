#pragma once
#include <stdint.h>
void settings_load(uint64_t title_id);            /* games/<id>.toml, then settings.toml */
void settings_set(const char *key, const char *value);   /* command-line overrides (call after settings_load) */
const char *settings_get(const char *key, const char *def);   /* key as "section.name" */
int settings_int(const char *key, int def);
void settings_each(const char *section, void (*fn)(const char *name, const char *value));
int settings_save(const char *key, const char *value);   /* edit one value in settings.toml, keeping the rest */
