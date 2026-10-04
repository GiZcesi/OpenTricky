#!/usr/bin/env python3
"""Revive branches whose condition is computed in a different function.

The disassembler cuts one machine function into several C functions wherever a
branch target looked like an entry point. That is usually harmless -- the
generated pieces tail-call each other and the global register variables carry
state across. Flags do not: they are the one piece of machine state the lifter
models per-function.

So when the flag-setting instruction lands in one piece and the `jcc` that
reads it lands in the next, the condition has nowhere to live. The producer
emits a comment and nothing else:

    (void)0; /* test eax, eax - flags set for next jcc */
    g_seh_ebp = ebp; sub_0014FC9A(); return;

and the consumer declares a fresh local, initialised to zero, and branches on
it:

    int _flags = 0; /* fallback flag var */
    if (_flags /* je: equal / zero */) { ... }

`_flags` is never assigned, so the branch is **never taken**. Not "usually not
taken" -- never. 29 branches across 23 functions were dead this way, and they
are invisible in a diff because both halves look like ordinary generated code.

One of them is why the frontend never loads. `FUN_0014fba0` walks a pack's
directory looking for an entry name; the `stricmp` result is tested in
`sub_0014FC8B` and branched on in `sub_0014FC9A`, so no name ever matched.
Every `|data/models/ssxfe.*` open returned NULL, `FUN_00141d20` did not check
it, and the loop wrote `*(0 + index*4)` for a quarter-million indices straight
across guest .text.

The fix carries the operands rather than the flags. Each producer gets a
`SPLIT_CMP(a, b, is_test)` before its tail call, and each consumer's
`if (_flags /* jcc */)` becomes the matching `SPLIT_J*` expression. Operands
are enough to rebuild every condition except overflow (`jo`/`jno`) and parity
(`jp`/`jnp`), which are left alone and reported.

    fixsplitflags.py --dry-run
    fixsplitflags.py

A consumer is only rewritten when **every** function that tail-calls it ends
with a parseable flag-setting comment, because a consumer reached from two
producers must get the right comparison from each.
"""

import argparse
import ssxpaths
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GEN = os.path.normpath(
    ssxpaths.GEN)

FUNC = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\)$")
DECL = re.compile(r"^\s*int _flags = 0; /\* fallback flag var \*/\s*$")
ASSIGN = re.compile(r"\b_flags\s*=(?!=)")
USE = re.compile(r"if \(_flags\s*/\* (?P<cc>j[a-z]+):")
TAIL = re.compile(r"g_seh_ebp = ebp; (?P<name>[A-Za-z_][A-Za-z0-9_]*)\(\); return;")
SETS = re.compile(r"^\s*\(void\)0; /\* (?P<op>test|cmp) (?P<a>.+?), (?P<b>.+?)"
                  r" - flags set for next jcc \*/\s*$")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+): ;")
CALL_ANY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\(\); /\* call|RECOMP_ICALL")

CC = {"je": "SPLIT_JE", "jz": "SPLIT_JE", "jne": "SPLIT_JNE", "jnz": "SPLIT_JNE",
      "jb": "SPLIT_JB", "jc": "SPLIT_JB", "jnae": "SPLIT_JB",
      "jae": "SPLIT_JAE", "jnb": "SPLIT_JAE", "jnc": "SPLIT_JAE",
      "jbe": "SPLIT_JBE", "jna": "SPLIT_JBE",
      "ja": "SPLIT_JA", "jnbe": "SPLIT_JA",
      "jl": "SPLIT_JL", "jnge": "SPLIT_JL",
      "jge": "SPLIT_JGE", "jnl": "SPLIT_JGE",
      "jle": "SPLIT_JLE", "jng": "SPLIT_JLE",
      "jg": "SPLIT_JG", "jnle": "SPLIT_JG"}


