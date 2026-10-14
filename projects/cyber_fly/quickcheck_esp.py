# SPDX-License-Identifier: Apache-2.0
#
# Single-file compile check for the three Task C sources that the host harness
# CANNOT see, because they live behind #if FLY_LINK_ESP_UDP or pull ESP-IDF
# headers (fly_mic.c -> esp_codec_dev.h, fly_wifi.c -> esp_wifi.h).
#
# Why this exists: on 2026-10-06 a 20-minute IDF build failed twice on one line
# of fly_link.c each time - first `ip_addr_t` has no member `addr`, then
# `ipaddr_aton()` wants an `ip_addr_t*` not a `struct in_addr*`. Neither is
# visible to the host core test. Reusing the exact compile command from
# build/compile_commands.json turns a 20-minute round trip into a 3-second one.
#
# Usage (from the project dir, after at least one successful configure):
#   python quickcheck_esp.py
import json
import os
import shlex
import subprocess
import sys

PROJECT = os.path.dirname(os.path.abspath(__file__))
CCJSON = os.path.join(PROJECT, "build", "compile_commands.json")
TARGETS = ["fly_link.c", "fly_mic.c", "fly_wifi.c", "fly_imu.c"]


def _split_windows(command: str) -> list:
    """Split a command line the way a Windows shell would.

    Double quotes group tokens; a backslash is literal unless it immediately
    precedes a quote. That is exactly what gcc's -D flags with embedded quotes
    and the absolute Windows paths both need.
    """
    tokens = []
    current = []
    in_quotes = False
    index = 0
    length = len(command)
    while index < length:
        char = command[index]
        if char == "\\" and index + 1 < length and command[index + 1] == '"':
            current.append('"')
            index += 2
            continue
        if char == '"':
            in_quotes = not in_quotes
            index += 1
            continue
        if char.isspace() and not in_quotes:
            if current:
                tokens.append("".join(current))
                current = []
            index += 1
            continue
        current.append(char)
        index += 1
    if current:
        tokens.append("".join(current))
    return tokens


def main() -> int:
    if not os.path.isfile(CCJSON):
        print(f"!! {CCJSON} not found - run a configure/build once first")
        return 2
    with open(CCJSON, encoding="utf-8") as handle:
        entries = json.load(handle)

    # The compiler path in compile_commands.json is Windows-style with
    # backslashes; _split_windows keeps those intact, but allow an override
    # for setups where the recorded path is stale.
    override_cc = os.environ.get("MOSAICO_QUICKCHECK_CC")

    by_file = {}
    for entry in entries:
        name = os.path.basename(entry.get("file", ""))
        by_file.setdefault(name, entry)

    missing = [t for t in TARGETS if t not in by_file]
    if missing:
        print("!! not in compile_commands.json: " + ", ".join(missing))
        print("   (CMakeLists.txt SRCS may be stale - reconfigure first)")
        return 2

    failures = 0
    for target in TARGETS:
        entry = by_file[target]
        # Reuse the real command verbatim, but redirect the object output to a
        # scratch file so a passing check never races the real build.
        #
        # Split the command WITHOUT shlex. Neither shlex mode works here:
        #   posix=False keeps the quote characters, so `-DFOO="a b"` reaches gcc
        #     with literal quotes -> `missing terminating " character`, which
        #     then cascades into hundreds of bogus <stdio.h> errors because the
        #     broken -D swallows every later flag;
        #   posix=True  eats the backslashes in Windows paths, so
        #     `D:\ESP\...` becomes `D:ESP...`.
        # MSVC-style manual tokenising handles both: double quotes group, and
        # backslashes are only special immediately before a quote.
        argv = _split_windows(entry["command"])
        if override_cc:
            argv[0] = override_cc
        if not os.path.isfile(argv[0]):
            raise SystemExit(f"compiler not found: {argv[0]!r}")
        out_index = argv.index("-o") + 1 if "-o" in argv else None
        if out_index is not None:
            argv[out_index] = os.path.join(PROJECT, "build", f"quickcheck_{target}.o")
        argv = [a for a in argv if a != "-MD"]
        for flag in ("-MF", "-MT"):
            if flag in argv:
                i = argv.index(flag)
                del argv[i:i + 2]

        proc = subprocess.run(argv, cwd=entry["directory"],
                              capture_output=True, text=True)
        if proc.returncode:
            failures += 1
            print(f"  FAIL {target}")
            sys.stdout.write(proc.stdout or "")
            sys.stderr.write(proc.stderr or "")
        else:
            noise = [l for l in (proc.stderr or "").splitlines()
                     if "warning:" in l or "error:" in l]
            if noise:
                print(f"  WARN {target}")
                for line in noise:
                    print("       " + line)
            else:
                print(f"  ok   {target}")

    print("PASS" if not failures else f"FAILED ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
