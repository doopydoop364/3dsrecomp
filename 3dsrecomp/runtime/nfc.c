/* nfc:u / nfc:m — amiibo. Behaviour follows the Azahar emulator's NFC service (command table, device states,
 * events, the 3-second automatic removal, and its "no keys" path).
 *
 * amiibo dumps (540-byte NTAG215 .bin files, as read from a figure) go in <game folder>/amiibo/. F5 places the
 * next one on the reader while the game is scanning, F6 takes it away. The game sees the tag's serial number and
 * figure ID, which a dump stores unencrypted, so no console keys are needed. Like Azahar without keys, the
 * figure's encrypted save area is not available (games get "needs creating" and skip it) and nothing is written
 * back to the file. RB_AMIIBO="<frame>:<file>" places a tag at a fixed frame, for tests. */
#define _GNU_SOURCE
#include "host.h"
#include <dirent.h>
#include <time.h>

uint64_t gpu_frame(void);

#define NFC_RES(desc, summary) ((uint32_t)(desc) | 93u << 10 | (uint32_t)(summary) << 21 | 25u << 27)
#define R_INVALID_OP   NFC_RES(512, 5)
#define R_INVALID_ARG  NFC_RES(80, 7)
#define R_NOT_SUPPORT  NFC_RES(522, 5)
#define R_NEED_CREATE  NFC_RES(544, 5)

enum { D_NOT_INIT, D_INIT, D_SEARCHING, D_FOUND, D_REMOVED, D_MOUNTED, D_PARTIAL };
enum { C_IDLE, C_SEARCHING_ADAPTER, C_INITIALIZED, C_ACTIVE };

static int mode, dstate, cstate;
static KObject *ev_in, *ev_out;
static uint8_t tag[540];
static uint64_t last_touch_ns;                 /* removal timer: 3 s after the game last looked at the tag */
static int ticking;
static char files[64][256]; static int nfiles, next_file;
static char game_amiibo_dir[300];

static void reply(uint32_t *cmd, int normal, int translate) {
    cmd[0] = (cmd[0] & 0xffff0000u) | ((uint32_t)normal << 6) | (uint32_t)translate;
    cmd[1] = RES_OK;
}
static int mounted(void) { return dstate == D_MOUNTED || dstate == D_PARTIAL; }
static void touch_tag(void) { last_touch_ns = now_ns(); }

static void close_tag(void) {
    if (dstate == D_NOT_INIT || dstate == D_INIT || dstate == D_SEARCHING || dstate == D_REMOVED) return;
    dstate = D_REMOVED;
    memset(tag, 0, sizeof tag);
    if (ev_in) ev_in->signaled = 0;
    if (ev_out) k_signal(ev_out);
    LOG(1, "nfc: amiibo removed");
}
static int load_tag(const char *path) {
    if (dstate != D_SEARCHING) { LOG(1, "nfc: the game is not scanning for an amiibo right now"); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { LOG(1, "nfc: cannot open %s", path); return 0; }
    size_t n = fread(tag, 1, sizeof tag, f); fclose(f);
    if (n < 0x60) { LOG(1, "nfc: %s is too short to be an amiibo dump", path); return 0; }
    dstate = D_FOUND;
    if (ev_out) ev_out->signaled = 0;
    if (ev_in) k_signal(ev_in);
    touch_tag();
    LOG(1, "nfc: amiibo %s placed (figure %02x%02x%02x%02x-%02x%02x%02x%02x)", path,
        tag[0x54], tag[0x55], tag[0x56], tag[0x57], tag[0x58], tag[0x59], tag[0x5a], tag[0x5b]);
    return 1;
}

static void scan_dir(void) {
    nfiles = 0;
    DIR *d = opendir(game_amiibo_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nfiles < 64) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".bin")) snprintf(files[nfiles++], sizeof files[0], "%s/%s", game_amiibo_dir, e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nfiles; i++) for (int j = i + 1; j < nfiles; j++)
        if (strcmp(files[i], files[j]) > 0) { char t[256]; memcpy(t, files[i], 256); memcpy(files[i], files[j], 256); memcpy(files[j], t, 256); }
}
/* Host hotkeys (video.c). */
void nfc_host_tap(void) {
    scan_dir();
    if (!nfiles) { fprintf(stderr, "amiibo: put amiibo .bin dumps in %s\n", game_amiibo_dir); return; }
    const char *p = files[next_file++ % nfiles];
    if (load_tag(p)) fprintf(stderr, "amiibo: tapped %s\n", p);
    else fprintf(stderr, "amiibo: %s not tapped (the game is not asking for an amiibo)\n", p);
}
void nfc_host_remove(void) { close_tag(); }

