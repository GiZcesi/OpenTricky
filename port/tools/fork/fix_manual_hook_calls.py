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
"""
import io, os, re, sys

PAIR = re.compile(r"xbox_va\s*==\s*0x([0-9A-Fa-f]{1,8})u?\s*\)\s*return\s+([A-Za-z_][A-Za-z0-9_]*)\s*;")


def hooks(src_dir):
    out = {}
    for f in sorted(os.listdir(src_dir)):
        if f.endswith(".c"):
            for a, h in PAIR.findall(io.open(os.path.join(src_dir, f), encoding="utf-8", errors="replace").read()):
                out["sub_%08X" % int(a, 16)] = h
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    gen, src = args[0], args[1]
    table = hooks(src)
    print("hooks in recomp_lookup_manual: %s" % ", ".join("%s -> %s" % kv for kv in sorted(table.items())))
    total = 0
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
