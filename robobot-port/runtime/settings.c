/* settings.toml reader: a small TOML subset (sections, key = "string" | number | true/false, # comments).
 * Looked up as $RB_SETTINGS, else ./settings.toml. Missing file or keys keep the defaults. */
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

void settings_load(void) {
    const char *path = getenv("RB_SETTINGS");
    FILE *f = fopen(path ? path : "settings.toml", "r");
    if (!f) return;
    char line[256], section[64] = "";
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
    fprintf(stderr, "settings: %d values from %s\n", nentries, path ? path : "settings.toml");
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
