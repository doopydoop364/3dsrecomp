#!/usr/bin/env python3
"""Play a game step by step from scripts or agents (the runtime's RB_CONTROL mode, runtime/control.c).

    tools/play.py start <title id> [--name NAME] [--fresh] [-- game options]
        Starts the game headless and uncapped in its own copy work/<id>/live_<NAME>/ (never the folder you play in),
        paused at its first frame. Saves in that copy are kept between sessions; --fresh starts from a console where
        the game never ran. NAME defaults to the title id.
    tools/play.py <NAME> "<command>" ["<command>" ...]
        Sends commands in order and prints each reply. Commands:
          run N            advance N frames (60 per second) with the held input
          press ITEMS [N]  hold ITEMS for N frames (default 6), then release
          hold ITEMS       keep holding ITEMS;  release   let go of everything
          shot PATH [OPTS] advance one frame and save it as a PNG (400x480: top screen above; bottom screen at
                           x 40..359, y 240..479, so a touch at picture (x, y) is touch=(x-40)/(y-240)).
                           OPTS: top | bottom | crop=X,Y,W,H | zoom=K (smaller pictures cost less to look at)
          grid PATH N STEP [OPTS] [cols=C]   N frames STEP apart in one labelled picture
          mash ITEMS N [GAP]   press N times, GAP frames apart (default 30): text boxes
          settle [MAX] [OPTS]  run until the picture stops changing (cutscenes, loading), at most MAX frames
          where            values named in games/<id>.toml [tests.memory] (e.g. the player's position)
          peek ADDR [N]    N bytes of guest memory (hex)
          save PATH        write a save state;  load PATH   load one (the game restarts with it, paused)
          frame            the current frame number;  quit   end the game
        ITEMS: buttons A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT, circle=X/Y and cstick=X/Y (-154..154, up is
        positive), touch=X/Y (bottom screen, 0..319 / 0..239), joined with commas: press A  press UP,circle=0/154 30
    tools/play.py stop <NAME>      end the game;   tools/play.py list   running sessions
The game's log is build/live/<NAME>/game.log. If the game crashes, commands report it with the end of the log.
"""
import os, shutil, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
LIVE = os.path.join(ROOT, "build", "live")
ROBOBOT = "0004000000183600"
READ_ONLY = ("banner.bin", "code.bin", "code_decompressed.bin", "exheader.bin", "icon.bin", "romfs.bin",
             "update_romfs.bin", "dlc", "cartridge", "sysarchives")


def session(name):
    return os.path.join(LIVE, name)


def alive(name):
    try:
        pid = int(open(os.path.join(session(name), "pid")).read())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return 0


def log_tail(name, n=15):
    try:
        return "".join(open(os.path.join(session(name), "game.log"), errors="replace").readlines()[-n:])
    except OSError:
        return ""


def prepare_copy(tid, name, fresh):
    """The game's own folder for this session: read-only data linked, saves kept unless fresh."""
    robobot = tid == ROBOBOT
    src = os.path.join(ROOT, "game") if robobot else os.path.join(ROOT, "work", tid, "game")
    if not os.path.exists(os.path.join(src, "exheader.bin")):
        sys.exit(f"no extracted game in {src} (build it first: ./3dsrecomp build {tid})")
    dst = os.path.join(ROOT, "work", tid, f"live_{name}")
    if fresh and os.path.isdir(dst):
        shutil.rmtree(dst)
    new = not os.path.isdir(dst)
    os.makedirs(dst, exist_ok=True)
    open(os.path.join(dst, "data_in_folder"), "w").close()   # its save data stays in this copy, not the player's folder
    for f in READ_ONLY:
        if os.path.exists(os.path.join(src, f)) and not os.path.lexists(os.path.join(dst, f)):
            os.symlink(os.path.join(src, f), os.path.join(dst, f))
    if robobot and new:                                    # Robobot's test state: the clean save from one boot
        for d in ("save", "extdata", "extdata_shared"):
            p = os.path.join(ROOT, "build", "pristine", d)
            if os.path.isdir(p):
                shutil.copytree(p, os.path.join(dst, d), symlinks=True)
    return dst


