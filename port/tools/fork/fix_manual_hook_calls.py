#!/usr/bin/env python3
"""Fork pass: route direct calls through the hand-written hooks.

A hook is installed in recomp_lookup_manual() (port/src/*.c):

    if (xbox_va == 0x0016B1C0u) return hook_lockrect_0016B1C0;

but recomp_lookup_manual() is only consulted for *indirect* calls. When the
regen emits a direct call to the same address --

    PUSH32(esp, 0); sub_0016B1C0(); /* call 0x0016B1C0 */

-- the hook never runs. That is why the intro videos were missing: the
video player (sub_00148B40) locks the frame buffer through a direct call to the
Xbox D3D LockRect, hook_lockrect_0016B1C0 never saw the lock, and the runtime
never learned where the decoded frames were. The upstream tree evidently
reached it indirectly.

This pass reads every `xbox_va == 0x...u) return hook_...;` pair from
port/src/*.c and rewrites each direct call or tail call `sub_X();` in the
generated tree into `hook_...();`, adding an extern declaration to the file.
Definitions, declarations and dispatch-table entries are left alone, so the
hook itself still calls the translated body. Idempotent.

    fix_manual_hook_calls.py GEN_DIR SRC_DIR [--dry-run]
    fix_manual_hook_calls.py --check-only SRC_DIR

Two hook bodies for one address: the pass fails. Before the check, a second
pair for an address already hooked elsewhere silently replaced the first one
in the table (the last file in name order won). Every `xbox_va == 0x...u)
return hook_...;` pair of SRC_DIR and its subfolders (not recomp/, the
generated tree) is read; one address with two bodies (two hook names, or one
name defined in two files) stops the regen, unless that address is a chain
listed in hook_chains.txt (next to this script): hooks that call the next
one themselves, or lookups of which only one is consulted. A chained
address is not routed: a direct call to it also stops the regen, since the
pass cannot tell which body should take it.
"""
import io, os, re, sys

PAIR = re.compile(r"xbox_va\s*==\s*0x([0-9A-Fa-f]{1,8})u?\s*\)\s*return\s+([A-Za-z_][A-Za-z0-9_]*)\s*;")
CHAINS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook_chains.txt")


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def hooks(src_dir):
    """Routing table: the pairs of SRC_DIR itself (as before), sub_X -> hook."""
    out = {}
    for f in sorted(os.listdir(src_dir)):
        if f.endswith(".c"):
            for a, h in PAIR.findall(read(os.path.join(src_dir, f))):
                out["sub_%08X" % int(a, 16)] = h
    return out


def all_pairs(src_dir):
    """Every pair of SRC_DIR and its subfolders: {address: {(file, hook): [lines]}}."""
    out = {}
    for root, dirs, files in os.walk(src_dir):
        rel_root = os.path.relpath(root, src_dir).replace("\\", "/")
        if rel_root == ".":
            dirs[:] = sorted(d for d in dirs if d != "recomp")
            rel_root = ""
        else:
            dirs.sort()
        for f in sorted(files):
            if not f.endswith(".c"):
                continue
            rel = rel_root + "/" + f if rel_root else f
            for no, line in enumerate(read(os.path.join(root, f)).splitlines(), 1):
                for a, h in PAIR.findall(line):
                    out.setdefault(int(a, 16), {}).setdefault((rel, h), []).append(no)
    return out


def chains():
    """hook_chains.txt: `0xADDR file:hook file:hook ...  # why`, paths from SRC_DIR."""
    out = {}
    if not os.path.exists(CHAINS):
        return out
    for line in read(CHAINS).splitlines():
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        out[int(line[0], 16)] = set(tuple(x.rsplit(":", 1)) for x in line[1:])
    return out


def check(src_dir):
    """Returns (chained addresses, errors)."""
    allowed = chains()
    chained, errors = set(), []
    for va, bodies in sorted(all_pairs(src_dir).items()):
        if len(bodies) < 2:
            continue
        where = ", ".join("%s:%s %s" % (f, ",".join(map(str, l)), h) for (f, h), l in sorted(bodies.items()))
        if va in allowed and set(bodies) <= allowed[va]:
            chained.add(va)
            print("chained hooks at 0x%08X (hook_chains.txt): %s" % (va, where))
        else:
            errors.append("two hook bodies for 0x%08X: %s" % (va, where))
    return chained, errors


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if "--check-only" in sys.argv:
        chained, errors = check(args[0])
        for e in errors:
            print("ERROR: " + e)
        print("hook check: %d chained address(es), %d error(s)" % (len(chained), len(errors)))
        sys.exit(2 if errors else 0)
    gen, src = args[0], args[1]
    chained, errors = check(src)
    if errors:
        for e in errors:
            print("ERROR: " + e)
        print("ERROR: one body per address (or list the chain in %s)" % CHAINS)
        sys.exit(2)
    table = hooks(src)
    held = sorted(sub for sub in table if int(sub[4:], 16) in chained)
    for sub in held:
        del table[sub]
    print("hooks in recomp_lookup_manual: %s" % ", ".join("%s -> %s" % kv for kv in sorted(table.items())))
    total = 0
    if held:
        direct = []
        for f in sorted(os.listdir(gen)):
            if f.startswith("recomp_0") and f.endswith(".c"):
                s = io.open(os.path.join(gen, f), encoding="utf-8", errors="surrogateescape", newline="").read()
                for sub in held:
                    n = len(re.findall(r"(?<![A-Za-z0-9_])%s\(\);" % sub, s))
                    if n:
                        direct.append("%s: %d direct call(s) to %s" % (f, n, sub))
        if direct:
            for d in direct:
                print("ERROR: chained address called directly, " + d)
            sys.exit(2)
        print("chained addresses, not routed (no direct call): %s" % ", ".join(held))
    for f in sorted(os.listdir(gen)):
        if not (f.startswith("recomp_0") and f.endswith(".c")):
            continue
        p = os.path.join(gen, f)
        s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
        n_file = 0
        for sub, hook in table.items():
            # a call statement, never `void sub_X(void)` nor `(recomp_func_t)sub_X`
            pat = re.compile(r"(?<![A-Za-z0-9_])%s\(\);" % sub)
            s, n = pat.subn(hook + "();", s)
            if n:
                decl = "extern void %s(void);   /* fork_manual_hook_calls */" % hook
                if decl not in s:
                    s = s.replace('#include "recomp_funcs.h"', '#include "recomp_funcs.h"\n' + decl, 1)
                print("  %-16s %d direct call(s) to %s -> %s" % (f, n, sub, hook))
                n_file += n
        if n_file and not dry:
            io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        total += n_file
    print("direct calls routed through hooks: %d%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
