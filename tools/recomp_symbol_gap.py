#!/usr/bin/env python3
"""Report libultra functions the recompiled code calls but the runtime lacks.

N64Recomp does not recompile a function whose name is in its `ignored_funcs` or
`reimplemented_funcs` list (src/symbol_lists.cpp). Where such a function is
CALLED, it emits `<name>_recomp(rdram, ctx)` instead, expecting
N64ModernRuntime to define it. Not every ignored name has an implementation, so
a game can emit a call to a `_recomp` symbol that does not exist. The symptom is
a compile error ("implicit declaration of function '__osSiGetAccess_recomp'")
or, past that, an unresolved symbol at link time.

A `_recomp` call is NOT necessarily a call into the runtime, which is why this
checks the recompiled output's own funcs.h too. N64Recomp's third list,
`renamed_funcs`, covers things like `bzero`, `memcpy` and `sqrtf`: those are
still recompiled, just under a `_recomp` name so they do not collide with the
host libc. They are self-satisfied, and counting them as missing is a false
positive.

That gap is not really a runtime bug -- it is a signal about THIS repo's symbol
table. It appears when a libultra function is still under a `func_XXXXXXXX`
placeholder while the internals it calls are named:

    game code  ->  func_80032FB0_33BB0   (unnamed: recompiled as game code)
                       -> __osSiGetAccess  (named: not recompiled, needs a host impl)

Naming the outer function fixes it at the root, and fixes something worse than a
build error at the same time. An unnamed libultra function is recompiled and
EXECUTED, so the port runs the game's own hardware drivers -- poking SI, VI and
PI registers that librecomp does not emulate -- instead of the runtime's native
implementation. Goemon64Recomp never hits this because it names the public
libultra API, which makes the internals unreachable.

So the fix is almost always "name the caller", not "stub the callee". Only
functions genuinely called from game code (a compiler support routine such as
`__d_to_ull`, say) need a host implementation.

Usage:
    uv run python tools/recomp_symbol_gap.py
    uv run python tools/recomp_symbol_gap.py --verbose   # every caller

Exits nonzero while any gap remains.
"""

import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Linked libultra occupies ROM 0x26F00-0x35D30 in the static segment; see
# lib/hybridheaven/README.md. A caller outside that range is game code, so its
# callee is public API rather than an internal.
LIBULTRA_ROM_START = 0x26F00
LIBULTRA_ROM_END = 0x35D30

CALL_RE = re.compile(r"^\s*(\w+_recomp)\(rdram, ctx\);")
FUNC_RE = re.compile(r"^RECOMP_FUNC void (\w+)\(")
PROVIDED_RE = re.compile(r'extern\s+"C"\s+void\s+(\w+_recomp)\s*\(')
NAME_RE = re.compile(r"^func_[0-9A-Fa-f]{8}_([0-9A-Fa-f]+)$")


def rom_of(func_name):
    """ROM offset encoded in a splat placeholder name, or None if named."""
    m = NAME_RE.match(func_name or "")
    return int(m.group(1), 16) if m else None


def is_libultra(func_name):
    rom = rom_of(func_name)
    return rom is not None and LIBULTRA_ROM_START <= rom < LIBULTRA_ROM_END


def scan_calls(func_dir):
    """{callee_recomp: {caller_func_name, ...}} over the recompiled output."""
    calls = {}
    all_funcs = set()
    for path in sorted(glob.glob(os.path.join(func_dir, "*.c"))):
        cur = None
        for line in open(path):
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(1)
                all_funcs.add(cur)
                continue
            c = CALL_RE.match(line)
            if c:
                calls.setdefault(c.group(1), set()).add(cur)
    return calls, all_funcs


def scan_direct_callers(func_dir, targets):
    """{target: {caller, ...}} for plain recompiled-to-recompiled calls."""
    pats = {t: re.compile(r"\b" + re.escape(t) + r"\(rdram, ctx\)") for t in targets}
    found = {t: set() for t in targets}
    for path in sorted(glob.glob(os.path.join(func_dir, "*.c"))):
        cur = None
        for line in open(path):
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(1)
                continue
            for t, pat in pats.items():
                if cur != t and pat.search(line):
                    found[t].add(cur)
    return found


