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
          shot PATH        advance one frame and save it as a PNG (400x480: top screen above; bottom screen at
                           x 40..359, y 240..479, so a touch at picture (x, y) is touch=(x-40)/(y-240))
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


def send(name, commands):
    sock = os.path.join(session(name), "sock")
    if not alive(name):
        sys.exit(f"session {name} is not running.\n{log_tail(name)}")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock)
    except OSError as e:
        sys.exit(f"cannot reach session {name}: {e}")
    f = s.makefile("rw")
    for c in commands:
        f.write(c.strip() + "\n"); f.flush()
        reply = f.readline()
        if not reply:
            sys.exit(f"{c}: the game stopped (it may have crashed):\n{log_tail(name)}")
        print(f"{c}: {reply.strip()}", flush=True)
        if reply.startswith("ok bye"):
            break
    s.close()


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
