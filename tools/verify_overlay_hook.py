#!/usr/bin/env python3
"""Check that the overlay loader hook can actually work.

The hook in patches/required.c hands librecomp three numbers the GAME computes
from its own two tables:

    recomp_load_overlays(file_rom_addr, buf_start, file_size)

librecomp's load_overlays() matches that ROM range against the section table
N64Recomp generated from the decomp ELF. Nothing checks that those two views
agree -- if they drift, load_overlays() simply finds no section, loads nothing,
and the first indirect call into the overlay dies in get_function() with
"Failed to find function at 0x...". There is no earlier warning.

So this asserts the agreement directly, for all 92 overlays:

  1. the file table's ROM address           == the section's rom_addr
  2. the load-address table's `start`       == the section's ram_addr
  3. each file's ROM range covers its own section AND NO OTHER
     (otherwise one load would register a second, non-resident overlay)

It also re-derives which sections librecomp loads at startup, which is what
settles the ".main needs no explicit load" question -- see the README.

Run after anything that changes the decomp's segmentation or a rebuild of
RecompiledFuncs/:

    uv run python tools/verify_overlay_hook.py

Exits nonzero on any disagreement.
"""

import argparse
import bisect
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ROM = os.path.join(REPO, "hybridheaven.z64")
DEFAULT_INL = os.path.join(REPO, "RecompiledFuncs", "recomp_overlays.inl")

# The load-address table: 624 entries of {start, end}, ending exactly where the
# Nisitenma-Ichigo signature begins. See lib/hybridheaven/PLAN.md Phase A.
LOAD_TABLE_ROM = 0x3885C

# librecomp's recomp::init() -- load_overlays(0x1000, entrypoint, 1MB).
STARTUP_ROM = 0x1000
STARTUP_RAM = 0x80000400
STARTUP_SIZE = 1024 * 1024

SECTION_RE = re.compile(
    r"\{\s*\.rom_addr\s*=\s*(0x[0-9A-Fa-f]+),\s*"
    r"\.ram_addr\s*=\s*(0x[0-9A-Fa-f]+),\s*"
    r"\.size\s*=\s*(0x[0-9A-Fa-f]+),\s*"
    r"\.funcs\s*=\s*section_\d+_(\w+?)_funcs,"
)


def parse_sections(inl_path):
    """Pull the code sections out of the generated recomp_overlays.inl."""
    with open(inl_path) as f:
        text = f.read()
    start = text.index("static SectionTableEntry section_table[] = {")
    end = text.index("};", start)
    out = []
    for m in SECTION_RE.finditer(text, start, end):
        out.append(
            dict(
                rom=int(m.group(1), 16),
                ram=int(m.group(2), 16),
                size=int(m.group(3), 16),
                name=m.group(4),
            )
        )
    return out