def scan_provided(runtime_dirs):
    provided = set()
    for d in runtime_dirs:
        for root, _dirs, files in os.walk(d):
            for fn in files:
                if not fn.endswith((".cpp", ".c", ".cc")):
                    continue
                text = open(os.path.join(root, fn), errors="ignore").read()
                provided.update(PROVIDED_RE.findall(text))
    return provided


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--funcs", default=os.path.join(REPO, "RecompiledFuncs"))
    ap.add_argument("--runtime", default=os.path.join(REPO, "lib", "N64ModernRuntime"))
    ap.add_argument("--verbose", action="store_true",
                    help="list every caller, not just the ones to act on")
    args = ap.parse_args()

    if not os.path.isdir(args.funcs):
        sys.exit(f"error: no recompiled output at {args.funcs} -- run N64Recomp first")

    calls, _all_funcs = scan_calls(args.funcs)
    # This project's own src/ counts as "provided" too -- src/game/ultra_missing.cpp
    # exists precisely to define the handful librecomp does not.
    provided = scan_provided([os.path.join(args.runtime, "ultramodern"),
                              os.path.join(args.runtime, "librecomp"),
                              os.path.join(REPO, "src")])

    # renamed_funcs are recompiled under their _recomp name and declared in the
    # output's own funcs.h -- self-satisfied, not a gap.
    self_provided = set()
    funcs_h = os.path.join(args.funcs, "funcs.h")
    if os.path.exists(funcs_h):
        self_provided = set(re.findall(r"^void (\w+_recomp)\(", open(funcs_h).read(),
                                       re.MULTILINE))

    missing = sorted(set(calls) - provided - self_provided)

    print(f"{len(calls)} distinct *_recomp symbols called by the recompiled code")
    print(f"{len(provided)} defined by N64ModernRuntime")
    print(f"{len(self_provided & set(calls))} recompiled under a _recomp name "
          f"(renamed_funcs -- self-satisfied)")
    print(f"{len(missing)} MISSING\n")

    if not missing:
        print("OK -- every _recomp symbol the recompiled code calls exists.")
        return 0

    # Which unnamed functions are responsible, and are they reachable?
    culprits = set()
    for sym in missing:
        culprits |= {c for c in calls[sym] if c}
    reach = scan_direct_callers(args.funcs, culprits)

    to_name, to_implement = [], []
    for sym in missing:
        for c in sorted(x for x in calls[sym] if x):
            if is_libultra(c):
                to_name.append((c, sym))
            else:
                to_implement.append((c, sym))

    print("Unnamed libultra functions to NAME in the decomp's symbol_addrs.txt.")
    print("Naming one stops it being recompiled, which removes its calls to the")
    print("missing symbols AND stops the port running the game's own driver:\n")
    named_set = sorted({c for c, _ in to_name})
    for c in named_set:
        needs = sorted({s for cc, s in to_name if cc == c})
        callers = sorted(reach.get(c, ()))
        game_callers = [x for x in callers if not is_libultra(x)]
        where = (f"called from game code by {len(game_callers)} function(s)"
                 if game_callers else
                 f"reached only from libultra ({len(callers)} caller(s))")
        print(f"  {c}")
        print(f"      needs: {', '.join(needs)}")
        print(f"      {where}")
        if args.verbose and callers:
            for x in callers:
                print(f"        <- {x}{'' if is_libultra(x) else '  [GAME]'}")
    print(f"\n  {len(named_set)} function(s) to name.")

    if to_implement:
        print("\nCalled from GAME code, so naming cannot help -- these need a host")
        print("implementation in src/ (the pattern is librecomp's ultra_stubs.cpp):\n")
        for sym in sorted({s for _, s in to_implement}):
            cs = sorted({c for c, s2 in to_implement if s2 == sym})
            print(f"  {sym}")
            for c in cs:
                print(f"      <- {c}")
        print(f"\n  {len({s for _, s in to_implement})} symbol(s) to implement.")

    return 1


if __name__ == "__main__":
    sys.exit(main())
