#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""gt4-gs 1.2.0 consumer test: every enum value the GT4Recomp settings table enables is accepted by gt4-gs.

Usage:
  test_enabled_values_1_2.py --gt4gs <gt4-gs.exe> --gscap <capture.gscap> --table <gs-settings.toml> --work <new empty folder>

For each keyed enum row of the table, each choice with status "ok" is written once to a gt4-gs.ini (one value per key
per run, so the runs go round the choices together). A value gt4-gs refuses is logged as "ignoring [section] key = value";
the test fails on any such line for a value it expected to be accepted. Only the Python standard library and tomllib
(Python 3.11 or later) are used. Offline replays end after VM initialization (as in 1.1.0); the startup log is what is
checked.
"""

import argparse
import os
import subprocess
import sys
import tomllib


def enabled_rounds(table):
    """Lists of (section, key, ini) for each round: the k-th enabled choice of every keyed enum row."""
    per_key = []
    for row in table["setting"]:
        if row.get("type") != "enum" or not row.get("section") or not row.get("key"):
            continue
        ini_values = [choice["ini"] for choice in row["choices"] if choice.get("status") == "ok" and choice.get("ini")]
        if ini_values:
            per_key.append((row["section"], row["key"], ini_values))
    rounds = max(len(values) for _, _, values in per_key)
    result = []
    for index in range(rounds):
        result.append([(section, key, values[index]) for section, key, values in per_key if index < len(values)])
    return result


def write_config(path, folder, pairs):
    lines = ["[Folders]", "", "[EmuCore/GS]"]
    lines += ["%s = %s" % (key, value) for _, key, value in pairs]
    lines += ["", "[GT4GS]", "DataRoot = " + os.path.join(folder, "data"), "DumpDir = " + os.path.join(folder, "dump"),
              "DumpEvery = 100", ""]
    with open(path, "w", encoding="ascii", newline="\n") as file:
        file.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gt4gs", required=True)
    parser.add_argument("--gscap", required=True)
    parser.add_argument("--table", required=True)
    parser.add_argument("--work", required=True)
    options = parser.parse_args()
    if os.path.exists(options.work) and os.listdir(options.work):
        sys.exit("--work %s is not empty" % options.work)
    with open(options.table, "rb") as handle:
        table = tomllib.load(handle)
    rounds = enabled_rounds(table)
    os.makedirs(options.work, exist_ok=True)
    failures = []
    total = 0
    for number, pairs in enumerate(rounds):
        folder = os.path.join(options.work, "round%02d" % number)
        os.makedirs(os.path.join(folder, "dump"))
        os.makedirs(os.path.join(folder, "data"))
        config = os.path.join(folder, "gt4-gs.ini")
        write_config(config, folder, pairs)
        log_path = os.path.join(folder, "output.log")
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            subprocess.run([options.gt4gs, "--config", config, "--replay-gscap", options.gscap], stdout=log,
                           stderr=subprocess.STDOUT, timeout=900)
        with open(log_path, encoding="utf-8", errors="replace") as log:
            text = log.read()
        for section, key, value in pairs:
            total += 1
            if ("ignoring [%s] %s = %s" % (section, key, value)) in text:
                failures.append("[%s] %s = %s refused by gt4-gs" % (section, key, value))
        if "VM subsystems initialized" not in text:
            failures.append("round %d: the startup did not reach VM initialization" % number)
    print("%d values in %d rounds" % (total, len(rounds)))
    for failure in failures:
        print("FAIL " + failure)
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
