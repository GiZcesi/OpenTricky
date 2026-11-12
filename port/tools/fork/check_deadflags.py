#!/usr/bin/env python3
"""Regen guard: no new never-taken `if (_flags ...)` branch in gen/.

A branch on the fallback `_flags = 0` local is never taken (see fixsplitflags.py
and fix_deadflags.py). After all passes, the ones left in gen/ must be the known
harmless ones listed in ALLOWED (function -> count). The regen fails if a
function outside the list has one, if a listed function has more than its
count, or if `_flags` is used in a form this guard does not know. A listed site
that is gone is only reported.

It also fails on an adc / sbb that reads the CF of a shift or of another
adc / sbb in another block: the lifter sets that CF only for a reader in the
same block, and marks any other reader (lifter._CF_NOT_MODELLED).

    check_deadflags.py GEN_DIR
"""
import glob, io, os, re, sys

ALLOWED = {
    # Lifter left the second jcc on the flags of a `sbb` (XAPI timer queue):
    # differs only when the low dword is exactly 0, with the same result.
    "sub_001521B3": 1,
    # XAPI date code, `cmp ax, di`: the taken path adds 0 when the branch would be taken.
    "sub_00152660": 1,
    # CRT x87 cos, sin, asin/acos, sqrt, acos/asin, tan, atan: flags returned by a
    # helper through `ret` (exponent 0x7FF); only Inf/NaN arguments take the branch.
    "sub_0015C911": 1, "sub_0015C9C1": 1, "sub_0015CAFD": 1, "sub_0015CBC9": 1,
    "sub_0015CC85": 1, "sub_0015CD51": 1, "sub_0015D07B": 1,
    # Not code: middle of an instruction (0x40030, 0x160200) or data (WAVEFORMATEX
    # and float tables at 0x17F200, a device-name string at 0x18727C), reachable only
    # through the dispatch table.
    "sub_00040030": 1, "sub_00160200": 1, "sub_0017F200": 6, "sub_0017F3B4": 1,
    "sub_0018727C": 2,
}

FUNC = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(void\)\r?$")
DECL = "int _flags = 0; /* fallback flag var */"
DEAD = re.compile(r"^\s*if \(_flags /\* [a-z]+:")
SWITCHED = re.compile(r"\bg_fix_[a-z_]+\b.*\? .* : _flags /\* j[a-z]+:")
USE = re.compile(r"\b_flags\b")
CF_NOT_MODELLED = "from another block: not modelled"   # lifter._CF_NOT_MODELLED
# Readers of such a CF proven harmless (function -> count).
CF_ALLOWED = {
    # Not code: the seven-arm jump table at 0x12CD6C and the byte index table at
    # 0x12CD88 of the switch before them, decoded from the padding at 0x12CD69
    # (`adc al, [eax]` is 12 00, a table word). Nothing references 0x12CD69.
    "sub_0012CD69": 1,
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    found, odd, cfmiss = {}, [], []
    for path in sorted(glob.glob(os.path.join(sys.argv[1], "recomp_*.c"))):
        name = None
        lines = io.open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")
        for i, l in enumerate(lines):
            m = FUNC.match(l)
            if m:
                name = m.group(1)
                continue
            if CF_NOT_MODELLED in l:
                # An adc / sbb reading the CF of a shift or adc / sbb in another
                # block: the lifter sets that CF only within the setter's block,
                # so `_cf` would be stale here (lifter._shift_cf_stmt).
                cfmiss.append((name, "%s:%d %s" % (os.path.basename(path), i + 1, l.strip()[:100])))
                continue
            if "_flags" not in l or not USE.search(l) or l.strip() == DECL:
                continue
            if DEAD.match(l):
                found.setdefault(name, []).append("%s:%d" % (os.path.basename(path), i + 1))
            elif not SWITCHED.search(l):
                odd.append("%s:%d %s" % (os.path.basename(path), i + 1, l.strip()[:100]))
    total = sum(len(v) for v in found.values())
    bad = ["%s: %d dead branch(es), %d allowed (%s)" % (n, len(v), ALLOWED.get(n, 0), ", ".join(v))
           for n, v in sorted(found.items()) if len(v) > ALLOWED.get(n, 0)]
    gone = sorted(n for n in ALLOWED if n not in found)
    cfcount = {}
    for n, _ in cfmiss:
        cfcount[n] = cfcount.get(n, 0) + 1
    cfbad = [where for n, where in cfmiss if cfcount[n] > CF_ALLOWED.get(n, 0)]
    print("check_deadflags: %d never-taken `if (_flags` branch(es) in %d function(s), "
          "%d listed function(s) gone" % (total, len(found), len(gone)))
    for n in gone:
        print("    gone (fine): %s" % n)
    if cfmiss:
        print("check_deadflags: %d adc / sbb reading a CF set in another block, %d allowed"
              % (len(cfmiss), len(cfmiss) - len(cfbad)))
    if bad or odd or cfbad:
        for b in bad:
            print("NEW DEAD BRANCH: " + b)
        for o in odd:
            print("UNKNOWN _flags USE: " + o)
        for c in cfbad:
            print("CF READ IN ANOTHER BLOCK: " + c)
        print("check_deadflags: FAILED -- a branch the lifter cut from its flags is never "
              "taken; fix it (fix_deadflags.py) or, if proven harmless, list it in ALLOWED")
        sys.exit(1)
    print("check_deadflags: OK")


if __name__ == "__main__":
    main()
