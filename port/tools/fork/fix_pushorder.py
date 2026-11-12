#!/usr/bin/env python3
"""Fork pass: the push-buffer write pointer of the simple render states stored after its data.

The title's D3D runtime writes NV2A methods into the push buffer and moves the
write pointer of the push context (Xbox VA 0x174B30; [0x1776C0] points at it).
Every writer reserves room, stores the method header and its data, and only
then stores the new write pointer -- except the fast path of the simple render
states, sub_00166FE0 (ecx = method header, edx = value), which D3DDevice_
SetRenderState (0x167030) and some fifty direct calls of the title go through
(alpha test, alpha reference, blend factors, depth function, colour mask,
stencil, ...):

    0x00166FF0  mov [0x174B30], eax     ; the pointer first
    0x00166FF5  mov [eax-8], ecx        ; then the header
    0x00166FF8  mov [eax-4], edx        ; then the value

On the console that order is harmless: the GPU reads up to PUT, which the
runtime writes at a kick, after whole commands, never up to the write pointer.
Here the pump thread translates everything below the write pointer as soon as
it moves (xboxrecomp/src/nv2a/nv2a_live_pb.c, which relies on "anything below
the sampled pointer is complete"). Sampled between the stores, the last two
dwords of a batch are those of an older pass of the ring: the pump skips them
as "not a command" -- or runs a stale command -- and never sees the render
state the title did send; the title only sends a state again when it changes,
so the wrong state stays until then.

The pass stores the header and the value first and the pointer last, as every
other writer of the runtime does. Same stores, same values, no load in between:
the title's thread ends the function in the same state; only the order the pump
can observe changes. MEM32 is volatile, so the compiler keeps the order, and
x86 makes the stores visible in that order.

    fix_pushorder.py GEN_DIR [--dry-run]
Idempotent. Stops if sub_00166FE0 changed shape.
"""
import glob, io, os, sys

TAG = "/* fork_pushorder */"
FUNC = "void sub_00166FE0(void)\n{\n"
OLD = ("    MEM32(0x174B30) = eax;\n"
       "    MEM32(eax + -8) = ecx;\n"
       "    MEM32(eax + -4) = edx;\n")
NEW = ("    MEM32(eax + -8) = ecx;\n"
       "    MEM32(eax + -4) = edx;\n"
       "    MEM32(0x174B30) = eax; " + TAG + " /* the write pointer after its data: the pump reads up to it */\n")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    hits = []
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if "void sub_00166FE0(void)" in src:
            hits.append((path, src))
    if len(hits) != 1:
        sys.exit("fix_pushorder: expected sub_00166FE0 in exactly 1 file, found %d" % len(hits))
    path, src = hits[0]
    nl = "\r\n" if "\r\n" in src else "\n"
    func, old, new_txt = (s.replace("\n", nl) for s in (FUNC, OLD, NEW))
    start = src.index(func)
    end = src.index(nl + "}" + nl, start)
    body = src[start:end]
    if TAG in body:
        print("%s: sub_00166FE0 already patched" % os.path.basename(path))
        print("fix_pushorder: 1 site")
        return
    if body.count(old) != 1:
        sys.exit("fix_pushorder: the three stores of sub_00166FE0 are not there once, in that order: "
                 "stop rather than guess")
    new = src[:start] + body.replace(old, new_txt, 1) + src[end:]
    print("%s: sub_00166FE0 stores the header and the value before the write pointer" % os.path.basename(path))
    if not dry:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("fix_pushorder: 1 site%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