static void tick(void) {
    static long long at = -2; static char path[256];
    if (at == -2) { const char *e = getenv("RB_AMIIBO"); at = -1; if (e && sscanf(e, "%lld:%255s", &at, path) != 2) at = -1; }
    if (at >= 0 && (long long)gpu_frame() >= at && dstate == D_SEARCHING) { load_tag(path); at = -1; }
    if ((dstate == D_FOUND || mounted()) && now_ns() - last_touch_ns > 3000000000ull) close_tag();
}

void nfc_set_dir(const char *game_dir) { snprintf(game_amiibo_dir, sizeof game_amiibo_dir, "%s/amiibo", game_dir); }

void nfc_service(KObject *s, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16, a = cmd[1];
    if (!ev_in) { ev_in = k_event(0); ev_out = k_event(0); }
    if (!ticking) { ticking = 1; k_add_periodic(tick, 50000000, "nfc"); }
    switch (id) {
    case 0x01:                                                          /* Initialize(mode) */
        reply(cmd, 1, 0);
        if (mode) { cmd[1] = R_INVALID_OP; break; }
        a &= 0xff;
        if (a == 1 || a == 2) { dstate = D_INIT; memset(tag, 0, sizeof tag); mode = (int)a; }
        else if (a == 3) mode = 3;
        else cmd[1] = R_INVALID_ARG;
        break;
    case 0x02:                                                          /* Finalize(mode) */
        reply(cmd, 1, 0);
        if ((int)(a & 0xff) != mode) { cmd[1] = R_INVALID_OP; break; }
        if (mounted() || dstate == D_FOUND) close_tag();
        dstate = D_NOT_INIT; cstate = C_IDLE; mode = 0;
        break;
    case 0x03:                                                          /* Connect */
        reply(cmd, 1, 0);
        if (dstate != D_INIT || cstate != C_IDLE) cmd[1] = R_INVALID_OP; else cstate = C_INITIALIZED;
        break;
    case 0x04:                                                          /* Disconnect */
        reply(cmd, 1, 0);
        if (cstate == C_IDLE) { cmd[1] = R_INVALID_OP; break; }
        if (mounted()) dstate = D_FOUND;
        dstate = D_INIT; cstate = C_IDLE;
        break;
    case 0x05:                                                          /* StartDetection */
        reply(cmd, 1, 0);
        if (mode != 1 && mode != 2) { cmd[1] = R_INVALID_ARG; break; }
        if (dstate != D_INIT && dstate != D_REMOVED && cstate != C_INITIALIZED) { cmd[1] = R_INVALID_OP; break; }
        if (cstate == C_IDLE) cstate = C_INITIALIZED;
        dstate = D_SEARCHING;
        LOG(1, "nfc: the game is scanning for an amiibo (F5 places one from %s)", game_amiibo_dir);
        break;
    case 0x06:                                                          /* StopDetection */
        reply(cmd, 1, 0);
        if (mounted()) dstate = D_FOUND;
        if (dstate == D_FOUND) close_tag();
        if (dstate == D_FOUND || dstate == D_SEARCHING || dstate == D_REMOVED) dstate = D_INIT;
        else cmd[1] = R_INVALID_OP;
        break;
    case 0x07: case 0x1a:                                               /* Mount, MountRom */
        reply(cmd, 1, 0); touch_tag();
        if (dstate != D_FOUND) { cmd[1] = R_INVALID_OP; break; }
        if (tag[0x5b] != 2) { cmd[1] = R_NOT_SUPPORT; break; }          /* tag type must be NTAG (type 2) */
        dstate = id == 0x07 ? D_MOUNTED : D_PARTIAL;
        break;
    case 0x08:                                                          /* Unmount */
        reply(cmd, 1, 0);
        if (mounted()) dstate = D_FOUND; else cmd[1] = R_INVALID_OP;
        break;
    case 0x09: reply(cmd, 1, 0); if (dstate != D_MOUNTED) cmd[1] = R_INVALID_OP; break;   /* Flush: write-protected, nothing written */
    case 0x0b: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(ev_in); break;              /* GetActivateEvent */
    case 0x0c: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(ev_out); break;             /* GetDeactivateEvent */
    case 0x0d: reply(cmd, 2, 0); cmd[2] = (uint32_t)dstate; break;                       /* GetStatus */
    case 0x0f: reply(cmd, 2, 0); cmd[2] = (uint32_t)cstate; break;                       /* GetTargetConnectionStatus */
    case 0x10: {                                                        /* GetTagInfo2: 0x60 bytes */
        touch_tag(); reply(cmd, 25, 0);
        uint8_t *o = (uint8_t *)&cmd[2]; memset(o, 0, 0x60);
        o[0] = 7; o[3] = 2; memcpy(o + 4, tag, 7);
        break;
    }
    case 0x11: {                                                        /* GetTagInfo: 0x2c bytes */
        touch_tag(); reply(cmd, 12, 0);
        if (dstate != D_FOUND && !mounted()) { cmd[1] = R_INVALID_OP; break; }
        uint8_t *o = (uint8_t *)&cmd[2]; memset(o, 0, 0x2c);
        o[0] = 7; o[3] = 2; memcpy(o + 4, tag, 7);
        break;
    }
    case 0x12: reply(cmd, 2, 0); cmd[2] = 0; break;                     /* GetConnectResult */
    case 0x13: reply(cmd, 1, 0); touch_tag(); cmd[1] = dstate == D_MOUNTED ? R_NEED_CREATE : R_INVALID_OP; break;   /* OpenApplicationArea */
    case 0x14: reply(cmd, 1, 0); cmd[1] = dstate == D_MOUNTED ? R_NOT_SUPPORT : R_INVALID_OP; break;              /* CreateApplicationArea */
    case 0x15: { uint32_t sb = staticbufs()[1]; reply(cmd, 1, 2); cmd[1] = R_INVALID_OP; cmd[2] = 0; cmd[3] = sb; break; }   /* ReadApplicationArea */
    case 0x16: reply(cmd, 1, 0); cmd[1] = R_INVALID_OP; break;          /* WriteApplicationArea */
    case 0x17: {                                                        /* GetNfpRegisterInfo: 0xa8 bytes */
        touch_tag(); reply(cmd, 43, 0);
        if (dstate != D_MOUNTED) { cmd[1] = R_INVALID_OP; break; }
        uint8_t *o = (uint8_t *)&cmd[2]; memset(o, 0, 0xa8);
        static const char name[] = "amiibo";                            /* nickname, UTF-16 big-endian */
        for (int i = 0; name[i]; i++) o[0x60 + 2 * i + 1] = (uint8_t)name[i];
        time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
        uint16_t y = (uint16_t)(tm.tm_year + 1900); memcpy(o + 0x78, &y, 2); o[0x7a] = (uint8_t)(tm.tm_mon + 1); o[0x7b] = (uint8_t)tm.tm_mday;
        break;
    }
    case 0x18: {                                                        /* GetNfpCommonInfo: 0x40 bytes */
        touch_tag(); reply(cmd, 17, 0);
        if (dstate != D_MOUNTED) { cmd[1] = R_INVALID_OP; break; }
        uint8_t *o = (uint8_t *)&cmd[2]; memset(o, 0, 0x40);
        time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
        uint16_t y = (uint16_t)(tm.tm_year + 1900); memcpy(o, &y, 2); o[2] = (uint8_t)(tm.tm_mon + 1); o[3] = (uint8_t)tm.tm_mday;
        o[6] = tag[0x54]; o[7] = tag[0x55]; o[8] = tag[0x56];           /* character id, variant */
        o[9] = tag[0x5a];                                               /* series */
        o[10] = tag[0x59]; o[11] = tag[0x58];                           /* model number (stored big-endian) */
        o[12] = tag[0x57];                                              /* figure type */
        o[14] = 0xd8;                                                   /* application area size */
        break;
    }
    case 0x19: reply(cmd, 16, 0); memset(&cmd[2], 0, 0x3c); break;     /* InitializeCreateInfo */
    case 0x1b: {                                                        /* GetIdentificationBlock: 0x36 bytes */
        touch_tag(); reply(cmd, 14, 0);
        if (!mounted()) { cmd[1] = R_INVALID_OP; break; }
        uint8_t *o = (uint8_t *)&cmd[2]; memset(o, 0, 0x36);
        o[0] = tag[0x54]; o[1] = tag[0x55]; o[2] = tag[0x56]; o[3] = tag[0x5a];
        o[4] = tag[0x59]; o[5] = tag[0x58]; o[6] = tag[0x57];
        break;
    }
    default:
        LOG(1, "STUB %-9s cmd %08x  args %08x %08x %08x %08x", s->name, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
        reply(cmd, 1, 0);
    }
    LOG(2, "nfc: cmd %04x -> %08x (device state %d)", id, cmd[1], dstate);
}


/* ---- save states ---- */
void nfc_state(SS *s) {
    ss_tag(s, "nfc");
    SS_V(s, mode); SS_V(s, dstate); SS_V(s, cstate); ss_obj(s, &ev_in); ss_obj(s, &ev_out);
    SS_V(s, tag); SS_V(s, last_touch_ns); SS_V(s, ticking);
    if (ss_loading(s) && ticking) k_periodic_restore("nfc", tick);
}