def load(gen):
    """{path: [lines]} plus {func: (path, start, end)}."""
    files, index, targets = {}, {}, {}
    for path in sorted(glob.glob(os.path.join(gen, "*.c"))):
        text = io.open(path, encoding="utf-8", errors="surrogateescape").read()
        lines = text.split("\n")
        files[path] = lines
        targets[path] = set(re.findall(r"goto (loc_[0-9A-Fa-f]+)", text))
        targets[path] = set(re.findall(r"goto (loc_[0-9A-Fa-f]+)", text))
        start = None; name = None
        for i, l in enumerate(lines):
            m = FUNC.match(l)
            if m:
                if name:
                    index[name] = (path, start, i)
                name, start = m.group(1), i
            elif l == "}" and name is not None and start is not None:
                index[name] = (path, start, i)
                name = start = None
    return files, index, targets


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files, index, targets_by_file = load(args.gen)
    if not index:
        sys.exit("no generated sources under %s" % args.gen)

    # consumers: declare _flags, never assign it, branch on it
    consumers = {}
    for name, (path, s, e) in index.items():
        body = files[path][s:e]
        if not any(DECL.match(l) for l in body):
            continue
        if any(ASSIGN.search(l) and not DECL.match(l) for l in body):
            continue
        uses = [(i + s, USE.search(l).group("cc"))
                for i, l in enumerate(body) if USE.search(l)]
        if uses:
            consumers[name] = (path, uses)

    # producers: tail-call a consumer, with a flag-setting comment just before
    producers = {}
    for name, (path, s, e) in index.items():
        lines = files[path]
        for i in range(s, e):
            m = TAIL.search(lines[i])
            if not m or m.group("name") not in consumers:
                continue
            setter = None; joined = False
            # Scan back for the flag-setting comment. The lifter emits it only
            # for the instruction whose flags the next jcc consumes, so what it
            # puts in between (moves, pushes, lea) is known not to clobber them
            # -- scanning past those is safe. Stop at a call or another compare.
            for j in range(i - 1, max(s, i - 14), -1):
                sm = SETS.match(lines[j])
                if sm:
                    setter = (j, sm); break
                t = lines[j].strip()
                if not t:
                    continue
                if CALL_ANY.search(t):
                    break
                lm = LABEL.match(t)
                if lm:
                    # A label something jumps to is a join point: control can
                    # reach the tail call without passing the comparison.
                    if lm.group(1) in targets_by_file.get(path, set()):
                        joined = True
                        break
                    continue
            producers.setdefault(m.group('name'), []).append(
                (path, i, None if joined else setter))

    fixed_c = fixed_p = 0
    skipped = []
    edits = {}   # path -> {line: newtext}

    for name, (cpath, uses) in sorted(consumers.items()):
        preds = producers.get(name, [])
        if not preds:
            skipped.append("%s: no producer tail-calls it" % name)
            continue
        if any(st is None for _p, _i, st in preds):
            skipped.append("%s: a producer has no parseable comparison" % name)
            continue
        bad = [cc for _ln, cc in uses if cc not in CC]
        if bad:
            skipped.append("%s: unsupported condition %s (needs OF/PF)"
                           % (name, ",".join(sorted(set(bad)))))
            continue
        for ppath, _i, (j, sm) in preds:
            ind = re.match(r"^(\s*)", files[ppath][j]).group(1)
            edits.setdefault(ppath, {})[j] = (
                "%sSPLIT_CMP(%s, %s, %d); /* %s %s, %s -- carried across the "
                "split to %s */" % (ind, sm.group("a"), sm.group("b"),
                                    1 if sm.group("op") == "test" else 0,
                                    sm.group("op"), sm.group("a"), sm.group("b"),
                                    name))
            fixed_p += 1
        for ln, cc in uses:
            old = files[cpath][ln]
            edits.setdefault(cpath, {})[ln] = old.replace("_flags", CC[cc], 1)
            fixed_c += 1

    if args.dry_run:
        for path, d in sorted(edits.items()):
            for ln in sorted(d)[:3]:
                print("%s:%d" % (os.path.basename(path), ln + 1))
                print("  -%s" % files[path][ln].strip()[:96])
                print("  +%s" % d[ln].strip()[:96])
    else:
        for path, d in edits.items():
            lines = files[path]
            for ln, new in d.items():
                lines[ln] = new
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))

    print("\n%s %d branch(es) in %d consumer(s), %d producer(s) instrumented"
          % ("would revive" if args.dry_run else "revived",
             fixed_c, len(set(c for c in consumers if c in producers)), fixed_p))
    if skipped:
        print("\nleft alone:")
        for s in skipped:
            print("    %s" % s)


if __name__ == "__main__":
    main()
