#!/usr/bin/env python3
"""Fork pass: keep an x87 compare result across a function split.

The x87 status word is CPU state: `fcomp` in one place, `fnstsw ax` later reads
it. The recompiler models it as g_fpu_cmp (global, read by FNSTSW_AX through
FPU_AH(), recomp_types.h:303) plus a per-function local _fpu_cmp, and every
`fnstsw` is emitted as

    g_fpu_cmp = _fpu_cmp; FNSTSW_AX(eax);

When the disassembler splits one machine function, the compare can land in one
C function and the `fnstsw` in the next. The second one declares a fresh
`int _fpu_cmp = 0;` and, before any compare of its own, copies that 0 over the
global: the result the previous fragment left there is replaced by "equal".

Found at sub_000C7FBC (the race HUD, 0x000C7FAF `fcomp` -> 0x000C7FBC
`fnstsw; test ah,1; je`): the branch at 0x000C7FC5 could never be taken. 10 of
the 11 sites tree-wide are CRT math helpers (0x15C9xx..0x15F38A).

This removes `g_fpu_cmp = _fpu_cmp; ` in front of FNSTSW_AX wherever the
function has not assigned _fpu_cmp yet (textual order), so FNSTSW_AX reads the
live global -- what the hardware does. Sites after the function's own first
compare are left alone. Idempotent.

    fix_fpu_cmp_split.py GEN_DIR [--dry-run]
"""
import io, os, re, sys

FUNC = re.compile(r"(\nvoid (sub_[0-9A-F]{8})\(void\)\n\{\n)(.*?)(\n\}\n)", re.S)
STALE = "g_fpu_cmp = _fpu_cmp; FNSTSW_AX("


def fix_body(body):
    if "int _fpu_cmp = 0;" not in body:
        return body, 0
    first_set = body.find("_fpu_cmp = (")
    end = len(body) if first_set < 0 else first_set
    head, tail = body[:end], body[end:]
    n = head.count(STALE)
    if n:
        head = head.replace(STALE, "/* g_fpu_cmp kept from the previous fragment (fork_fpu_cmp_split) */ FNSTSW_AX(")
    return head + tail, n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    gen = args[0]
    total = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith("recomp_0") and f.endswith(".c")):
            continue
        p = os.path.join(gen, f)
        s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
        hits = []

        def sub(m):
            body, n = fix_body(m.group(3))
            if n:
                hits.append((m.group(2), n))
            return m.group(1) + body + m.group(4)
        s2 = FUNC.sub(sub, s)
        for name, n in hits:
            print("  %-16s %s: %d fnstsw site(s)" % (f, name, n))
            total += n
        if hits and not dry:
            io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s2)
    print("stale x87 compare results no longer overwritten: %d%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
