#!/usr/bin/env python3
"""Homebrew test programs, run both in 3dsrecomp and in Azahar (the reference), screens compared.

    tests/homebrew.py [NAME ...] [--frames N] [--seconds S]

NAME is a folder under tests/homebrew/ (our own test programs) or a path under $DEVKITPRO/examples/3ds/ (devkitPro's
examples, e.g. graphics/gpu/simple_tri); default: everything listed in ALL below. Each is built from source with
devkitPro (devkitARM, libctru, citro3d: https://devkitpro.org; the toolchain is installed separately, see README),
recompiled with `3dsrecomp build`, run headless for N frames (default 300) in a test copy, and run in Azahar (Flatpak
org.azahar_emu.Azahar, on its own X display :77) for S seconds (default 12). Both screens are compared:
"same" = no pixel differs by more than 8. Animated programs differ by timing; build/homebrew/<name>.png shows ours
next to Azahar's for a look. Results: build/homebrew/results.txt. Text through the system font differs on purpose:
3dsrecomp uses the console's shared font (system/sysarchives), a fresh Azahar its open-source replacement font.

Results 2026-11-19 (all 16 run): same as Azahar: simple_tri, geoshader, multiple_buf, cubemap, immediate, proctex
(top); nearly: loop_subdivision, mipmap_fog, 2d_shapes; timing only: both_screens, fragment_light, normal_mapping,
lenny, particles, gpusprites; font only: composite_scene. Found and fixed: immediate-mode drawing (0x232 = 15)."""
import os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
OUT = os.path.join(ROOT, "build", "homebrew")
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
ALL = ["graphics/gpu/simple_tri", "graphics/gpu/both_screens", "graphics/gpu/2d_shapes", "graphics/gpu/immediate",
       "graphics/gpu/geoshader", "graphics/gpu/loop_subdivision", "graphics/gpu/proctex", "graphics/gpu/fragment_light",
       "graphics/gpu/mipmap_fog", "graphics/gpu/cubemap", "graphics/gpu/normal_mapping", "graphics/gpu/particles",
       "graphics/gpu/gpusprites", "graphics/gpu/multiple_buf", "graphics/gpu/composite_scene", "graphics/gpu/lenny"]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, **kw)


def build_3dsx(name):
    """Copy the program's source to build/homebrew/src/<name> and build it with devkitPro; returns the .3dsx."""
    own = os.path.join(ROOT, "tests", "homebrew", name)
    src = own if os.path.isdir(own) else os.path.join(DKP, "examples", "3ds", name)
    if not os.path.isdir(src):
        sys.exit(f"{name}: no tests/homebrew/{name} and no {src}")
    dst = os.path.join(OUT, "src", name.replace("/", "_"))
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    env = dict(os.environ, DEVKITPRO=DKP, DEVKITARM=os.path.join(DKP, "devkitARM"),
               PATH=f"{DKP}/tools/bin:{DKP}/devkitARM/bin:" + os.environ["PATH"])
    r = sh(["make", "-j8"], cwd=dst, env=env)
    out = [f for f in os.listdir(dst) if f.endswith(".3dsx")]
    if r.returncode or not out:
        sys.exit(f"{name}: building failed:\n{r.stdout[-1500:]}{r.stderr[-1500:]}")
    return os.path.join(dst, out[0])


def ours(path, frames):
    """3dsrecomp: build, then run in its test copy until no missing code is left; returns the 400x480 shot."""
    r = sh([os.path.join(ROOT, "3dsrecomp"), "build", path], cwd=ROOT)
    if r.returncode:
        return None, "build failed: " + (r.stdout + r.stderr)[-800:]
    tid = sh([sys.executable, "-c", f"import sys; sys.path.insert(0, 'extract'); import extract; print('%016x' % extract.homebrew_tid({path!r}))"], cwd=ROOT).stdout.strip()
    r = sh(["sh", "tests/converge.sh", tid, "", str(frames), str(frames)], cwd=ROOT)
    shot = os.path.join(ROOT, "build", f"shot_{frames:06d}.png")
    log = open(os.path.join(ROOT, "build", "play.log"), errors="replace").read()
    fatal = [l for l in log.splitlines() if l.startswith("FATAL")]
    if not os.path.exists(shot):
        return None, (fatal[0] if fatal else "no screenshot: " + r.stdout[-300:])
    return shot, (fatal[0] if fatal else "")


