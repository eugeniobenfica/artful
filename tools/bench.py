#!/usr/bin/env python3
"""What Artful's frames take, kept to be compared: the baseline of docs/plano-de-correcao.md (0.5).

    python tools/bench.py run  <artful-render> <out-dir> [--render2d <artful-render2d>] [--backend vulkan|software] [--frames n]
                               [--size w h] [--repeat n]
    python tools/bench.py compare <before-dir> <after-dir> [--tolerance 0.05]

`run` draws each sequence below with artful-render --json and writes one file a sequence into out-dir. With --repeat,
each is drawn that many times and the run with the lowest p50 is kept, which is what makes two runs on one machine
comparable. `compare` reads two such directories and names what got slower: the p50 by more than --tolerance (a share:
0.05 is 5%), the p95, p99 and the first frame by more than --tail-tolerance, as a few hundred frames say little of their
slowest; and never by less than --min-ms, which is a timer's noise. It exits 1 when something did.
"""
import argparse
import json
import os
import subprocess
import sys

# The sequences: each a scene of artful-render and what it is drawn with. They are what the plan names: a scene in the
# open, a city behind a wall, figures that bend, what shows through, many lights; and with --render2d, the 2D room of
# artful-render2d (room2d), its JSON as artful-render's.
SEQUENCES = {
    "outdoors": ["--scene", "outdoors", "--hdr", "--temporal", "--shadows", "2048"],
    "city": ["--scene", "city"],
    "cityOccluded": ["--scene", "city", "+occlusion"],
    "crowd": ["--scene", "crowd", "--figures", "500"],
    "layers": ["--scene", "layers"],
    "lights": ["--scene", "lights"],
    "spheres": ["--scene", "spheres"],
}

MEASURES = [("cpuMs", "p50"), ("cpuMs", "p95"), ("cpuMs", "p99"), ("gpuMs", "p50"), ("gpuMs", "p95"), ("gpuMs", "p99"),
            ("frameMs", "p50"), ("frameMs", "p95"), ("frameMs", "p99")]  # (frameMs: artful-render2d's, frame to frame)


def run(args):
    os.makedirs(args.out, exist_ok=True)
    args.render = os.path.abspath(args.render)
    sequences = dict(SEQUENCES)
    if args.render2d:
        sequences["room2d"] = ["@2d"]
    for name, extra in sequences.items():
        env = dict(os.environ)
        argv = [a for a in extra if not a.startswith("+")]
        features = [a[1:] for a in extra if a.startswith("+")]
        if features:
            env["ARTFUL_FEATURES"] = ",".join("+" + f for f in features)
        best = None
        for attempt in range(args.repeat):
            path = os.path.join(args.out, f"{name}.{attempt}.json")
            if extra == ["@2d"]:
                command = [os.path.abspath(args.render2d), "--backend", args.backend, "--frames", str(args.frames), "--width", str(args.size[0]),
                           "--height", str(args.size[1]), "--out", os.path.join(args.out, "room2d.png"), "--json", path]
            else:
                command = [args.render, *argv, "--backend", args.backend, "--frames", str(args.frames),
                           "--size", str(args.size[0]), str(args.size[1]), "--json", path]
            done = subprocess.run(command, env=env, capture_output=True, text=True)
            if done.returncode != 0:
                print(f"{name}: {os.path.basename(command[0])} failed ({done.returncode})\n{done.stderr}", file=sys.stderr)
                return 1
            with open(path, encoding="utf-8") as f:
                result = json.load(f)
            os.remove(path)
            if best is None or result["cpuMs"]["p50"] < best["cpuMs"]["p50"]:
                best = result
        best["features"] = env.get("ARTFUL_FEATURES", "")
        with open(os.path.join(args.out, f"{name}.json"), "w", encoding="utf-8") as f:
            json.dump(best, f, indent=2)
        gpu = f", GPU p50 {best['gpuMs']['p50']:.3f}" if "gpuMs" in best else ""
        print(f"{name:14} p50 {best['cpuMs']['p50']:8.3f} ms, p99 {best['cpuMs']['p99']:8.3f}{gpu}, first {best['firstFrameMs']:.1f} ms")
    return 0


def compare(args):
    slower = 0
    names = sorted(n[:-5] for n in os.listdir(args.before) if n.endswith(".json"))
    for name in names:
        after_path = os.path.join(args.after, name + ".json")
        if not os.path.exists(after_path):
            print(f"{name}: not in {args.after}")
            continue
        with open(os.path.join(args.before, name + ".json"), encoding="utf-8") as f:
            before = json.load(f)
        with open(after_path, encoding="utf-8") as f:
            after = json.load(f)
        if before.get("renderer") != after.get("renderer"):
            print(f"{name}: drawn by another renderer ({before.get('renderer')} / {after.get('renderer')}), not compared")
            continue
        pairs = [(f"{group} {p}", before[group][p], after[group][p]) for group, p in MEASURES if group in before and group in after]
        pairs.append(("first frame", before["firstFrameMs"], after["firstFrameMs"]))
        for label, b, a in pairs:
            if b <= 0:
                continue
            change = a / b - 1
            limit = args.tolerance if label.endswith("p50") else args.tail_tolerance
            mark = ""
            if change > limit and a - b > args.min_ms:
                mark = "  SLOWER"
                slower += 1
            elif change < -limit and b - a > args.min_ms:
                mark = "  faster"
            print(f"{name:14} {label:12} {b:9.3f} -> {a:9.3f} ms  {change * 100:+6.1f}%{mark}")
    print(f"{slower} slower (p50 by more than {args.tolerance * 100:.0f}%, the rest by more than {args.tail_tolerance * 100:.0f}%)")
    return 1 if slower else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run")
    r.add_argument("render")
    r.add_argument("out")
    r.add_argument("--render2d", default=None)
    r.add_argument("--backend", default="vulkan", choices=["vulkan", "software"])
    r.add_argument("--frames", type=int, default=600)
    r.add_argument("--size", type=int, nargs=2, default=[1920, 1080])
    r.add_argument("--repeat", type=int, default=3)
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    c.add_argument("--tolerance", type=float, default=0.05)
    c.add_argument("--tail-tolerance", type=float, default=0.25)
    c.add_argument("--min-ms", type=float, default=0.05)
    args = parser.parse_args()
    return run(args) if args.command == "run" else compare(args)


if __name__ == "__main__":
    sys.exit(main())
