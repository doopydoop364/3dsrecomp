/* Input state shared by the display (producer) and the HID service (consumer).
 *
 * RB_INPUT scripts input for tests, by frame number, for example
 *   RB_INPUT="1300-1310:A;1500-1520:A;1600-1700:RIGHT,circle=154/0;1800-1805:touch=160/200"
 * Each entry is FROM-TO:items with items separated by commas: button names (A B X Y L R START SELECT
 * UP DOWN LEFT RIGHT), circle=X/Y, touch=X/Y. While a script is active the host input is ignored. */
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t from, to; InputState st; } ScriptEntry;
static ScriptEntry script[256];
static int nscript, scripted;
static InputState host, current;
static uint64_t cur_frame;

static uint32_t button_bit(const char *n, size_t len) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        { "A", BTN_A }, { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y }, { "L", BTN_L }, { "R", BTN_R },
        { "START", BTN_START }, { "SELECT", BTN_SELECT }, { "UP", BTN_UP }, { "DOWN", BTN_DOWN },
        { "LEFT", BTN_LEFT }, { "RIGHT", BTN_RIGHT } };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i].name) == len && !strncmp(names[i].name, n, len)) return names[i].bit;
    return 0;
}

void input_init(void) {
    const char *s = getenv("RB_INPUT");
    if (!s) return;
    scripted = 1;
    while (*s && nscript < 256) {
        ScriptEntry e; memset(&e, 0, sizeof e);
        char *end;
        e.from = strtoull(s, &end, 10); s = end;
        e.to = e.from;
        if (*s == '-') { e.to = strtoull(s + 1, &end, 10); s = end; }
        if (*s == ':') s++;
        while (*s && *s != ';') {
            size_t n = strcspn(s, ",;");
            if (!strncmp(s, "circle=", 7)) { int x = 0, y = 0; sscanf(s + 7, "%d/%d", &x, &y); e.st.cx = (int16_t)x; e.st.cy = (int16_t)y; }
            else if (!strncmp(s, "touch=", 6)) { int x = 0, y = 0; sscanf(s + 6, "%d/%d", &x, &y); e.st.touch = 1; e.st.tx = (uint16_t)x; e.st.ty = (uint16_t)y; }
            else { uint32_t b = button_bit(s, n); if (!b) fprintf(stderr, "RB_INPUT: unknown item '%.*s'\n", (int)n, s); e.st.buttons |= b; }
            s += n;
            if (*s == ',') s++;
        }
        if (*s == ';') s++;
        script[nscript++] = e;
    }
    fprintf(stderr, "input: %d scripted entries\n", nscript);
}

void input_set_host(const InputState *s) { host = *s; }

void input_frame(uint64_t frame) {
    cur_frame = frame;
    if (!scripted) { current = host; return; }
    memset(&current, 0, sizeof current);
    for (int i = 0; i < nscript; i++)
        if (frame >= script[i].from && frame <= script[i].to) {
            current.buttons |= script[i].st.buttons;
            if (script[i].st.cx || script[i].st.cy) { current.cx = script[i].st.cx; current.cy = script[i].st.cy; }
            if (script[i].st.touch) { current.touch = 1; current.tx = script[i].st.tx; current.ty = script[i].st.ty; }
        }
}

void input_get(InputState *out) { *out = current; }
