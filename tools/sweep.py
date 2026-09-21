"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at two positions against the test card, and report
any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**Screen angles repeat.** A lattice at 0 degrees is the lattice at 90 and at
180, so an angle swept from one end of its range to the other renders the same
picture twice and reads as dead. The angles are swept to 45 degrees.

**A registration offset of a whole number of cells is invisible on a flat
field**, and nearly so on the card. The offsets are swept to a few pixels.

**Total Ink only bites when a colour carries more than the limit**, and with
full black generation the card's colours never reach 300%. It is swept with
Black Generation at zero.

**Wander Speed changes nothing at zero wander**, and a slow wander changes
little in one frame. Both wander controls are swept with the press moving
and thirty frames in.

**Every name must be unique.** `--set` finds a parameter by name and takes
the first match.

**A dropdown holds its element VALUE.** `rztest --list` prints an option's
real range for exactly this reason.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "rztest")
SCRATCH = tempfile.mkdtemp(prefix="rzsweep")

WIDTH, HEIGHT = 640, 360
FRAMES = 1

# Parameters that cannot or must not be swept, with the reason.
SKIP = {
    "Audio": "the FFT buffer; its float value is meaningless. The harness feeds "
             "every render a spectrum, and Audio Drive is the sweepable proof "
             "that it reaches the press",
}

MOVING = {"Press Wander": 0.5, "_frames": 30}

CONTEXT = {
    "Total Ink": {"Black Generation": 0},
    "Angle C": {"_high": 0.25},
    "Angle M": {"_high": 0.25},
    "Angle Y": {"_high": 0.25},
    "Angle K": {"_high": 0.25},
    "Register C X": {"_low": 0.5, "_high": 0.6},
    "Register C Y": {"_low": 0.5, "_high": 0.6},
    "Register M X": {"_low": 0.5, "_high": 0.6},
    "Register M Y": {"_low": 0.5, "_high": 0.6},
    "Register Y X": {"_low": 0.5, "_high": 0.6},
    "Register Y Y": {"_low": 0.5, "_high": 0.6},
    "Register K X": {"_low": 0.5, "_high": 0.6},
    "Register K Y": {"_low": 0.5, "_high": 0.6},
    "Press Wander": {"_frames": 30},
    "Wander Speed": MOVING,
    # The harness feeds a beat every half second; thirty frames is one beat
    # and its decay.
    "Audio Drive": {"_frames": 30},
    # The sweep's positions only ever reach preset 1 at the low end when the
    # range is 0..6 -- Custom against Drifting Press, which moves the plates.
    "Preset": {"_frames": 30},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides, frames):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    merged = {k: v for k, v in overrides.items() if not k.startswith("_")}
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run that is cut off by a CI
    # timeout still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    BIN = args.binary
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("text", "buffer") or name in SKIP:
            skipped.append((name, SKIP.get(name, kind)))
            continue
        context = CONTEXT.get(name, {})
        work.append((pid, name, low, high, context))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
