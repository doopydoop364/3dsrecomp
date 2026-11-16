#define _GNU_SOURCE
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
/* Writes one value into the user's settings.toml, keeping every other line (and comments): replaces the key in its
 * section, adds it under the section header, or appends the section. Also applies it to the running settings. */
int settings_save(const char *key, const char *value) {
    const char *dot = strchr(key, '.');
    if (!dot) return -1;
    char sec[64], name[64], line[300];
    snprintf(sec, sizeof sec, "%.*s", (int)(dot - key), key); snprintf(name, sizeof name, "%s", dot + 1);
    int quote = !(!strcmp(value, "true") || !strcmp(value, "false") || (value[0] && strspn(value, "0123456789.-") == strlen(value)));
    char newl[200]; snprintf(newl, sizeof newl, quote ? "%s = \"%s\"\n" : "%s = %s\n", name, value);
    const char *path = getenv("RB_SETTINGS"); if (!path) path = "settings.toml";
    char *out = 0; size_t outn = 0; FILE *m = open_memstream(&out, &outn);
    FILE *f = fopen(path, "r");
    char cur[64] = ""; int done = 0, saw_sec = 0;
    while (f && fgets(line, sizeof line, f)) {
        char tmp[300]; snprintf(tmp, sizeof tmp, "%s", line); char *p = trim(tmp);
        if (*p == '[') {
            if (!done && saw_sec) { fputs(newl, m); done = 1; }         /* key was missing from its section */
            char *e = strchr(p, ']'); if (e) *e = 0; snprintf(cur, sizeof cur, "%s", trim(p + 1));
            if (!strcmp(cur, sec)) saw_sec = 1;
            fputs(line, m); continue;
        }
        char *eq = strchr(p, '=');
        if (!done && eq && *p != '#' && !strcmp(cur, sec)) { *eq = 0; if (!strcmp(trim(p), name)) { fputs(newl, m); done = 1; continue; } }
        fputs(line, m);
    }
    if (f) fclose(f);
    if (!done) { if (!saw_sec) { fflush(m); fprintf(m, outn ? "\n[%s]\n" : "[%s]\n", sec); } fputs(newl, m); }
    fclose(m);
    f = fopen(path, "w");
    if (!f) { free(out); return -1; }
    fwrite(out, 1, outn, f); fclose(f); free(out);
    settings_set(key, value);
    return 0;
}
