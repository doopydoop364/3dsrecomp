#pragma once
void settings_load(void);
const char *settings_get(const char *key, const char *def);   /* key as "section.name" */
int settings_int(const char *key, int def);
