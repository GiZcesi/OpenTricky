#!/usr/bin/env python3
"""Fork pass: read an esp-relative indirect call target before the call's push.

`call dword ptr [esp+N]` reads its target *before* pushing the return address.
The lifter emits the dummy return push first and evaluates the operand after:

    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(esp + 0x1C), _icall_esp);

so MEM32(esp + 0x1C) is read 4 bytes too low and the call goes to the wrong
slot. Found at 0x0014E7EF (sub_0014E770, the archive walker behind
sub_0014BDA0): the callback 0x0014BD20 was never called, the "target" was
implausible, RECOMP_ICALL_SAFE rewound esp over the two argument pushes, and
the frame drift zeroed esi/edi up to Application_RunMainLoop (this = NULL):
the language files were never opened and the boot spun on a null vtable.

Rewritten to

    { uint32_t _icall_tgt = MEM32(esp + 0x1C); PUSH32(esp, 0); RECOMP_ICALL_SAFE(_icall_tgt, _icall_esp); }

Idempotent (a rewritten site no longer matches). Only operands that read esp
are touched; PUSH32 itself already evaluates its value before decrementing.

    fix_icall_esp_target.py GEN_DIR [--dry-run]
"""
import io, os, re, sys

SITE = re.compile(r"PUSH32\(esp, 0\); (RECOMP_ICALL(?:_SAFE)?)\((MEM32\([^()]*\besp\b[^()]*\))((?:, [A-Za-z_][A-Za-z0-9_]*)?)\);")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    gen = args[0]
    total = 0
    for f in sorted(os.listdir(gen)):
        if not f.endswith(".c"):
            continue
        p = os.path.join(gen, f)
        src = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
        new, n = SITE.subn(lambda m: "{ uint32_t _icall_tgt = %s; PUSH32(esp, 0); %s(_icall_tgt%s); }"
                           % (m.group(2), m.group(1), m.group(3)), src)
        if n:
            total += n
            print("  %-26s %d site(s)" % (f, n))
            if not dry:
                io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("esp-relative indirect call targets fixed: %d%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
