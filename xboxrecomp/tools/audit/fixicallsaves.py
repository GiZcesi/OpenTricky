"""Move the _icall_esp capture past leading callee-saved register pushes.

The lifter opens an indirect-call block at the first PUSH of the emitted run,
which conflates frame saves with call arguments:

    { uint32_t _icall_esp = g_esp;
      PUSH32(esp, ebp); PUSH32(esp, esi); PUSH32(esp, edi);   <- frame saves
      PUSH32(esp, edx); PUSH32(esp, edx);                     <- real args
      PUSH32(esp, 0); RECOMP_ICALL_SAFE(target, _icall_esp); }

On an ICALL miss RECOMP_ICALL_SAFE rewinds esp to _icall_esp, discarding the
saves along with the args -- 4 bytes of frame damage each -- and the epilogue
then pops stack garbage into the callee-saved registers.

A leading push is treated as a frame save only when all three hold, so the pass
keys on real structure rather than on the register name alone:

  1. the capture sits in the function entry block (no loc_ label before it),
  2. the pushed registers are callee-saved (ebx/ebp/esi/edi),
  3. some epilogue pops exactly those registers in reverse order, where the
     epilogue pop order is read in sequence and may be interleaved with other
     statements (the lifter routinely splits it with stores and moves).
"""
import io, os, re, sys

SAVEABLE = ("ebp", "esi", "edi", "ebx")
CAP = "{ uint32_t _icall_esp = g_esp;"
PUSH_RE = re.compile(r"^\s*PUSH32\(esp,\s*([a-z]{3})\);\s*$")
POP_RE = re.compile(r"^\s*POP32\(esp,\s*([a-z]{3})\);\s*$")
LABEL_RE = re.compile(r"^\s*loc_[0-9A-Fa-f]{8}:\s*;\s*$")
RET_RE = re.compile(r"\breturn;")
FUNC_RE = re.compile(r"^void (sub_[0-9A-Fa-f]{8}|[A-Za-z_][A-Za-z0-9_]*)\(void\)\s*$")


def func_spans(lines):
    spans, i = [], 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1].strip() == "{":
            j = i + 2
            while j < len(lines) and lines[j] != "}":
                j += 1
            spans.append((m.group(1), i, j))
            i = j + 1
        else:
            i += 1
    return spans


def epilogue_pops(lines, lo, hi):
    """Pop order for each return site, read back to the preceding label."""
    orders = []
    for i in range(lo, hi):
        if not RET_RE.search(lines[i]):
            continue
        regs, j = [], i
        while j >= lo and not LABEL_RE.match(lines[j]):
            m = POP_RE.match(lines[j])
            if m:
                regs.append(m.group(1))
            j -= 1
        if regs:
            orders.append(regs[::-1])  # walked backwards, so undo
    return orders


def scan_file(path, apply_fix):
    raw = io.open(path, encoding="utf-8", errors="surrogateescape").read()
    lines = [l.rstrip("\r") for l in raw.split("\n")]
    hits = []
    for name, fs, fe in func_spans(lines):
        orders = epilogue_pops(lines, fs, fe)
        if not orders:
            continue
        nlab = 0
        for i in range(fs, fe):
            if LABEL_RE.match(lines[i]):
                nlab += 1
                if nlab > 1:
                    break  # left the entry block; prologue saves cannot start here
            if lines[i].strip() != CAP:
                continue
            lead, j = [], i + 1
            while j < fe:
                m = PUSH_RE.match(lines[j])
                if not m or m.group(1) not in SAVEABLE:
                    break
                lead.append((j, m.group(1)))
                j += 1
            if not lead:
                continue
            best = 0
            for k in range(len(lead), 0, -1):
                want = [r for _, r in lead[:k]][::-1]
                if any(o == want for o in orders):
                    best = k
                    break
            if best:
                hits.append((name, i, [r for _, r in lead[:best]], lead[best - 1][0]))
    if apply_fix and hits:
        for name, cap_i, regs, last_i in sorted(hits, key=lambda h: -h[1]):
            cap_line = lines[cap_i]
            saves = lines[cap_i + 1 : last_i + 1]
            lines[cap_i : last_i + 1] = saves + [cap_line]
        io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write("\n".join(lines))
    return hits


def main():
    apply_fix = "--apply" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = args[0]
    only = args[1] if len(args) > 1 else None
    files = sorted(os.path.join(root, f) for f in os.listdir(root) if f.endswith(".c"))
    total, byreg = 0, {}
    for p in files:
        hits = scan_file(p, apply_fix)
        hits = [h for h in hits if only is None or h[0] == only]
        if not hits:
            continue
        total += len(hits)
        for name, _, regs, _ in hits:
            byreg[len(regs)] = byreg.get(len(regs), 0) + 1
        if only:
            for name, ci, regs, li in hits:
                print("  %s line %d  saves=%s (%d bytes)"
                      % (name, ci + 1, ",".join(regs), 4 * len(regs)))
    print("sites=%d  by save-count: %s  (%s)"
          % (total, dict(sorted(byreg.items())), "APPLIED" if apply_fix else "scan"))


main()