def startup_window(sections):
    """Which sections does recomp::init() load before the game runs?

    Replicates librecomp's load_overlays() bounds, including that
    init_overlays() sorts the table by rom_addr first.
    """
    rows = sorted(sections, key=lambda s: s["rom"])
    lower = bisect.bisect_left([s["rom"] for s in rows], STARTUP_ROM)

    # upper_bound(.., rom+size, [](addr, e){ return addr < e.size + e.rom_addr; })
    # The range is not strictly partitioned by that predicate, so std's binary
    # search could differ from a linear scan; compute both and report if so.
    value = STARTUP_ROM + STARTUP_SIZE

    def comp(addr, e):
        return addr < e["size"] + e["rom"]

    lo, hi = 0, len(rows)
    while lo < hi:
        mid = (lo + hi) // 2
        if comp(value, rows[mid]):
            hi = mid
        else:
            lo = mid + 1
    upper_bin = lo
    upper_lin = next((i for i, e in enumerate(rows) if comp(value, e)), len(rows))
    return rows, lower, upper_bin, upper_lin


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=DEFAULT_ROM,
                    help="decompressed ROM (default: %(default)s)")
    ap.add_argument("--inl", default=DEFAULT_INL,
                    help="generated recomp_overlays.inl (default: %(default)s)")
    args = ap.parse_args()

    for path, what in ((args.rom, "ROM"), (args.inl, "recomp_overlays.inl")):
        if not os.path.exists(path):
            sys.exit(f"error: {what} not found at {path}")

    rom = open(args.rom, "rb").read()
    sig = rom.find(b"Nisitenma-Ichigo")
    if sig < 0:
        sys.exit("error: no Nisitenma-Ichigo signature -- is this the decompressed ROM?")
    ents_base = sig + 0x10  # the u32 entries follow the 16-byte signature

    def entry(i):
        return struct.unpack_from(">I", rom, ents_base + i * 4)[0]

    def load_addr(i):
        return struct.unpack_from(">II", rom, LOAD_TABLE_ROM + i * 8)

    sections = parse_sections(args.inl)
    by_name = {s["name"]: s for s in sections}
    overlays = sorted((n for n in by_name if n.startswith("file_")),
                      key=lambda n: int(n.split("_")[1]))

    print(f"signature at {sig:#x}, {len(sections)} code sections, "
          f"{len(overlays)} overlays")

    # --- the startup window, i.e. the ".main" question -----------------------
    rows, lower, upper_bin, upper_lin = startup_window(sections)
    print(f"\nrecomp::init() load_overlays({STARTUP_ROM:#x}, "
          f"{STARTUP_RAM:#010x}, {STARTUP_SIZE // 1024}K) loads:")
    loaded_at_startup = []
    for i in range(lower, upper_bin):
        s = rows[i]
        at = s["rom"] - STARTUP_ROM + STARTUP_RAM
        flag = "" if at == s["ram"] else "  <-- NOT at its link address"
        print(f"  .{s['name']:<10s} rom {s['rom']:#08x}..{s['rom'] + s['size']:#08x}"
              f"  ->  {at:#010x}{flag}")
        loaded_at_startup.append(s["name"])

    failures = []
    if upper_bin != upper_lin:
        failures.append(f"startup window ambiguous: binary search gives "
                        f"{upper_bin}, linear scan {upper_lin}")
    if "main" not in loaded_at_startup:
        failures.append(".main is NOT loaded at startup -- indirect calls into "
                        "it will not resolve; see README")

    # --- the hook's agreement with the game's own tables ---------------------
    bad_rom = bad_ram = bad_cover = 0
    by_rom = sorted((s["rom"], s["rom"] + s["size"], s["name"]) for s in sections)

    for name in overlays:
        n = int(name.split("_")[1])  # splat's 0-based index == the table index
        s = by_name[name]
        file_rom = entry(n) & 0x7FFFFFFF
        file_size = (entry(n + 1) & 0x7FFFFFFF) - file_rom
        start, _end = load_addr(n)

        if file_rom != s["rom"]:
            bad_rom += 1
            print(f"  ROM MISMATCH .{name}: file table {file_rom:#010x} "
                  f"vs section {s['rom']:#010x}")
        if start != s["ram"]:
            bad_ram += 1
            print(f"  RAM MISMATCH .{name}: load table {start:#010x} "
                  f"vs section {s['ram']:#010x}")

        covered = [k for (a, b, k) in by_rom
                   if a >= file_rom and b <= file_rom + file_size]
        if covered != [name]:
            bad_cover += 1
            print(f"  COVERAGE .{name}: its rom range registers {covered}")

    print(f"\n{len(overlays)} overlays checked")
    print(f"  rom_addr mismatches                : {bad_rom}")
    print(f"  ram_addr mismatches (overlay moves): {bad_ram}")
    print(f"  files registering != 1 section     : {bad_cover}")

    if bad_rom:
        failures.append(f"{bad_rom} overlays whose ROM address the hook would "
                        f"pass does not match any section")
    if bad_ram:
        failures.append(f"{bad_ram} overlays load somewhere other than their "
                        f"link address (relocation assumptions no longer hold)")
    if bad_cover:
        failures.append(f"{bad_cover} overlays whose ROM range does not register "
                        f"exactly one section")

    if failures:
        print("\nFAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("\nOK -- the loader hook agrees with the recompiler's section table.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
