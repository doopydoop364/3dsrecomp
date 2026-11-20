/* Live control: a running game takes commands over a Unix socket, so a script or an agent can play it step by step.
 * RB_CONTROL=<socket path> turns it on. The game pauses at a vertical blank whenever it has no command to carry out,
 * and stays paused while nobody is connected. One command per line, one reply line each ("ok ..." or "error ..."):
 *   run N            advance N frames with the held input; reply "ok frame F"
 *   hold ITEMS       hold input from now on (RB_INPUT items: A,B,UP,circle=X/Y,cstick=X/Y,touch=X/Y); no frames pass
 *   release          let go of everything
 *   press ITEMS [N]  hold ITEMS for N frames (default 6), then release; reply "ok frame F"
 *   shot PATH        advance one frame and write it to PATH (PNG, 400x480: top screen above, bottom screen at x 40..359,
 *                    y 240..479, so a touch at (x, y) in the picture is touch=(x-40)/(y-240))
 *   save PATH        write a save state (state.c) of this moment; no frames pass
 *   load PATH        load a save state: the program restarts with it (same process id) and listens again, paused;
 *                    the reply "ok loading" comes first, then the connection closes
 *   peek ADDR [N]    N bytes (1-256, default 4) of guest memory at ADDR (hex), as hex: "ok 0011aabb"
 *   frame            reply "ok frame F" without advancing
 *   quit             end the game
 * tools/play.py starts games in this mode and sends commands. */
#include "host.h"
#include "input.h"
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void video_shot_next(const char *path);
int input_parse_items(const char *s, InputState *st);

static int lfd = -1, cfd = -1, on = -1;
static uint64_t target, release_at;        /* run until frame target; release the held input at release_at */
static int replying, shot_pending;
static char shot_path[512];
static InputState held;
static char buf[4096]; static size_t blen;

int control_on(void) {
    if (on < 0) on = getenv("RB_CONTROL") != 0;
    return on;
}
int control_input(InputState *out) {
    if (!control_on()) return 0;
    *out = held;
    return 1;
}
static void say(const char *fmt, ...) {
    char line[700]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (cfd >= 0 && send(cfd, line, (size_t)n, MSG_NOSIGNAL) < 0) { close(cfd); cfd = -1; }
}
static void listen_once(void) {
    if (lfd >= 0) return;
    const char *p = getenv("RB_CONTROL");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(p) >= sizeof a.sun_path) fatal("RB_CONTROL: socket path too long");
    strcpy(a.sun_path, p);
    unlink(p);
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 4)) fatal("RB_CONTROL: cannot listen on %s (%s)", p, strerror(errno));
    fprintf(stderr, "control: listening on %s\n", p);
}
/* Next command line from the client; waits for a client and for input. */
static void next_line(char *out, size_t n) {
    for (;;) {
        char *nl = memchr(buf, '\n', blen);
        if (nl) {
            size_t k = (size_t)(nl - buf);
            size_t m = k < n - 1 ? k : n - 1;
            memcpy(out, buf, m); out[m] = 0;
            if (m && out[m - 1] == '\r') out[m - 1] = 0;
            memmove(buf, nl + 1, blen - k - 1); blen -= k + 1;
            return;
        }
        if (cfd < 0) { cfd = accept(lfd, 0, 0); blen = 0; if (cfd < 0) continue; }
        if (blen == sizeof buf) blen = 0;                          /* an overlong line: drop it */
        ssize_t r = read(cfd, buf + blen, sizeof buf - blen);
        if (r <= 0) { close(cfd); cfd = -1; blen = 0; continue; }
        blen += (size_t)r;
    }
}

/* Called at the end of every vertical blank (frame = frames shown so far). */
void control_vblank(uint64_t frame) {
    if (!control_on()) return;
    listen_once();
    if (release_at && frame >= release_at) { memset(&held, 0, sizeof held); release_at = 0; }
    if (frame < target) return;
    if (replying) {
        if (shot_pending) say("ok frame %llu shot %s", (unsigned long long)frame, shot_path);
        else say("ok frame %llu", (unsigned long long)frame);
        replying = shot_pending = 0;
    }
    for (;;) {
        char line[1024], *arg;
        next_line(line, sizeof line);
        char *cmd = line; while (*cmd == ' ') cmd++;
        arg = cmd + strcspn(cmd, " "); if (*arg) *arg++ = 0;
        while (*arg == ' ') arg++;
        if (!strcmp(cmd, "run")) {
            long long n = atoll(arg);
            if (n <= 0) { say("error run needs a frame count"); continue; }
            target = frame + (uint64_t)n; replying = 1; return;
        } else if (!strcmp(cmd, "hold") || !strcmp(cmd, "press")) {
            InputState st; memset(&st, 0, sizeof st);
            char items[1024]; size_t k = strcspn(arg, " ");
            memcpy(items, arg, k); items[k] = 0;
            if (!*items || !input_parse_items(items, &st)) { say("error unknown input '%s'", items); continue; }
            held = st;
            if (!strcmp(cmd, "hold")) { release_at = 0; say("ok frame %llu", (unsigned long long)frame); continue; }
            long long n = arg[k] ? atoll(arg + k) : 6;
            if (n <= 0) n = 6;
            target = release_at = frame + (uint64_t)n; replying = 1; return;
        } else if (!strcmp(cmd, "release")) {
            memset(&held, 0, sizeof held); release_at = 0; say("ok frame %llu", (unsigned long long)frame);
        } else if (!strcmp(cmd, "shot")) {
            if (!*arg) { say("error shot needs a path"); continue; }
            snprintf(shot_path, sizeof shot_path, "%s", arg);
            video_shot_next(shot_path);
            target = frame + 1; replying = shot_pending = 1; return;
        } else if (!strcmp(cmd, "save")) {
            if (!*arg) { say("error save needs a path"); continue; }
            if (state_save(arg)) say("error could not save %s", arg); else say("ok frame %llu saved %s", (unsigned long long)frame, arg);
        } else if (!strcmp(cmd, "load")) {
            if (!*arg || access(arg, R_OK)) { say("error cannot read %s", arg); continue; }
            say("ok loading %s", arg);
            if (cfd >= 0) { close(cfd); cfd = -1; }
            state_load_restart(arg);
            say("error loading failed");
        } else if (!strcmp(cmd, "peek")) {                  /* peek ADDR [N]: N bytes of guest memory, as hex */
            uint32_t a = (uint32_t)strtoul(arg, 0, 16), n = 4; char *sp = strchr(arg, ' ');
            if (sp) n = (uint32_t)strtoul(sp + 1, 0, 0);
            if (n == 0 || n > 256) { say("error peek reads 1 to 256 bytes"); continue; }
            if (!mem_valid(a, n)) { say("error %08x+%u is not mapped", a, n); continue; }
            char hex[520]; for (uint32_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", ((uint8_t *)(uintptr_t)a)[i]);
            say("ok %s", hex);
        } else if (!strcmp(cmd, "frame")) {
            say("ok frame %llu", (unsigned long long)frame);
        } else if (!strcmp(cmd, "quit")) {
            say("ok bye"); fprintf(stderr, "control: quit at frame %llu\n", (unsigned long long)frame); exit(0);
        } else if (*cmd) say("error unknown command '%s'", cmd);
    }
}
