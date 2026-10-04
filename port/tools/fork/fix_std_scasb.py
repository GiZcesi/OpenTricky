#!/usr/bin/env python3
"""Fork pass: `repne scasb` left as a TODO because of the direction flag.

The lifter tracks the direction flag per function (`std`/`cld`, lifter.py
_df_state) and, when it believes DF is set, emits a scan as

    /* TODO: repne scasb with the direction flag set */

which fixrepstr.py does not match (it only rewrites forward scans). The only
site in SSX is the CRT strrchr, 0x0015D780:

    or ecx,-1 / repne scasb / inc ecx / neg ecx / dec edi   ; strlen, forward
    mov al,[ebp+0xC] / std / repne scasb / inc edi          ; search, backward
    cmp [edi],al / je found / xor eax,eax ... cld

Both scans come out as the TODO (DF state is per function and survives the
lifter's second walk over the same function, so even the forward one before
`std` is flagged). With neither scan run, strrchr returns "not found" or a bogus
pointer: the hard-disk save builds `U:\\<folder>\\` with an empty file name and
fails ("Save Failed"). The upstream port's notes list the same TODO
("Hard-disk save", cause 5).

This rewrites each such TODO with a faithful loop whose direction is the one in
force at that point of the function, read from the lifter's own comments: after
`/* std - direction flag set */` (and before any `/* cld`) the scan runs backwards,
otherwise forwards. Intel semantics: stop on the first match, edi one element past the
byte that ended the run (in the scan direction), residual count in ecx, ZF in
g_str_ne as fixrepstr.py does. Idempotent.

    fix_std_scasb.py GEN_DIR [--dry-run]
"""
import io, os, re, sys

FUNC = re.compile(r"(\nvoid (sub_[0-9A-F]{8})\(void\)\n\{\n)(.*?)(\n\}\n)", re.S)
TODO = re.compile(r"^(?P<ind>[ \t]*)/\* TODO: repne scasb with the direction flag set \*/[ \t]*$", re.M)
STD = "/* std - direction flag set */"
CLD = "/* cld - direction flag clear */"

BODY = """{ uint32_t _n = ecx; int _hit = 0;
%(i)s  while (_n) { uint32_t _b = MEM8(edi);
%(i)s               edi %(op)s= 1; _n--;
%(i)s               if (_b == LO8(eax)) { _hit = 1; break; } }
%(i)s  ecx = _n; g_str_ne = !_hit; } /* repne scasb, %(dir)s (fork_std_scasb) */"""


def fix_body(body):
    out, pos, sites = [], 0, []
    for m in TODO.finditer(body):
        before = body[:m.start()]
        down = before.rfind(STD) > before.rfind(CLD)
        ind = m.group("ind")
        out.append(body[pos:m.start()])
        out.append(ind + BODY % {"i": ind, "op": "-" if down else "+",
                                 "dir": "DF set, backwards" if down else "DF clear, forwards"})
        pos = m.end()
        sites.append("backwards" if down else "forwards")
    out.append(body[pos:])
    return "".join(out), sites


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
        if "TODO: repne scasb with the direction flag set" not in s:
            continue
        hits = []

        def sub(m):
            body, sites = fix_body(m.group(3))
            if sites:
                hits.append((m.group(2), sites))
            return m.group(1) + body + m.group(4)

        new = FUNC.sub(sub, s)
        for name, sites in hits:
            print("%s %s: %s" % (f, name, ", ".join(sites)))
            total += len(sites)
        if hits and not dry:
            io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("%s %d site(s)" % ("would rewrite" if dry else "rewrote", total))


if __name__ == "__main__":
    main()
