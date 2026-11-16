/* Settings: a small TOML subset (sections, key = "string" | number | true/false, # comments).
 * Layers, later ones winning: games/<title id>.toml (per-game defaults shipped with the port), then the user's
 * settings.toml ($RB_SETTINGS, else ./settings.toml), then command-line flags (settings_set). Missing files
 * or keys keep the built-in defaults. */
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char key[64], value[64]; } Entry;
static Entry entries[256];
static int nentries;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], section[64] = "";
    int n0 = nentries;
    while (fgets(line, sizeof line, f) && nentries < 256) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        if (*p == '[') { char *e = strchr(p, ']'); if (e) { *e = 0; snprintf(section, sizeof section, "%s", trim(p + 1)); } continue; }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        if (*v == '"') { v++; char *q = strchr(v, '"'); if (q) *q = 0; }
        else { char *c = strchr(v, '#'); if (c) { *c = 0; v = trim(v); } }
        snprintf(entries[nentries].key, sizeof entries[nentries].key, "%s.%s", section, k);
        snprintf(entries[nentries].value, sizeof entries[nentries].value, "%s", v);
        nentries++;
    }
    fclose(f);
    fprintf(stderr, "settings: %d values from %s\n", nentries - n0, path);
    return 1;
}
void settings_load(uint64_t title_id) {
    char path[300];
    snprintf(path, sizeof path, "games/%016llx.toml", (unsigned long long)title_id);
    load_file(path);
    const char *user = getenv("RB_SETTINGS");
    load_file(user ? user : "settings.toml");
}
void settings_set(const char *key, const char *value) {
    if (nentries == 256) return;
    snprintf(entries[nentries].key, sizeof entries[nentries].key, "%s", key);
    snprintf(entries[nentries].value, sizeof entries[nentries].value, "%s", value);
    nentries++;
}
const char *settings_get(const char *key, const char *def) {
    for (int i = nentries - 1; i >= 0; i--) if (!strcmp(entries[i].key, key)) return entries[i].value;
    return def;
}
int settings_int(const char *key, int def) {
    const char *v = settings_get(key, 0);
    if (!v) return def;
    if (!strcmp(v, "true")) return 1;
    if (!strcmp(v, "false")) return 0;
    return atoi(v);
}
/* Calls fn(name, value) for every key in a section ("hotspots" -> "F7", "160,200"); later layers override earlier. */
void settings_each(const char *section, void (*fn)(const char *name, const char *value)) {
    size_t n = strlen(section);
    for (int i = 0; i < nentries; i++) {
        if (strncmp(entries[i].key, section, n) || entries[i].key[n] != '.') continue;
        int newest = 1;
        for (int j = i + 1; j < nentries; j++) if (!strcmp(entries[j].key, entries[i].key)) newest = 0;
        if (newest) fn(entries[i].key + n + 1, entries[i].value);
    }
}
