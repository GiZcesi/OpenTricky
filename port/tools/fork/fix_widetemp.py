#!/usr/bin/env python3
"""Fork pass: room for the terminating zero of a wide string the title copies into a block it sized itself.

The text object's "append a narrow string" method (0x000BE4F0, its tail lifted as
sub_000BE51B) measures the string (strlen), allocates `lea edi, [eax+eax]` bytes,
2 per character, from the title's own heap (0x150D70, align 0x10, tag "temp"),
fills the block with 0xDEADC0ED, then widens the string into it with 0x000AA4C0,
which writes 2 bytes per character plus a 2-byte zero. The zero has no room: it
lands 2 bytes past the block. The heap rounds a block up to 16 bytes, so this only
shows when the length is a multiple of 8 (an 8-character trick name such as
"BS Rail " at the results of a Showoff): the zero then overwrites the magic word
of the next block's header. It is the original code, translated as is; where the
next block is a permanent one (the case seen), nothing follows, but it is a write
outside the block, and what sits after the block depends on the order of
allocations.

This pass makes the size `eax + eax + 2` (the original instruction with a +2
displacement): the block, its fill and the copy then agree. The block is freed
right after the text is appended (0x000BE557 onwards); nothing else reads the size.

    fix_widetemp.py GEN_DIR [--dry-run]
Idempotent. Stops unless the function holds exactly one such line, right before
the allocation call it sizes.
"""
import glob, io, os, re, sys

TAG = "/* fork_widetemp */"
FN = "sub_000BE51B"
OLD = re.compile(r"^(?P<ind>[ \t]*)edi = eax \+ eax;(?P<rest>[^\r\n]*)(?P<eol>\r?\n)", re.M)
# The lines that must follow it in the lifted body (the allocation it sizes).
AFTER = ("PUSH32(esp, 0x10);", "PUSH32(esp, edi);", "PUSH32(esp, 0x19BA18);", "sub_00150D70();")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    hits = []
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if "void %s(void)" % FN in src:
            hits.append((path, src))
    if len(hits) != 1:
        sys.exit("fix_widetemp: %s found in %d file(s), 1 expected" % (FN, len(hits)))
    path, src = hits[0]
    nl = "\r\n" if "\r\n" in src else "\n"
    head = "void %s(void)%s{%s" % (FN, nl, nl)
    if head not in src:
        sys.exit("fix_widetemp: %s has an unexpected header" % FN)
    start = src.index(head)
    end = src.index(nl + "}" + nl, start)
    body = src[start:end]
    if TAG in body:
        if body.count(TAG) != 1:
            sys.exit("fix_widetemp: %s carries %d tags, 1 expected" % (FN, body.count(TAG)))
        print("fix_widetemp: %s already patched%s" % (FN, " (dry run)" if dry else ""))
        return
    sites = list(OLD.finditer(body))
    if len(sites) != 1:
        sys.exit("fix_widetemp: %s has %d `edi = eax + eax;` lines, 1 expected: stop rather than guess"
                 % (FN, len(sites)))
    m = sites[0]
    # The allocation call must come right after (comments and RECOMP_LOC lines aside).
    tail = [l.strip() for l in body[m.end():].split(nl)]
    tail = [l for l in tail if l and not l.startswith("RECOMP_LOC") and not l.startswith("/*")]
    for want, got in zip(AFTER, tail):
        if want not in got:
            sys.exit("fix_widetemp: after the size, expected `%s`, found `%s`: stop" % (want, got))
    if "sub_000AA4C0();" not in body[m.end():]:
        sys.exit("fix_widetemp: no copy (sub_000AA4C0) after the allocation in %s: stop" % FN)
    new_line = ("%sedi = eax + eax + 2;%s %s /* 2 bytes per character and 2 for the terminating zero "
                "(the original sizes the block without it, and the copy writes it past the block) */%s"
                % (m.group("ind"), m.group("rest"), TAG, m.group("eol")))
    body = body[:m.start()] + new_line + body[m.end():]
    new = src[:start] + body + src[end:]
    if not dry:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("%s: %s, 1 site" % (os.path.basename(path), FN))
    print("fix_widetemp: 1 site%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
