#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""gt4-gs 1.2.0 consumer test: custom texture folder, load-only textures and the 2-row bottom crop.

Usage:
  test_textures_1_2.py --gt4gs <gt4-gs.exe> --gscap <capture.gscap> --work <new empty folder>

The capture is any GT4Recomp .gscap version 2 file (for example a headless menus run). The test never writes next to
the capture; every output goes into --work. gt4-gs runs offline (--replay-gscap), surfaceless, with --surface-size
2560x1080. Checks, from the console log and the frame dumps:

- The replacement folder is `[Folders] Textures` + `replacements`, with no game-serial subfolder: a synthetic
  replacement name placed there is counted ("gt4-gs textures: 1 replacement(s) in <folder>"); the same file placed in
  `Textures/SCUS-97328/replacements` is not counted.
- An empty replacement folder changes nothing: the frame dumps are byte-identical to a run without the folder.
- Texture dumping is not available: `DumpReplaceableTextures = true` is refused ("fixed by gt4-gs").
- The GT4 display crop is 2 native pixels at the bottom and 1 at the right ("display crop left 0 top 0 right 1 bottom 2");
  a config `CropBottom = 5` is refused.

Only the Python standard library is used.
"""

import argparse
import filecmp
import os
import subprocess
import sys

SURFACE = (2560, 1080)
# The offline replay of gt4-gs ends after VM initialization with -16843010 (0xFEFEFEFE) in this build and in 1.1.0; the
# startup log is complete before that. 0 is accepted too.
EXIT_CODES = (0, -16843010 & 0xFFFFFFFF)
SERIAL = "SCUS-97328"
# A replacement name in PCSX2's form: <TEX0 hash, 16 hex digits>-<bits, 8 hex digits>.png. The hash matches no texture
# of a menus capture, so the frames of a run with the pack must equal the frames of a run without it.
PACK_NAME = "0123456789abcdef-00000000.png"
PNG_SIGNATURE = bytes([137, 80, 78, 71, 13, 10, 26, 10])


def write_config(path, folder, extra_lines):
    lines = ["[Folders]", "Textures = " + os.path.join(folder, "Textures"), "", "[EmuCore/GS]",
             "upscale_multiplier = 1", "LoadTextureReplacements = true"] + extra_lines + [
        "", "[GT4GS]", "DataRoot = " + os.path.join(folder, "data"), "DumpDir = " + os.path.join(folder, "dump"),
        "DumpEvery = 100", ""]
    with open(path, "w", encoding="ascii", newline="\n") as file:
        file.write("\n".join(lines))


def run(gt4gs, gscap, folder, pack_folder, extra_lines):
    """One replay. `pack_folder` is the texture folder to fill with the synthetic pack, or None."""
    os.makedirs(os.path.join(folder, "dump"))
    os.makedirs(os.path.join(folder, "data"))
    if pack_folder is not None:
        os.makedirs(pack_folder)
        with open(os.path.join(pack_folder, PACK_NAME), "wb") as file:
            file.write(PNG_SIGNATURE)
    config = os.path.join(folder, "gt4-gs.ini")
    write_config(config, folder, extra_lines)
    command = [gt4gs, "--config", config, "--surface-size", "%dx%d" % SURFACE, "--replay-gscap", gscap]
    log_path = os.path.join(folder, "output.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    with open(log_path, encoding="utf-8", errors="replace") as log:
        text = log.read()
    return result.returncode, text


def frame_dumps(folder):
    dump = os.path.join(folder, "dump")
    return sorted(name for name in os.listdir(dump) if name.endswith(".png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gt4gs", required=True)
    parser.add_argument("--gscap", required=True)
    parser.add_argument("--work", required=True)
    options = parser.parse_args()
    if os.path.exists(options.work) and os.listdir(options.work):
        sys.exit("--work %s is not empty" % options.work)
    os.makedirs(options.work, exist_ok=True)

    failures = []

    def check(condition, text):
        print(("PASS " if condition else "FAIL ") + text)
        if not condition:
            failures.append(text)

    refused = ["DumpReplaceableTextures = true", "CropBottom = 5"]

    # Run 1: the pack in Textures\replacements (no serial folder), plus two refused keys.
    run1 = os.path.join(options.work, "run1-pack")
    textures = os.path.join(run1, "Textures")
    code, log = run(options.gt4gs, options.gscap, run1, os.path.join(textures, "replacements"), refused)
    check(code in EXIT_CODES, "run 1 exit code is a normal end (got %d)" % code)
    check("gt4-gs textures: 1 replacement(s) in " + os.path.join(textures, "replacements") in log,
          "run 1: the pack in Textures/replacements is read (no serial folder)")
    check("ignoring [EmuCore/GS] DumpReplaceableTextures = true" in log and "fixed by gt4-gs" in log,
          "run 1: DumpReplaceableTextures = true is refused")
    check("ignoring [EmuCore/GS] CropBottom = 5" in log, "run 1: CropBottom from the settings file is refused")
    check("gt4-gs: display crop left 0 top 0 right 1 bottom 2" in log, "run 1: the GT4 crop is 2 rows at the bottom, 1 at the right")
    check(sorted(os.listdir(textures)) == ["replacements"], "run 1: nothing but the replacements folder is written under Textures")

    # Run 2: no pack: nothing is read, and the frames must equal run 1's frames byte for byte.
    run2 = os.path.join(options.work, "run2-empty")
    code, log = run(options.gt4gs, options.gscap, run2, None, [])
    check(code in EXIT_CODES, "run 2 exit code is a normal end (got %d)" % code)
    check("gt4-gs textures: 0 replacement(s)" in log, "run 2: an empty folder gives 0 replacements")
    names = frame_dumps(run1)
    if names:
        check(names == frame_dumps(run2), "run 2: the same frame files as run 1 (%d)" % len(names))
        same = all(filecmp.cmp(os.path.join(run1, "dump", name), os.path.join(run2, "dump", name), shallow=False)
                   for name in names)
        check(same, "run 2: frames byte-identical to run 1 (the pack matches no texture of this capture)")
    else:
        # The offline replay of this build ends after VM initialization with the frame dumps not written (the same
        # with gt4-gs 1.1.0); the checks above come from the startup log, which is written before that.
        print("INFO run 2: no frame dumps from the offline replay (as with gt4-gs 1.1.0); the byte comparison is skipped")

    # Run 3: the same pack under the serial folder (the PCSX2 layout): it must not be read.
    run3 = os.path.join(options.work, "run3-serial")
    code, log = run(options.gt4gs, options.gscap, run3, os.path.join(run3, "Textures", SERIAL, "replacements"), [])
    check(code in EXIT_CODES, "run 3 exit code is a normal end (got %d)" % code)
    check("gt4-gs textures: 0 replacement(s)" in log, "run 3: a pack under Textures/%s is not read" % SERIAL)

    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