def azahar(path, seconds):
    """Azahar on display :77; returns its window's picture (top screen centred above the bottom screen)."""
    from Xlib import display
    if sh("pgrep -x Xvfb").returncode:
        subprocess.Popen(["Xvfb", ":77", "-screen", "0", "1280x1024x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)
    os.makedirs(os.path.join(ROOT, "build", "azahar"), exist_ok=True)
    local = os.path.join(ROOT, "build", "azahar", os.path.basename(path))   # the Flatpak sees $HOME, not /tmp
    shutil.copy(path, local)
    env = dict(os.environ, DISPLAY=":77", WAYLAND_DISPLAY="")
    p = subprocess.Popen(["flatpak", "run", "--socket=x11", "--nosocket=wayland", "--env=QT_QPA_PLATFORM=xcb",
                          "org.azahar_emu.Azahar", local], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(seconds)
    root_png = os.path.join(OUT, "azahar_root.png")
    sh(["import", "-display", ":77", "-window", "root", root_png])
    geo = None
    d = display.Display(":77")
    def walk(w):
        nonlocal geo
        for c in w.query_tree().children:
            n = c.get_wm_name()
            if n and n.startswith("Azahar ") and not geo:
                g = c.get_geometry(); geo = (g.x, g.y, g.width, g.height)
            walk(c)
    walk(d.screen().root)
    for pid in sh("pgrep -x azahar").stdout.split() + [l.split()[0] for l in sh("pgrep -af azahar-launcher").stdout.splitlines() if "pgrep" not in l]:
        sh(["kill", "-9", pid])
    p.wait()
    return root_png, geo


def compare(name, ours_png, az_png, geo):
    from PIL import Image, ImageChops
    import numpy as np
    a = Image.open(ours_png).convert("RGB")
    win = Image.open(az_png).convert("RGB").crop((geo[0], geo[1], geo[0] + geo[2], geo[1] + geo[3]))
    w = geo[2]
    az_top = win.crop(((w - 400) // 2, 0, (w - 400) // 2 + 400, 240))
    az_bot = win.crop(((w - 320) // 2, 240, (w - 320) // 2 + 320, 480))
    res = []
    for label, x, y in (("top", (0, 0, 400, 240), az_top), ("bottom", (40, 240, 360, 480), az_bot)):
        d = np.asarray(ImageChops.difference(a.crop(x), y)).max(axis=2)
        res.append(f"{label} {'same' if (d > 8).sum() == 0 else f'{100.0 * (d > 8).mean():.2f}% differ'}")
    sheet = Image.new("RGB", (800, 480), (30, 30, 30))
    sheet.paste(a, (0, 0)); sheet.paste(az_top, (400, 0)); sheet.paste(az_bot, (440, 240))
    sheet.save(os.path.join(OUT, name.replace("/", "_") + ".png"))
    return ", ".join(res)


def main():
    args, frames, seconds = [], 300, 12
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--frames": frames = int(next(it))
        elif a == "--seconds": seconds = int(next(it))
        else: args.append(a)
    os.makedirs(OUT, exist_ok=True)
    lines = []
    for name in args or ALL:
        path = build_3dsx(name)
        shot, err = ours(path, frames)
        if not shot:
            line = f"{name}: ours FAILED ({err})"
        else:
            keep = os.path.join(OUT, name.replace("/", "_") + "_ours.png"); shutil.copy(shot, keep)
            az, geo = azahar(path, seconds)
            line = f"{name}: " + (compare(name, keep, az, geo) if geo else "Azahar window not found") + (f" ({err})" if err else "")
        print(line, flush=True); lines.append(line)
    open(os.path.join(OUT, "results.txt"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
