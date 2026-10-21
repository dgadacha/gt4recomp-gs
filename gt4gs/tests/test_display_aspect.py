#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""gt4-gs 1.1.0 consumer test: protocol 1.3 CONTROL display_aspect, the Stretch setting and the fixed GT4 crop.

Usage:
  test_display_aspect.py --gt4gs <gt4-gs.exe> --gscap <capture.gscap> --work <new empty folder>

The capture is any GT4Recomp .gscap version 2 file with at least 1,300 VSYNCs and a picture from frame 400 on (for
example a headless menus run of the game with GT4_GS_CAPTURE). The test never writes next to the capture: it writes a
copy with synthetic CONTROL records (gscap_inject_control.py) and every output into --work. gt4-gs runs offline
(--replay-gscap), surfaceless, with --surface-size 2560x1080 (an ultrawide virtual window), so each frame dump has the
size of the presented picture: about 1920x1080 at 16:9 (pillarboxed), 2560x1080 when it fills the window.

Run 1, settings file AspectRatio = 16:9 (and CropRight = 5, which must be refused as a fixed key). Records:
display_aspect 1 after frame 500, an invalid mode 5 after frame 700 (ERROR bad_argument, mode unchanged), mode 0 after
frame 900, mode 1 after frame 1100. Expected dump widths: 400 -> 1920, 600 -> 2560, 800 -> 2560, 1000 -> 1920,
1200 -> 2560.
Run 2, settings file AspectRatio = Stretch (accepted since 1.1.0), same records: every dump is 2560 wide, because mode
0 means the settings-file aspect (the user's default) and that is Stretch here.
Only the Python standard library is used.
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SURFACE = (2560, 1080)
CONTROL_DISPLAY_ASPECT = 7
INJECT = ["500:7:1", "700:7:5", "900:7:0", "1100:7:1"]
LAST_FRAME = 1300
CHECK_FRAMES = (400, 600, 800, 1000, 1200)


def png_size(path):
    with open(path, "rb") as file:
        head = file.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ValueError(f"{path} is not a PNG file")
    return struct.unpack(">II", head[16:24])


def write_config(path, folder, aspect, extra_lines):
    lines = ["[EmuCore/GS]", f"AspectRatio = {aspect}", "upscale_multiplier = 1"] + extra_lines + [
        "[GT4GS]",
        f"DataRoot = {os.path.join(folder, 'data')}",
        f"DumpDir = {os.path.join(folder, 'dump')}",
        "DumpEvery = 100",
    ]
    with open(path, "w", encoding="ascii", newline="\n") as file:
        file.write("\n".join(lines) + "\n")


def run_replay(gt4gs, gscap, folder, aspect, extra_lines):
    os.makedirs(os.path.join(folder, "dump"))
    os.makedirs(os.path.join(folder, "data"))
    config = os.path.join(folder, "gt4-gs.ini")
    write_config(config, folder, aspect, extra_lines)
    command = [gt4gs, "--config", config, "--surface-size", f"{SURFACE[0]}x{SURFACE[1]}", "--replay-gscap", gscap]
    with open(os.path.join(folder, "output.log"), "w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    with open(os.path.join(folder, "output.log"), encoding="utf-8", errors="replace") as log:
        text = log.read()
    widths = {}
    for frame in CHECK_FRAMES:
        dump = os.path.join(folder, "dump", f"replay_frame{frame:05d}.png")
        widths[frame] = png_size(dump) if os.path.exists(dump) else None
    return result.returncode, text, widths


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gt4gs", required=True)
    parser.add_argument("--gscap", required=True)
    parser.add_argument("--work", required=True)
    options = parser.parse_args()
    if os.path.exists(options.work) and os.listdir(options.work):
        sys.exit(f"--work {options.work} is not empty")
    os.makedirs(options.work, exist_ok=True)

    injected = os.path.join(options.work, "display-aspect.gscap")
    inject = [sys.executable, os.path.join(HERE, "gscap_inject_control.py"), options.gscap, injected, "--last", str(LAST_FRAME)]
    for at in INJECT:
        inject += ["--at", at]
    subprocess.run(inject, check=True)

    failures = []

    def check(condition, text):
        print(("PASS " if condition else "FAIL ") + text)
        if not condition:
            failures.append(text)

    # Run 1: settings aspect 16:9, CONTROL display_aspect switches it.
    code, log, sizes = run_replay(options.gt4gs, injected, os.path.join(options.work, "run1-16x9"), "16:9", ["CropRight = 5"])
    check(code == 0, f"run 1 exit code 0 (got {code})")
    check("ignoring [EmuCore/GS] CropRight = 5" in log and "fixed by gt4-gs" in log, "run 1: CropRight from the settings file is refused (fixed key)")
    check("ignoring [EmuCore/GS] AspectRatio" not in log, "run 1: AspectRatio 16:9 accepted")
    check(log.count("CONTROL display_aspect 1 (fill)") == 2, "run 1: two display_aspect 1 records applied")
    check(log.count("CONTROL display_aspect 0 (game aspect)") == 1, "run 1: one display_aspect 0 record applied")
    check("CONTROL display_aspect mode 5 is not 0 (game aspect) or 1 (fill); the mode stays 1" in log,
          "run 1: mode 5 answered with an error, mode unchanged")
    expected = {400: 1920, 600: 2560, 800: 2560, 1000: 1920, 1200: 2560}
    for frame, width in expected.items():
        # 16:9 of a 1080-line window is 1920 wide; the GT4 crop (639 x 447 of 640 x 448) moves it by a pixel or two.
        ok = sizes[frame] is not None and abs(sizes[frame][0] - width) <= 2 and sizes[frame][1] == SURFACE[1]
        check(ok, f"run 1 frame {frame}: dump {sizes[frame]}, expected about {(width, SURFACE[1])}")

    # Run 2: settings aspect Stretch; mode 0 returns to the settings aspect, so every frame fills the window.
    code, log, sizes = run_replay(options.gt4gs, injected, os.path.join(options.work, "run2-stretch"), "Stretch", [])
    check(code == 0, f"run 2 exit code 0 (got {code})")
    check("ignoring [EmuCore/GS] AspectRatio" not in log, "run 2: AspectRatio Stretch accepted")
    for frame in CHECK_FRAMES:
        check(sizes[frame] == SURFACE, f"run 2 frame {frame}: dump {sizes[frame]}, expected {SURFACE}")

    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