def start(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit("usage: tools/play.py start <title id> [--name NAME] [--fresh] [-- game options]")
    tid, rest, name, fresh, extra = argv[0].lower(), argv[1:], None, False, []
    if "--" in rest:
        extra = rest[rest.index("--") + 1:]; rest = rest[:rest.index("--")]
    while rest:
        a = rest.pop(0)
        if a == "--name" and rest: name = rest.pop(0)
        elif a == "--fresh": fresh = True
        else: sys.exit(f"unknown option {a}")
    name = name or tid
    if alive(name):
        sys.exit(f"session {name} is already running (tools/play.py stop {name})")
    binp = os.path.join(ROOT, "build", "robobot") if tid == ROBOBOT else os.path.join(ROOT, "work", tid, "native")
    if not os.access(binp, os.X_OK):
        sys.exit(f"no {binp} (build it first: ./3dsrecomp build {tid})")
    game = prepare_copy(tid, name, fresh)
    sd = session(name)
    os.makedirs(sd, exist_ok=True)
    sock = os.path.join(sd, "sock")
    if os.path.exists(sock):
        os.remove(sock)
    env = dict(os.environ, RB_CONTROL=sock, RB_HEADLESS="1", RB_FAST="1", RB_SETTINGS="/dev/null",
               RB_SYSTEM_DIR=os.path.join(ROOT, "system"))
    for k in ("RB_INPUT", "RB_SHOT", "RB_EXIT_FRAME"):
        env.pop(k, None)
    with open(os.path.join(sd, "game.log"), "w") as log:
        p = subprocess.Popen([binp, game] + extra, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    open(os.path.join(sd, "pid"), "w").write(str(p.pid))
    open(os.path.join(sd, "title"), "w").write(tid)
    for _ in range(1200):                                  # the socket appears at the game's first frame
        if os.path.exists(sock):
            print(f"session {name}: {tid} running, paused at its first frame (game folder {os.path.relpath(game, ROOT)})")
            return
        if p.poll() is not None:
            sys.exit(f"the game exited during start-up (code {p.returncode}):\n{log_tail(name)}")
        time.sleep(0.1)
    sys.exit("the game did not reach its first frame within 2 minutes; see " + os.path.join(sd, "game.log"))


def connect(name, wait=120):
    """A connection to the session's game; waits while it (re)starts, e.g. after loading a state."""
    sock = os.path.join(session(name), "sock")
    t0 = time.time()
    while True:
        if not alive(name):
            sys.exit(f"session {name} is not running.\n{log_tail(name)}")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(sock)
            return s, s.makefile("rw")
        except OSError as e:
            s.close()
            if time.time() - t0 > wait:
                sys.exit(f"cannot reach session {name}: {e}")
            time.sleep(0.1)


class Conn:
    """One connection to a session's game; follows it across restarts (loading a state)."""
    def __init__(self, name):
        self.name = name
        self.s, self.f = connect(name)

    def raw(self, c):
        self.f.write(c.strip() + "\n"); self.f.flush()
        reply = self.f.readline()
        if not reply:
            sys.exit(f"{c}: the game stopped (it may have crashed):\n{log_tail(self.name)}")
        reply = reply.strip()
        if reply.startswith("ok loading"):                 # the game restarts with the state: reconnect when it listens
            self.s.close(); time.sleep(0.3)
            self.s, self.f = connect(self.name)
        return reply

    def frame(self):
        return int(self.raw("frame").split()[-1])


# ---- commands handled here, on top of the game's own (smaller pictures cost an agent less to look at) ----
def _image(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


def _view(img, opts):
    """top | bottom | crop=x,y,w,h | zoom=N, applied to a 400x480 shot."""
    for o in opts:
        if o == "top": img = img.crop((0, 0, 400, 240))
        elif o == "bottom": img = img.crop((40, 240, 360, 480))
        elif o.startswith("crop="):
            x, y, w, h = (int(v) for v in o[5:].split(","))
            img = img.crop((x, y, x + w, y + h))
        elif o.startswith("zoom="):
            k = float(o[5:]); img = img.resize((max(1, int(img.width * k)), max(1, int(img.height * k))))
        else:
            raise ValueError(f"unknown picture option {o}")
    return img


def _tmp(conn, tag):
    return os.path.join(session(conn.name), f"tmp_{tag}.png")


def cmd_shot(conn, args):
    path, opts = args[0], args[1:]
    if not opts:
        return conn.raw(f"shot {path}")
    t = _tmp(conn, "shot"); r = conn.raw(f"shot {t}")
    if not r.startswith("ok"): return r
    img = _view(_image(t), opts); img.save(path)
    return f"{r.replace(t, path)} ({img.width}x{img.height})"


def cmd_grid(conn, args):
    """grid PATH N STEP [picture options] [cols=C]: N frames STEP apart in one picture, each labelled with its frame."""
    from PIL import Image, ImageDraw
    path, n, step = args[0], int(args[1]), int(args[2])
    opts = [o for o in args[3:] if not o.startswith("cols=")]
    cols = next((int(o[5:]) for o in args[3:] if o.startswith("cols=")), min(n, 4))
    tiles = []
    for i in range(n):
        if i: conn.raw(f"run {step}")
        t = _tmp(conn, f"grid{i}"); r = conn.raw(f"shot {t}")
        if not r.startswith("ok"): return r
        tiles.append((_view(_image(t), opts), r.split()[2]))
    w, h = tiles[0][0].width, tiles[0][0].height
    rows = (n + cols - 1) // cols
    g = Image.new("RGB", (cols * w, rows * h), (40, 40, 40)); d = ImageDraw.Draw(g)
    for i, (img, fr) in enumerate(tiles):
        x, y = (i % cols) * w, (i // cols) * h
        g.paste(img, (x, y)); d.text((x + 3, y + 2), fr, fill=(255, 255, 0))
    g.save(path)
    return f"ok frames {tiles[0][1]}..{tiles[-1][1]} grid {path} ({g.width}x{g.height})"


def cmd_mash(conn, args):
    """mash ITEMS N [GAP]: press ITEMS N times, GAP frames apart (default 30): for text boxes."""
    items, n, gap = args[0], int(args[1]), int(args[2]) if len(args) > 2 else 30
    for _ in range(n):
        r = conn.raw(f"press {items}")
        if not r.startswith("ok"): return r
        r = conn.raw(f"run {gap}")
    return r


def cmd_settle(conn, args):
    """settle [MAX] [picture options]: run until the picture stops changing (two equal shots 10 frames apart)."""
    mx = int(args[0]) if args and args[0].isdigit() else 600
    opts = [o for o in args if not o.isdigit()]
    prev, waited = None, 0
    while waited <= mx:
        t = _tmp(conn, "settle"); r = conn.raw(f"shot {t}")
        if not r.startswith("ok"): return r
        cur = _view(_image(t), opts).tobytes()
        if cur == prev:
            return f"ok frame {conn.frame()} settled after {waited} frames"
        prev = cur; conn.raw("run 10"); waited += 11
    return f"ok frame {conn.frame()} still changing after {mx} frames"


TYPES = {"u8": ("<B", 1), "s8": ("<b", 1), "u16": ("<H", 2), "s16": ("<h", 2), "u32": ("<I", 4), "s32": ("<i", 4), "f32": ("<f", 4)}


def _peek(conn, addr, n):
    r = conn.raw(f"peek {addr:08x} {n}")
    if not r.startswith("ok "): raise ValueError(r)
    return bytes.fromhex(r[3:])


def cmd_where(conn, args):
    """where: the values named in games/<id>.toml [tests.memory] (test-only data), e.g.
         player_x = "f32 0x0803c1a4"          a value at a fixed address
         player_y = "f32 *0x0803c1a4+0x28"    a pointer at that address, plus 0x28 (chains: *A+O1*+O2 ...)"""
    import struct, tomllib
    tid = open(os.path.join(session(conn.name), "title")).read().strip()
    toml = os.path.join(ROOT, "games", f"{tid}.toml")
    mem = tomllib.load(open(toml, "rb")).get("tests", {}).get("memory", {}) if os.path.exists(toml) else {}
    if not mem: return f"error games/{tid}.toml has no [tests.memory] section"
    out = []
    for key, spec in mem.items():
        typ, expr = spec.split(None, 1)
        try:
            parts = expr.replace(" ", "").split("+")
            addr = None
            for p_ in parts:
                deref = p_.startswith("*") or p_.endswith("*")
                v = int(p_.strip("*"), 0)
                addr = v if addr is None else addr + v
                if p_.startswith("*"): addr = struct.unpack("<I", _peek(conn, addr, 4))[0]
                if p_.endswith("*"): addr = struct.unpack("<I", _peek(conn, addr, 4))[0]
            fmt, n = TYPES[typ]
            val = struct.unpack(fmt, _peek(conn, addr, n))[0]
            out.append(f"{key}={val:.3f}" if typ == "f32" else f"{key}={val}")
        except Exception as e:
            out.append(f"{key}=? ({e})")
    return "ok " + " ".join(out)


LOCAL = {"shot": cmd_shot, "grid": cmd_grid, "mash": cmd_mash, "settle": cmd_settle, "where": cmd_where}


def send(name, commands):
    conn = Conn(name)
    for c in commands:
        words = c.split()
        if words and words[0] in LOCAL and (words[0] != "shot" or len(words) > 2):
            try:
                reply = LOCAL[words[0]](conn, words[1:])
            except (ValueError, IndexError, KeyError) as e:
                reply = f"error {e}"
        else:
            reply = conn.raw(c)
        print(f"{c}: {reply}", flush=True)
        if reply.startswith("ok bye"):
            break
    conn.s.close()


def stop(name):
    pid = alive(name)
    if not pid:
        print(f"session {name} is not running"); return
    try:
        send(name, ["quit"])
    except SystemExit:
        pass
    for _ in range(50):
        if not alive(name): break
        time.sleep(0.1)
    else:
        os.kill(pid, signal.SIGTERM)
    print(f"session {name} stopped")


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help", "help"):
        print(__doc__.strip()); return
    if a[0] == "start": start(a[1:])
    elif a[0] == "stop" and len(a) == 2: stop(a[1])
    elif a[0] == "list":
        for n in sorted(os.listdir(LIVE)) if os.path.isdir(LIVE) else []:
            if alive(n):
                print(n, open(os.path.join(session(n), "title")).read().strip())
    elif len(a) >= 2: send(a[0], a[1:])
    else: print(__doc__.strip())


if __name__ == "__main__":
    main()
