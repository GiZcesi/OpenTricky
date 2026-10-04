"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: double fp_stack[8] with fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import os
import struct

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size, segment=None):
    """Return the MEM macro name for a given operand size.

    An `fs:`-segment operand gets the FS* family instead, which resolves
    against the thread's TIB explicitly. Without this the prefix is dropped and
    `mov eax, fs:0x28` becomes a plain `MEM32(0x28)` -- indistinguishable from a
    genuine null dereference at offset 0x28, which is the ambiguity the
    low-address TIB redirect exists to paper over.
    """
    if segment == "fs":
        return {1: "FS8", 2: "FS16", 4: "FS32"}.get(size, "FS32")
    return {1: "MEM8", 2: "MEM16", 4: "MEM32"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {1: "SMEM8", 2: "SMEM16", 4: "SMEM32"}.get(size, "SMEM32")


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size, getattr(op, 'mem_segment', None))
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size, getattr(op, 'mem_segment', None))
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    elif op.type == "snap":
        # A flag-operand snapshot; `reg` holds its width-preserving C read.
        return op.reg
    return "/* unknown operand */"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

# Mnemonics routed to _lift_sse().
#
# This is the dispatch gate, and it is the ONLY thing that decides whether an
# SSE/MMX opcode gets a translation or falls through to the `/* TODO: ... */`
# fallback at the end of _lift_one(). It used to be an inline tuple at the
# dispatch site, duplicating part of the list in _EFLAGS_PRESERVE below -- and
# that list is about *flags*, not dispatch, so adding an opcode there looks
# like it should work and does nothing. Adding paddusb/pmaddwd/psrld to it and
# writing the handlers left every site still emitted as a TODO comment.
#
# A TODO comment for an MMX opcode is silent data loss, not a missing nicety:
# the video's YUV-to-RGB converters are written entirely in MMX, so a dropped
# opcode corrupts the picture rather than slowing it. Keep this set and the
# handlers in _lift_sse() in step -- an entry here with no handler falls
# through to the generic SSE comment, which is the same silent loss.
_SSE_DISPATCH = frozenset({
    # scalar float
    "movss", "movsd", "addss", "subss", "mulss", "divss", "sqrtss",
    "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
    "minss", "maxss", "minsd", "maxsd", "rsqrtss", "rcpss",
    "comiss", "comisd", "ucomiss", "ucomisd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si", "cvtss2sd", "cvtsd2ss",
    # packed float
    "movaps", "movups", "movlps", "movhps", "movapd", "movupd", "movdqa", "movdqu",
    "movlpd", "movhpd", "movlhps", "movhlps", "movntps", "movntdq",
    "andpd", "orpd", "andnps", "andnpd", "cmpnltps", "cmpnleps",
    "addps", "subps", "mulps", "divps", "minps", "maxps",
    "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "xorps", "xorpd", "andps", "orps",
    "cmpneqps", "cmpeqps", "cmpltps", "cmpleps", "movmskps",
    # MMX moves
    "movd", "movq", "movntq", "emms",
    # MMX bitwise
    "pand", "pandn", "por", "pxor",
    # MMX packed arithmetic
    "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
    "paddusb", "paddusw", "psubusb", "psubusw",
    "paddsb", "paddsw", "psubsb", "psubsw",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pavgb", "pavgw", "psadbw",
    "pminub", "pmaxub", "pminsw", "pmaxsw",
    # MMX shifts
    "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad",
    # MMX pack / unpack
    "packuswb", "packsswb", "packssdw",
    "punpcklbw", "punpckhbw", "punpcklwd", "punpckhwd",
    "punpckldq", "punpckhdq",
    # MMX compare
    "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd",
})

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg",           # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
    "lock xadd",           # Lock prefix - complex flag behavior
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "paddusb", "paddusw", "psubusb", "psubusw",
    "paddsb", "paddsw", "psubsb", "psubsw",
    "pavgb", "pavgw", "psadbw",
    "pminub", "pmaxub", "pminsw", "pmaxsw",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


_TREE_NAMES = None


def _tree_names():
    """VA -> symbol, from the generated tree's dispatch table.

    Functions in the tree get renamed after generation (Heap_Free for
    sub_00150950, CRT_ftol_TruncateToInt64 for sub_0015CA68, ...), so a freshly
    lifted body that calls them by their sub_ name does not link. relift.py and
    recover_batch.py used to paper over that afterwards, or not at all (a relift left six undefined references). XLIFT_NAME_MAP names the
    recomp_dispatch.c to read; without it nothing changes."""
    global _TREE_NAMES
    if _TREE_NAMES is None:
        _TREE_NAMES = {}
        p = os.environ.get("XLIFT_NAME_MAP")
        if p and os.path.exists(p):
            import re as _re
            for l in open(p, encoding="utf-8", errors="replace"):
                m = _re.search(r"\{ 0x([0-9A-Fa-f]{8})u, \(recomp_func_t\)(\w+) \}", l)
                if m:
                    _TREE_NAMES[int(m.group(1), 16)] = m.group(2)
    return _TREE_NAMES


# Pseudo flag setter for a block whose predecessors disagree; its single
# "operand" is the name prefix of the per-condition locals they assign.
FIN_SETTER = "__fin"


def _make_condition(jcc, flag_setter, flag_ops):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.

    The sign and carry of an 8- or 16-bit operation live in bit 7 / bit 15.
    The raw forms below cast to (int32_t)/(uint32_t), which on a byte operand
    never sees that bit: `test al, al; jge` read 0xFF as 255 >= 0, so every AI
    rider in a race took the human-player branch and the race paused with
    "controller disconnected" (Race_SpawnRidersAndLoadAssets
    0x0002DDC8). Every cast in the condition is narrowed to the operation's
    width here.
    """
    r = _make_condition_raw(jcc, flag_setter, flag_ops)
    if (r is None or flag_setter == FIN_SETTER or not flag_ops
            or flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip",
                               "fcomi", "sahf", "comiss", "comisd", "ucomiss", "ucomisd")):
        return r
    size = _op_size(flag_ops[0])
    if size not in (1, 2):
        return r
    nt, ut = ("(int8_t)", "(uint8_t)") if size == 1 else ("(int16_t)", "(uint16_t)")
    cond, desc = r
    cmp_macro = (COND_MAP.get(jcc) or (None,))[0]
    if (flag_setter == "cmp" and cmp_macro in ("CMP_L", "CMP_GE", "CMP_LE", "CMP_G",
                                               "CMP_EQ", "CMP_NE", "CMP_B", "CMP_AE",
                                               "CMP_BE", "CMP_A")
            and len(flag_ops) >= 2 and cond.startswith(cmp_macro + "(")):
        # Both operands at the operation width: an imm8 of 0x80..0xFF is
        # negative in a byte compare, but RECOMP_SEXT sees a plain int.
        # Equality and unsigned compares too -- `cmp word [x], -1`
        # was emitted as CMP_NE(MEM16(x), 0xFFFFFFFFu), which never matches a
        # zero-extended 16-bit load; the pose sampler's "is this the root
        # bone" test (0x107E07 / 0x10803B) always failed and riders' bodies
        # rendered un-rotated -- mirrored against the board in the race.
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
        return f"{cmp_macro}({ut}({lhs}), {ut}({rhs}))", desc
    if flag_setter == "test" and cmp_macro and len(flag_ops) >= 2 and cond.startswith(cmp_macro + "("):
        # `a & b` is promoted to int, which hides the operand width from
        # RECOMP_SEXT (sizeof-based); cast it back.
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
        return f"{cmp_macro}({ut}({lhs} & {rhs}), 0)", desc
    return cond.replace("(int32_t)", nt).replace("(uint32_t)", ut), desc


def _make_condition_raw(jcc, flag_setter, flag_ops):
    """Width-agnostic conditions; _make_condition narrows them."""
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    # Flags materialised by each predecessor (translator._materialise_flags):
    # the block is reached from setters that disagree, so the condition was
    # evaluated at the end of every predecessor into a named local.
    if flag_setter == FIN_SETTER:
        return f"{flag_ops[0]}_{jcc}", desc

    if len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # ── FPU compare-to-EFLAGS and sahf: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi", "sahf"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(_fpu_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "0 /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "1 /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        def _sse_op(op):
            if op.type == "reg":
                if op.reg.startswith("xmm"):
                    return op.reg + (".d" if flag_setter.endswith("sd") else ".f")
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        a = _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f"
        b = _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f"
        # comiss uses unsigned condition codes (CF, ZF)
        if jcc in ("ja", "jnbe"):
            return f"({a} > {b})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({a} >= {b})", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({a} < {b})", desc
        if jcc in ("jbe", "jna"):
            return f"({a} <= {b})", desc
        if jcc in ("je", "jz"):
            return f"({a} == {b})", desc
        if jcc in ("jne", "jnz"):
            return f"({a} != {b})", desc
        if jcc == "jp":
            return f"0 /* {jcc}: unordered/NaN */", desc
        if jcc == "jnp":
            return f"1 /* {jcc}: ordered */", desc
        return None

    # ── cmp: flags from (a - b), operands unchanged ──
    if flag_setter == "cmp":
        if cmp_macro:
            return f"{cmp_macro}({lhs}, {rhs})", desc
        if jcc == "js":
            return f"((int32_t)({lhs} - {rhs}) < 0)", desc
        if jcc == "jns":
            return f"((int32_t)({lhs} - {rhs}) >= 0)", desc
        if jcc in ("jp", "jnp"):
            return f"1 /* {jcc} after cmp - parity */", desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return f"{cmp_macro}({lhs} & {rhs}, 0)", desc
        if jcc == "js":
            return f"((int32_t)({lhs} & {rhs}) < 0)", desc
        if jcc == "jns":
            return f"((int32_t)({lhs} & {rhs}) >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc in ("jp", "jnp"):
            return f"1 /* {jcc} after test - parity */", desc
        return None

    # ── sub: a = a - b, flags from (a_orig - b) ──
    if flag_setter == "sub":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        # Ordered: reconstruct original a = result + b
        if cmp_macro and rhs:
            return f"{cmp_macro}((uint32_t){lhs} + (uint32_t){rhs}, (uint32_t){rhs})", desc
        if jcc in ("jb", "jnae"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── add: a = a + b, flags from result ──
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc == "jle":
            return f"((int32_t){lhs} <= 0)", desc
        if jcc == "jg":
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jb", "jnae", "jbe", "jna"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jae", "jnb", "ja", "jnbe"):
            return "1", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = "(int32_t)" + lhs
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    if flag_setter in ("bsf", "bsr"):
        if rhs is None:
            return None
        if jcc in ("je", "jz"):
            return f"({rhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({rhs} != 0)", desc
        return None

    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── cmpxchg: compares accumulator with dest, sets ZF on match ──
    if flag_setter == "cmpxchg":
        if jcc in ("je", "jz"):
            return f"({lhs} == eax)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != eax)", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter == "xadd":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        return None

    # ── repe cmpsb / repne scasb: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "1 /* strings matched (repe cmpsb) */", desc
        if jcc in ("jne", "jnz"):
            return "0 /* strings differed (repe cmpsb) */", desc
        return None

    return None


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        # Conditional tail call via the fused cmp/test+jcc path (the most
        # common shape for this pattern -- see the matching, rarer bug in
        # _lift_jcc for the standalone-jcc case, both fixed together).
        # Needs the same `g_seh_ebp = ebp;` bridge as an unconditional tail
        # jmp (_lift_jmp): missing it here means the target function's own
        # `ebp = g_seh_ebp;` seed reads whatever unrelated, older frame
        # g_seh_ebp last held instead of this function's actual frame.
        name = lifter._call_target_name(target)
        return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    result = _make_condition(second.mnemonic, first.mnemonic, first.operands)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24\x10")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prolog_addr, epilog_addr); either may be None if not found, which
    is normal for a title whose CRT does not use them.
    """
    from .config import va_to_file_offset

    prolog = epilog = None

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (prolog is None and size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prolog = addr
        elif (epilog is None and size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilog = addr

        if prolog is not None and epilog is not None:
            break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prolog)}, "
              f"__SEH_epilog {fmt(epilog)}", file=sys.stderr)

    return prolog, epilog


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.SEH_PROLOG = seh_prolog
        self.SEH_EPILOG = seh_epilog

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        names = _tree_names()
        if addr in names:
            name = names[addr]
        elif addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)
        if m in ("pushal", "pushad"):
            # Emitted as a TODO until part 183, so the popal that pairs with
            # it never restored anything: sub_001033D0 (board shadow
            # silhouettes) kept a clobbered ebp after its SSE loop, read its
            # object from the wrong frame and wrote a byte per triangle
            # through wild pointers every frame -- onto whatever the heap put
            # there, a rider's mesh part table once the free list worked.
            return ["{ uint32_t _sp = esp; PUSH32(esp, eax); PUSH32(esp, ecx); "
                    "PUSH32(esp, edx); PUSH32(esp, ebx); PUSH32(esp, _sp); "
                    "PUSH32(esp, ebp); PUSH32(esp, esi); PUSH32(esp, edi); } /* pushal */"]
        if m in ("popal", "popad"):
            return ["{ uint32_t _dead; POP32(esp, edi); POP32(esp, esi); POP32(esp, ebp); "
                    "POP32(esp, _dead); POP32(esp, ebx); POP32(esp, edx); POP32(esp, ecx); "
                    "POP32(esp, eax); (void)_dead; } /* popal */"]

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw"):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]
        if m == "in":
            return self._lift_in(insn, ops)
        if m == "out":
            return self._lift_out(insn, ops)

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            # The direction flag, tracked statically within the function:
            # both std sites in SSX are straight-line std / string op / cld
            # (strrchr, memmove's backward copy), and emitting them as
            # comments made both run forwards (part 183 -- every hard-disk
            # save failed). Blocks are lifted in address order, so this is
            # exact for that idiom.
            self._df_state()
            self._df = (m == "std")
            return [f"/* {m} - direction flag {'set' if self._df else 'clear'} */"]
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            return ["/* emms - empty MMX state */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE / MMX ──
        if m in _SSE_DISPATCH:
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Unhandled ──
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
        expr = f"{dst} {c_op} {src}"
        return [_fmt_operand_write(ops[0], expr)]

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})")]

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        return [_fmt_operand_write(ops[0], f"{dst} - {src} - _cf") + " /* sbb */"]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], f"{dst} + {src} + _cf") + " /* adc */"]

    def _lift_in(self, insn, ops):
        """IN AL/AX/EAX, DX  or  IN AL/AX/EAX, imm8 -- x86 port I/O read.

        Found unhandled (silently left as a `/* TODO: in ... */` comment,
        with the destination register never touched) while tracing SSX
        Tricky's D3D device-init chain into real hardware bring-up code --
 Unlike
        MEM32/MEM8 (plain memory, correctly emulated by mapping real pages),
        x86 port I/O has no address-space representation at all in this
        runtime, so it needs its own primitive: XBOX_IO_READ8/16/32(port),
        dispatched at runtime (xbox_io_port_read in kernel_bridge.c) to
        whatever that specific port actually means on real Xbox hardware --
        confirmed against xemu's port-0x80C0 (ACPI GPIO block) handling for
        the one call site this was found through, defaulting to 0 for any
        port without a specific implementation (the same "nothing here"
        default real unpopulated I/O space reads as).
        """
        if len(ops) < 2:
            return [f"/* in: bad operands */"]
        dst_reg = ops[0].reg if ops[0].type == "reg" else None
        port_expr = _fmt_operand_read(ops[1])
        if dst_reg == "al":
            return [f"SET_LO8(eax, XBOX_IO_READ8({port_expr})); /* in al, ... */"]
        if dst_reg == "ax":
            return [f"SET_LO16(eax, XBOX_IO_READ16({port_expr})); /* in ax, ... */"]
        return [f"eax = XBOX_IO_READ32({port_expr}); /* in eax, ... */"]

    def _lift_out(self, insn, ops):
        """OUT DX/imm8, AL/AX/EAX -- x86 port I/O write. See _lift_in."""
        if len(ops) < 2:
            return [f"/* out: bad operands */"]
        port_expr = _fmt_operand_read(ops[0])
        src_reg = ops[1].reg if ops[1].type == "reg" else None
        if src_reg == "al":
            return [f"XBOX_IO_WRITE8({port_expr}, LO8(eax)); /* out ..., al */"]
        if src_reg == "ax":
            return [f"XBOX_IO_WRITE16({port_expr}, LO16(eax)); /* out ..., ax */"]
        return [f"XBOX_IO_WRITE32({port_expr}, eax); /* out ..., eax */"]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        if ops[2].type == "imm":
            n = ops[2].imm & 31
            if n == 0:
                return ["/* shld by 0: no change */"]
            return [_fmt_operand_write(ops[0],
                f"({dst} << {n}) | ({src} >> {32 - n})") + " /* shld */"]
        # x86 masks the count to 5 bits and a count of 0 changes nothing;
        # `src >> (32 - 0)` is undefined in C and on x86 shifts by 0, which
        # ORed the whole source in (__aullshr by 0 turned the save
        # folder 201120EF6C64 into ...6C65, so every hard-disk save failed).
        return [f"{{ uint32_t _n = (uint32_t)({cnt}) & 31u; if (_n) "
                + _fmt_operand_write(ops[0], f"({dst} << _n) | ({src} >> (32u - _n))")
                + " } /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        if ops[2].type == "imm":
            n = ops[2].imm & 31
            if n == 0:
                return ["/* shrd by 0: no change */"]
            return [_fmt_operand_write(ops[0],
                f"({dst} >> {n}) | ({src} << {32 - n})") + " /* shrd */"]
        # See _lift_shld: a count of 0 must change nothing.
        return [f"{{ uint32_t _n = (uint32_t)({cnt}) & 31u; if (_n) "
                + _fmt_operand_write(ops[0], f"({dst} >> _n) | ({src} << (32u - _n))")
                + " } /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], f"{dst} {c_op} {cnt}")]

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        # An arithmetic shift copies the operand's own sign bit: bit 7 of a
        # byte, bit 15 of a word. (int32_t) of a byte read is never negative,
        # so `sar cl, 1` shifted in zeros.
        st = {1: "int8_t", 2: "int16_t"}.get(_op_size(ops[0]) or 4, "int32_t")
        return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t)({st}){dst} >> {cnt})")
                if st != "int32_t" else
                _fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})")]

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        width = {1: "8", 2: "16"}.get(_op_size(ops[0]) or 4, "32")
        func = ("ROL" if m == "rol" else "ROR") + width
        return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

    # ── Compare / Test (standalone) ──

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        return [f"(void)0; /* cmp {lhs}, {rhs} - flags set for next jcc */"]

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        return [f"(void)0; /* test {lhs}, {rhs} - flags set for next jcc */"]

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = None
    SEH_EPILOG = None

    def _lift_call(self, insn, ops):
        # x86 'call' pushes return address then jumps.
        # With global esp, we push a dummy return address (0) then call.
        # The callee's 'ret' will pop it back off.
        if insn.call_target:
            name = self._call_target_name(insn.call_target)
            if name == "CRT_ftol_TruncateToInt64":
                # The helper converts ST(0), which lives in the caller's
                # simulated x87 stack; it reads it through g_ftol_arg
                # (recomp_types.h). Every call in the tree carries this, added
                # by a post-pass the lifter never learned.
                lines = [f"PUSH32(esp, 0); g_ftol_arg = fp_top(); {name}(); "
                         f"/* call 0x{insn.call_target:08X} */"]
            else:
                lines = [f"PUSH32(esp, 0); {name}(); /* call 0x{insn.call_target:08X} */"]
            # After __SEH_prolog/__SEH_epilog, read back the frame pointer.
            if insn.call_target in (self.SEH_PROLOG, self.SEH_EPILOG):
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            return [f"PUSH32(esp, 0); RECOMP_ICALL_SAFE({target}, _icall_esp); /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if self.func_start in (self.SEH_PROLOG, self.SEH_EPILOG):
            prefix = "g_seh_ebp = ebp; "
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Detect if an indirect jmp operand is an intra-function switch table.
        Pattern: jmp [reg*scale + table_base] or jmp [reg + table_base]
        Returns (targets: list[int]) if ALL table entries are within the current
        function, else empty list."""
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        table_va = op.mem_disp
        targets = self._read_jump_table(table_va)
        if not targets:
            return []
        # Check that ALL targets are within the current function
        if all(self.func_start <= t < self.func_end for t in targets):
            return targets
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                name = self._call_target_name(insn.jump_target)
                return [f"g_seh_ebp = ebp; {name}(); return; /* tail jmp 0x{insn.jump_target:08X} */"]
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    # Conditional tail call. Same "bridge ebp so the target
                    # can inherit our frame pointer" requirement as the
                    # unconditional case in _lift_jmp -- missing here was a
                    # real bug (found on SSX Tricky): sub_0017FE15's own
                    # `push ebp; mov ebp, esp` prologue frame never reached
                    # sub_0017FE66 through this exact conditional-tail-call
                    # path, so sub_0017FE66's `ebp = g_seh_ebp;` (itself a
                    # separate, now-fixed bug -- see the has_prologue seeding
                    # fix in translator.py) read whatever g_seh_ebp was last
                    # set to by a completely unrelated, much older frame.
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ g_seh_ebp = ebp; {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc
        if target:
            if self._is_external_target(target):
                # Same missing-ebp-bridge bug as the jecxz/jcxz case above.
                name = self._call_target_name(target)
                return [f"if (_flags /* {jcc}: {desc} */) {{ g_seh_ebp = ebp; {name}(); return; }}"]
            return [f"if (_flags /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _df_state(self):
        """Direction flag for the current function (reset at each new one)."""
        if getattr(self, "_df_func", None) != self.func_start:
            self._df_func = self.func_start
            self._df = False
        return self._df

    def _lift_rep_string(self, insn, m):
        if self._df_state():
            # Descending (std): element by element from the given addresses
            # down, as the hardware does; overlapping ranges stay correct.
            for op, sz, mem in (("movsb", 1, "MEM8"), ("movsw", 2, "MEM16"), ("movsd", 4, "MEM32")):
                if op in m:
                    return [f"{{ uint32_t _i; for (_i = 0; _i < ecx; _i++) {mem}(edi - _i*{sz}) = {mem}(esi - _i*{sz}); }}",
                            f"esi -= ecx * {sz}; edi -= ecx * {sz}; ecx = 0; /* std; rep {op} */"]
            for op, sz, mem, val in (("stosb", 1, "MEM8", "LO8(eax)"), ("stosw", 2, "MEM16", "LO16(eax)"),
                                     ("stosd", 4, "MEM32", "eax")):
                if op in m:
                    return [f"{{ uint32_t _i; for (_i = 0; _i < ecx; _i++) {mem}(edi - _i*{sz}) = {val}; }}",
                            f"edi -= ecx * {sz}; ecx = 0; /* std; rep {op} */"]
            return [f"/* TODO: {m} with the direction flag set */"]
        if "movsb" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx);",
                    "esi += ecx; edi += ecx; ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4);",
                    "esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 2);",
                    "esi += ecx * 2; edi += ecx * 2; ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            return ["memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);",
                    "edi += ecx; ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*2) = LO16(eax); }",
                "edi += ecx * 2; ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m or "cmpsw" in m or "cmpsd" in m:
            return [f"/* {m} - string compare, ecx iterations */"]
        if "scasb" in m or "scasw" in m or "scasd" in m:
            return [f"/* {m} - string scan, ecx iterations */"]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if self._df_state():
            sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
            mem = {1: "MEM8", 2: "MEM16", 4: "MEM32"}[sz]
            reg = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}[sz]
            if m.startswith("movs"):
                return [f"{mem}(edi) = {mem}(esi); esi -= {sz}; edi -= {sz}; /* std; {m} */"]
            if m.startswith("stos"):
                return [f"{mem}(edi) = {reg}; edi -= {sz}; /* std; {m} */"]
            if m.startswith("lods"):
                load = {1: "SET_LO8(eax, MEM8(esi))", 2: "SET_LO16(eax, MEM16(esi))", 4: "eax = MEM32(esi)"}[sz]
                return [f"{load}; esi -= {sz}; /* std; {m} */"]
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi++; edi++; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4; edi += 4; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi++; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi++; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4; /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += 2; edi += 2; /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += 2; /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += 2; /* lodsw */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations."""
        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        # XMM registers are recomp_xmm_t (recomp_types.h): .f low float lane,
        # .d low double, .l[4]/.u[4] packed lanes, .x all 16 bytes. They used
        # to be declared `float`, which moved 4 of 16 bytes on every movaps and
        # had no packed arithmetic at all; part 47 fixed the generated tree by
        # a one-off transform, but the lifter kept emitting the old model, so
        # every function recovered afterwards was broken again.
        dbl = m.endswith("sd") or m.endswith("pd") or m in ("comisd", "ucomisd")
        lane = "d" if dbl else "f"

        def _is_x(op):
            return op.type == "reg" and op.reg.startswith("xmm")

        def _sse_read(op, ln=None):
            ln = ln or lane
            if op.type == "reg":
                return f"{op.reg}.{ln}" if op.reg.startswith("xmm") else op.reg
            elif op.type == "mem":
                if ln == "d" or op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val, ln=None):
            ln = ln or lane
            if op.type == "reg":
                return (f"{op.reg}.{ln} = {val};" if op.reg.startswith("xmm")
                        else f"{op.reg} = {val};")
            elif op.type == "mem":
                if ln == "d" or op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        def _x128(op):
            """A whole 128-bit operand as recomp_xmm_t."""
            if _is_x(op):
                return op.reg
            if op.type == "mem":
                return f"XMMM({_fmt_mem(op)})"
            return "/* x128? */"

        # ── MMX moves ──
        #
        # `movq` between an MMX register and memory was falling through to the
        # generic "/* SSE: ... */" comment, i.e. it did nothing. The MMX
        # register file (mm0-mm7 as uint64_t) and the arithmetic helpers were
        # already there, so the loads and the *store* were the only missing
        # piece -- and dropping the store is what makes an MMX loop read
        # nothing, compute nothing and write nothing while the scalar code
        # around it advances its pointers as though it had.
        #
        # 100 sites in this tree, 45 of them in the five functions that do the
        # video's YUV-to-RGB conversion.
        def _is_mm(op):
            return op.type == "reg" and op.reg.startswith("mm")                 and not op.reg.startswith("mmx")

        if m in ("movq", "movntq") and nops >= 2:
            dst, src = ops[0], ops[1]
            if _is_mm(dst) and src.type == "mem":
                return [f"{dst.reg} = MEM64({_fmt_mem(src)}); /* {m} */"]
            if dst.type == "mem" and _is_mm(src):
                return [f"MEM64({_fmt_mem(dst)}) = {src.reg}; /* {m} */"]
            if _is_mm(dst) and _is_mm(src):
                return [f"{dst.reg} = {src.reg}; /* {m} */"]
            # movq between xmm registers, or xmm<->mem: the 64-bit low half.
            if dst.type == "reg" and src.type == "mem"                     and dst.reg.startswith("xmm"):
                return [f"memcpy(&{dst.reg}, XBOX_PTR({_fmt_mem(src)}), 8);"
                        f" /* {m} */"]
            if dst.type == "mem" and src.type == "reg"                     and src.reg.startswith("xmm"):
                return [f"memcpy(XBOX_PTR({_fmt_mem(dst)}), &{src.reg}, 8);"
                        f" /* {m} */"]

        # MMX bitwise ops.  The mm registers are plain uint64_t in the generated
        # code, so these are ordinary integer operations and there was never a
        # reason to drop them.  All 8 remaining sites (4 pand, 4 por) sit in the
        # video's YUV-to-RGB converters -- sub_00149450/500/5E0/670 -- where the
        # mask and the merge are what assemble each output pixel, so dropping
        # them silently produces garbage for every frame.
        # MMX packed integer arithmetic and packing. These were emitted as bare
        # `TODO` comments, which is silent data loss rather than a missing
        # nicety: in the video's YUV-to-RGB row converter (sub_001493E0) the two
        # dropped `paddw`s meant chroma was never added to luma and the dropped
        # `packuswb` meant 16-bit table entries were stored unpacked, producing
        # a luminance-only picture with the chroma bytes left at zero.
        _MMX_BIN = {
            "paddb": "mmx_paddb", "paddw": "mmx_paddw", "paddd": "mmx_paddd",
            "psubb": "mmx_psubb", "psubw": "mmx_psubw", "psubd": "mmx_psubd",
            "packuswb": "mmx_packuswb", "packsswb": "mmx_packsswb",
            "packssdw": "mmx_packssdw",
            # Saturating adds. paddusb is what applies the per-channel bias to
            # an already-packed pixel in the YUV converters, and the saturation
            # is the point -- without it a bright channel wraps to black.
            "paddusb": "mmx_paddusb", "paddusw": "mmx_paddusw",
            "psubusb": "mmx_psubusb", "psubusw": "mmx_psubusw",
            "paddsb": "mmx_paddsb", "psubsb": "mmx_psubsb",
            "paddsw": "mmx_paddsw", "psubsw": "mmx_psubsw",
            # Packed multiply. pmaddwd folds an RGB888 triple onto the bit
            # positions of RGB565 in one instruction.
            "pmaddwd": "mmx_pmaddwd", "pmullw": "mmx_pmullw",
            "pmulhw": "mmx_pmulhw", "pmulhuw": "mmx_pmulhuw",
            # Unpack / interleave / average, used by motion compensation.
            "punpcklbw": "mmx_punpcklbw", "punpckhbw": "mmx_punpckhbw",
            "punpcklwd": "mmx_punpcklwd", "punpckhwd": "mmx_punpckhwd",
            "punpckldq": "mmx_punpckldq", "punpckhdq": "mmx_punpckhdq",
            "pavgb": "mmx_pavgb", "pavgw": "mmx_pavgw",
            # Compare / min / max / SAD.
            "pcmpeqb": "mmx_pcmpeqb", "pcmpeqw": "mmx_pcmpeqw",
            "pcmpeqd": "mmx_pcmpeqd", "pcmpgtb": "mmx_pcmpgtb",
            "pcmpgtw": "mmx_pcmpgtw", "pcmpgtd": "mmx_pcmpgtd",
            "pminub": "mmx_pminub", "pmaxub": "mmx_pmaxub",
            "pminsw": "mmx_pminsw", "pmaxsw": "mmx_pmaxsw",
            "psadbw": "mmx_psadbw",
        }
        if m in _MMX_BIN and nops >= 2:
            dst, src = ops[0], ops[1]
            if _is_mm(dst):
                if _is_mm(src):
                    rhs = src.reg
                elif src.type == "mem":
                    rhs = f"MEM64({_fmt_mem(src)})"
                else:
                    rhs = None
                if rhs is not None:
                    fn = _MMX_BIN[m]
                    return [f"{dst.reg} = {fn}({dst.reg}, {rhs}); /* {m} */"]

        # Packed shifts. Unlike the arithmetic above these take an immediate
        # count as often as a register one, which is why they were missed by
        # the reg/mem-only test and left as TODO comments. In the YUV-to-RGB
        # converters the `psrld mm1, 0xb` is what moves the red channel down to
        # its RGB565 bit position and the `psrlq mm0, 0x10` is what brings the
        # second pixel into the low half before the store -- dropping either
        # leaves the packed pixel scrambled rather than merely dimmed.
        _MMX_SHIFT = {
            "psrlw": "mmx_psrlw", "psrld": "mmx_psrld", "psrlq": "mmx_psrlq",
            "psllw": "mmx_psllw", "pslld": "mmx_pslld", "psllq": "mmx_psllq",
            "psraw": "mmx_psraw", "psrad": "mmx_psrad",
        }
        if m in _MMX_SHIFT and nops >= 2:
            dst, src = ops[0], ops[1]
            if _is_mm(dst):
                if src.type == "imm":
                    rhs = f"{src.imm & 0xFF}u"
                elif _is_mm(src):
                    rhs = src.reg
                elif src.type == "mem":
                    rhs = f"MEM64({_fmt_mem(src)})"
                else:
                    rhs = None
                if rhs is not None:
                    fn = _MMX_SHIFT[m]
                    return [f"{dst.reg} = {fn}({dst.reg}, {rhs}); /* {m} */"]

        if m in ("pand", "pandn", "por", "pxor") and nops >= 2:
            dst, src = ops[0], ops[1]
            if _is_mm(dst):
                if _is_mm(src):
                    rhs = src.reg
                elif src.type == "mem":
                    rhs = f"MEM64({_fmt_mem(src)})"
                else:
                    rhs = None
                if rhs is not None:
                    if m == "pand":
                        return [f"{dst.reg} &= {rhs}; /* {m} */"]
                    if m == "por":
                        return [f"{dst.reg} |= {rhs}; /* {m} */"]
                    if m == "pxor":
                        return [f"{dst.reg} ^= {rhs}; /* {m} */"]
                    # pandn dst, src  ==  dst = (~dst) & src
                    return [f"{dst.reg} = (~{dst.reg}) & {rhs}; /* {m} */"]

        # ── Moves ──
        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu",
                 "movntps", "movntdq") and nops >= 2:
            d, sr = ops[0], ops[1]
            if _is_x(d) and _is_x(sr):
                return [f"{d.reg} = {sr.reg}; /* {m} */"]
            if _is_x(d) and sr.type == "mem":
                return [f"{d.reg}.x = MEMX({_fmt_mem(sr)}); /* {m} */"]
            if d.type == "mem" and _is_x(sr):
                return [f"MEMX({_fmt_mem(d)}) = {sr.reg}.x; /* {m} */"]
        if m in ("movss", "movsd") and nops >= 2:
            d, sr = ops[0], ops[1]
            if _is_x(d) and sr.type == "mem":
                # a load clears the lanes above the scalar, a register move does not
                hi = "u[1] = {r}.u[2] = {r}.u[3]" if lane == "f" else "u[2] = {r}.u[3]"
                return [f"{d.reg}." + hi.format(r=d.reg) + f" = 0; {_sse_write(d, _sse_read(sr))} /* {m} */"]
            return [_sse_write(d, _sse_read(sr)) + f" /* {m} */"]
        if m in ("movlps", "movlpd", "movhps", "movhpd") and nops >= 2:
            d, sr = ops[0], ops[1]
            half = "0" if m in ("movlps", "movlpd") else "2"
            if _is_x(d) and sr.type == "mem":
                return [f"memcpy(&{d.reg}.u[{half}], XBOX_PTR({_fmt_mem(sr)}), 8); /* {m} */"]
            if d.type == "mem" and _is_x(sr):
                return [f"memcpy(XBOX_PTR({_fmt_mem(d)}), &{sr.reg}.u[{half}], 8); /* {m} */"]
        if m in ("movlhps", "movhlps") and nops >= 2 and _is_x(ops[0]) and _is_x(ops[1]):
            return [f"XMM_{m.upper()}({ops[0].reg}, {ops[1].reg}); /* {m} */"]
        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps"):
            return [f"/* {m} {insn.op_str} */"]

        if m == "movd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1]) if ops[1].type != "reg" or not ops[1].reg.startswith("xmm") else _sse_read(ops[1])
                if ops[0].type == "reg" and ops[0].reg.startswith("xmm"):
                    return [f"memcpy(&{ops[0].reg}, &{src}, 4); /* movd to xmm */"]
                else:
                    return [f"{_fmt_operand_write(ops[0], src)} /* movd */"]
            return [f"/* movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        if m in ("addps", "subps", "mulps", "divps"):
            if nops >= 2:
                c_op = {"addps": "+", "subps": "-", "mulps": "*", "divps": "/"}[m]
                return [f"XMM_BINOP({ops[0].reg}, {_x128(ops[1])}, {c_op}=); /* {m} */"]

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1], 'f')}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1], 'f')}", "d") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1], 'd')}", "f") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                return [f"/* {m} {_sse_read(ops[0])}, {_sse_read(ops[1])} - sets EFLAGS */"]

        # ── Bitwise ──
        if m in ("xorps", "xorpd", "andps", "andpd", "orps", "orpd", "andnps", "andnpd") and nops >= 2:
            d = ops[0].reg
            if m in ("xorps", "xorpd") and _is_x(ops[1]) and ops[1].reg == d:
                return [f"memset(&{d}, 0, sizeof({d})); /* {m} self = zero */"]
            if m in ("andnps", "andnpd"):
                return [f"XMM_ANDNPS({d}, {_x128(ops[1])}); /* {m} */"]
            bop = {"x": "^", "a": "&", "o": "|"}[m[0]]
            return [f"{{ recomp_xmm_t _s_ = {_x128(ops[1])}; int _i_; "
                    f"for (_i_ = 0; _i_ < 4; _i_++) {d}.u[_i_] {bop}= _s_.u[_i_]; }} /* {m} */"]

        # ── Packed min/max ──
        if m in ("minps", "maxps"):
            if nops >= 2:
                return [f"XMM_{m.upper()}({ops[0].reg}, {_x128(ops[1])}); /* {m} */"]

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        # The SSE model here tracks only the low lane as a single float, so
        # these compute the low lane like their scalar ...ss forms rather than
        # all four. That is the same low-lane approximation the packed
        # arithmetic ops (addps/mulps) already use -- but computing the low lane
        # is strictly better than the TODO no-op these used to hit, which left
        # the destination stale and fed garbage into vector normalisation.
        # rsqrtps/sqrtps are the workhorse of 3D vector normalize; Wreckless
        # uses them heavily, Burnout 3 did not, which is why this surfaced now.
        if m in ("sqrtps", "rsqrtps", "rcpps") and nops >= 2:
            f = {"sqrtps": "sqrtf(_s_.l[_i_])", "rsqrtps": "1.0f / sqrtf(_s_.l[_i_])",
                 "rcpps": "1.0f / _s_.l[_i_]"}[m]
            return [f"{{ recomp_xmm_t _s_ = {_x128(ops[1])}; int _i_; "
                    f"for (_i_ = 0; _i_ < 4; _i_++) {ops[0].reg}.l[_i_] = {f}; }} /* {m} */"]

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps", "cmpnltps", "cmpnleps"):
            if nops >= 2:
                c = {"cmpneqps": "!=", "cmpeqps": "==", "cmpltps": "<", "cmpleps": "<=",
                     "cmpnltps": ">=", "cmpnleps": ">"}[m]
                return [f"XMM_CMPPS({ops[0].reg}, {_x128(ops[1])}, {c}); /* {m} */"]

        # ── Move mask ──
        if m == "movmskps":
            if nops >= 2 and _is_x(ops[1]):
                r = ops[1].reg
                return [_fmt_operand_write(ops[0], f"(({r}.u[0] >> 31) | (({r}.u[1] >> 31) << 1)"
                        f" | (({r}.u[2] >> 31) << 2) | (({r}.u[3] >> 31) << 3))") + " /* movmskps */"]

        # ── MMX / integer SIMD ──
        if m in ("pand", "pandn", "por", "pxor", "pcmpgtd"):
            if nops >= 2:
                return [f"/* {m} {insn.op_str} (MMX/SIMD integer) */"]

        # ── Shuffle/unpack ──
        if m == "shufps" and nops >= 3 and ops[2].type == "imm":
            return [f"XMM_SHUFPS({ops[0].reg}, {_x128(ops[1])}, 0x{ops[2].imm & 0xFF:x}); /* shufps */"]
        if m in ("unpcklps", "unpckhps") and nops >= 2:
            return [f"XMM_{m.upper()}({ops[0].reg}, {_x128(ops[1])}); /* {m} */"]
        if m in ("shufps", "unpcklps", "unpckhps"):
            return [f"/* {m} {insn.op_str} */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    def _lift_fpu(self, insn, m, ops):
        """x87 -> C over the simulated register file (fp_push/fp_pop/fp_top/
        fp_st1 and _fp_stack[(_fp_top + i) & 7] for st(i)).

        Rewritten in part 179. The old version collapsed every fadd/fsub/fmul/
        fdiv to the popping two-register form, popped on `fst`, did not pop on
        `fistp`/`fcomp`/`fcompp`, compared a memory `fcom` against st(1), and
        left fld/fstp st(i), fsubr/fdivr, the integer forms, fsin/fcos and
        fnstsw as comments. The generated tree was corrected afterwards by a
        series of post-passes (fixfpmem, fixfpudrop, fixfpbranch, ...) that
        nothing recovered later ever received. Semantics are Intel's.
        """
        def st(i):
            if i == 0:
                return "fp_top()"
            if i == 1:
                return "fp_st1()"
            return f"_fp_stack[(_fp_top + {i}) & 7]"

        def st_index(op):
            if op.type == "reg" and op.reg and op.reg.startswith("st"):
                r = op.reg.replace("(", "").replace(")", "")
                return int(r[2:]) if len(r) > 2 else 0
            return None

        def mem_float(op):
            if op.mem_size == 8:
                return f"MEMD({_fmt_mem(op)})"
            if op.mem_size == 4:
                return f"(double)MEMF({_fmt_mem(op)})"
            return None

        def mem_int(op):
            if op.mem_size == 2:
                return f"(double)SMEM16({_fmt_mem(op)})"
            if op.mem_size == 4:
                return f"(double)SMEM32({_fmt_mem(op)})"
            if op.mem_size == 8:
                return f"(double)(int64_t)MEM64({_fmt_mem(op)})"
            return None

        c = f"/* {m} {insn.op_str} */".replace("  */", " */")
        nops = len(ops)

        # ── loads ──
        if m == "fld" and nops >= 1:
            if ops[0].type == "mem":
                if ops[0].mem_size == 4:
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                if ops[0].mem_size == 8:
                    return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                if ops[0].mem_size == 10:
                    return [f"fp_push(x87_load80(XBOX_PTR({_fmt_mem(ops[0])}))); {c}"]
            i = st_index(ops[0])
            if i is not None:
                return [f"{{ double _v = {st(i)}; fp_push(_v); }} {c}"]
        if m == "fild" and nops >= 1 and ops[0].type == "mem":
            if ops[0].mem_size in (2, 4):
                return [f"fp_push((double){_smem_accessor(ops[0].mem_size)}({_fmt_mem(ops[0])})); /* fild */"]
            v = mem_int(ops[0])
            if v:
                return [f"fp_push({v}); {c}"]
        consts = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.14159265358979323846",
                  "fldl2e": "1.44269504088896340736", "fldl2t": "3.32192809488736234787",
                  "fldlg2": "0.301029995663981195214", "fldln2": "0.693147180559945309417"}
        if m in consts:
            return [f"fp_push({consts[m]}); {c}"]

        # ── stores ──
        if m in ("fst", "fstp") and nops >= 1:
            if ops[0].type == "mem" and ops[0].mem_size == 10:
                pop = " fp_pop();" if m == "fstp" else ""
                return [f"x87_store80(XBOX_PTR({_fmt_mem(ops[0])}), fp_top());{pop} {c}"]
            if ops[0].type == "mem" and ops[0].mem_size in (4, 8):
                acc = "MEMF" if ops[0].mem_size == 4 else "MEMD"
                cast = "(float)" if ops[0].mem_size == 4 else ""
                if m == "fstp":
                    return [f"{acc}({_fmt_mem(ops[0])}) = {cast}fp_top(); fp_popp(); /* fstp */"]
                return [f"{acc}({_fmt_mem(ops[0])}) = {cast}fp_top(); "
                        f"/* fst: stores st(0), does NOT pop */"]
            i = st_index(ops[0])
            if i is not None:
                if m == "fst":
                    return [f"{st(i)} = fp_top(); {c}"]
                if i == 0:
                    return [f"fp_pop(); {c}"]
                return [f"{{ double _v = fp_top(); fp_pop(); _fp_stack[(_fp_top + {i - 1}) & 7] = _v; }} {c}"]
        if m in ("fist", "fistp", "fisttp") and nops >= 1 and ops[0].type == "mem":
            pop = " fp_pop();" if m != "fist" else ""
            conv = "trunc(fp_top())" if m == "fisttp" else "x87_rint(fp_top())"
            sz = ops[0].mem_size
            if sz == 8:
                return [f"MEM64({_fmt_mem(ops[0])}) = (uint64_t)(int64_t){conv};{pop} {c}"]
            if sz == 4:
                return [f"MEM32({_fmt_mem(ops[0])}) = (uint32_t)(int32_t){conv};{pop} {c}"]
            if sz == 2:
                return [f"MEM16({_fmt_mem(ops[0])}) = (uint16_t)(int16_t){conv};{pop} {c}"]

        # ── arithmetic (port of tools/audit/fixfpmem.py build()) ──
        base = m
        popping = base.endswith("p") and base in ("faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp")
        if popping:
            base = base[:-1]
        reverse = base in ("fsubr", "fdivr", "fisubr", "fidivr")
        core = base[:-1] if reverse else base
        sym = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/",
               "fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/"}.get(core)
        if sym is not None:
            if nops == 1 and ops[0].type == "mem":
                v = mem_int(ops[0]) if core.startswith("fi") else mem_float(ops[0])
                if v:
                    e = f"{v} {sym} fp_top()" if reverse else f"fp_top() {sym} {v}"
                    return [f"fp_top() = {e}; {c}"]
            regs = [st_index(o) for o in ops]
            if nops == 0:
                regs = [1]                      # faddp == faddp st(1), st(0)
            if all(r is not None for r in regs):
                if len(regs) == 2 and regs[1] == 0 and regs[0] != 0:
                    dst, src = regs[0], 0       # st(i), st(0)
                elif len(regs) == 2:
                    dst, src = 0, regs[1]       # st(0), st(i)
                elif popping:
                    dst, src = regs[0], 0       # fop p st(i)  ==  st(i), st(0)
                else:
                    dst, src = 0, regs[0]       # fop st(i)    ==  st(0), st(i)
                if popping and dst == 1 and src == 0 and not reverse:
                    return [f"fp_st1() {sym}= fp_top(); fp_pop(); /* {m} */"]
                e = f"{st(src)} {sym} {st(dst)}" if reverse else f"{st(dst)} {sym} {st(src)}"
                return [f"{st(dst)} = {e};{' fp_pop();' if popping else ''} {c}"]

        # ── one-operand and constant-free ops ──
        unary = {"fchs": "-fp_top()", "fabs": "fabs(fp_top())", "fsqrt": "sqrt(fp_top())",
                 "fsin": "sin(fp_top())", "fcos": "cos(fp_top())",
                 "frndint": "x87_rint(fp_top())", "f2xm1": "(pow(2.0, fp_top()) - 1.0)"}
        if m in unary:
            return [f"fp_top() = {unary[m]}; {c}"]
        if m == "fsincos":
            return [f"{{ double _s = sin(fp_top()), _c = cos(fp_top()); fp_top() = _s; fp_push(_c); }} {c}"]
        if m == "fptan":
            return [f"fp_top() = tan(fp_top()); fp_push(1.0); {c}"]
        if m == "fpatan":
            return [f"fp_st1() = atan2(fp_st1(), fp_top()); fp_pop(); {c}"]
        if m == "fscale":
            return [f"fp_top() = fp_top() * pow(2.0, trunc(fp_st1())); {c}"]
        if m == "fprem":
            return [f"fp_top() = fmod(fp_top(), fp_st1()); {c}"]
        if m == "fprem1":
            return [f"fp_top() = remainder(fp_top(), fp_st1()); {c}"]
        if m == "fyl2x":
            return [f"fp_st1() = fp_st1() * log2(fp_top()); fp_pop(); {c}"]
        if m == "fyl2xp1":
            return [f"fp_st1() = fp_st1() * log2(fp_top() + 1.0); fp_pop(); {c}"]
        if m == "fxch":
            i = next((st_index(o) for o in ops if st_index(o)), 1)
            cx = "/* fxch */" if i == 1 else c
            return [f"{{ double _t = fp_top(); fp_top() = {st(i)}; {st(i)} = _t; }} {cx}"]

        # ── compares ──
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp", "ftst"):
            if m == "ftst":
                rhs = "0.0"
            elif nops >= 1 and ops[0].type == "mem":
                rhs = mem_int(ops[0]) if m.startswith("fi") else mem_float(ops[0])
            else:
                i = next((st_index(o) for o in ops if st_index(o)), 1)
                rhs = st(i)
            pops = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            if rhs == "fp_st1()" and pops == 0 and m in ("fcom", "fucom"):
                return [f"_fpu_cmp = (fp_top() < fp_st1()) ? -1 : (fp_top() > fp_st1()) ? 1 : 0;"
                        f" /* {m} {insn.op_str} */"]
            if rhs:
                return [f"{{ double _fc = {rhs}; _fpu_cmp = (fp_top() < _fc) ? -1 : (fp_top() > _fc) ? 1 : 0; "
                        f"g_fpu_cmp = _fpu_cmp;{' fp_pop();' * pops} }} {c}"]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF); the *ip forms pop.
            i = next((st_index(o) for o in ops if st_index(o)), 1)
            pops = m.endswith("pi") or m.endswith("ip")
            return [f"_fpu_cmp = (fp_top() < {st(i)}) ? -1 : (fp_top() > {st(i)}) ? 1 : 0;"
                    f"{' fp_pop();' if pops else ''} {c}"]
        if m in ("fnstsw", "fstsw"):
            if nops >= 1 and ops[0].type == "reg" and ops[0].reg == "ax":
                return [f"g_fpu_cmp = _fpu_cmp; FNSTSW_AX(eax); {c}"]
            if nops >= 1 and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = (uint16_t)(FPU_AH() << 8); {c}"]
        if m in ("fnstcw", "fstcw") and nops >= 1 and ops[0].type == "mem":
            return [f"MEM16({_fmt_mem(ops[0])}) = g_x87_cw; {c}"]
        if m == "fldcw" and nops >= 1 and ops[0].type == "mem":
            return [f"g_x87_cw = MEM16({_fmt_mem(ops[0])}); {c}"]
        if m == "fxam":
            # Classify st(0) into C3/C2/C1/C0 for the next fnstsw (x87_fxam).
            return [f"_fpu_cmp = g_fpu_cmp = x87_fxam(fp_top()); {c}"]
        if m in ("fnstcw", "fstcw", "fldcw", "fnclex", "fclex", "fninit", "finit",
                 "fwait", "wait", "fnop", "ffree", "fdecstp", "fincstp"):
            return [f"(void)0; {c}"]

        return [f"/* FPU: {m} {insn.op_str} */"]


# ── Deferred-flag operand snapshots ──────────────────────────
#
# Flags are not materialised. A flag-setting instruction emits nothing (cmp,
# test) or only its data effect (sub, and, dec, ...), and the consumer -- a
# jcc, setcc, cmovcc, sbb or adc -- rebuilds the condition from the setter's
# OPERANDS, formatted where the consumer is. That is only right if nothing in
# between rewrites those operands, and compiled code does exactly that all the
# time, because mov/pop/lea leave the flags alone:
#
#     cmp  [ebp-8], edi         ; edi == 0 here
#     pop  edi                  ; edi restored to the caller's value
#     pop  esi
#     je   skip                 ; hardware tests the cmp above
#
# was emitted as `if (CMP_EQ(MEM32(ebp + -8), edi))` after the pops -- a
# compare against the wrong edi. In DirectSound's lock release that sent
# KfLowerIrql a byte the lock never wrote (48, then 192), and the video path
# bugchecked IRQL_NOT_LESS_OR_EQUAL on it. The same class once broke async
# file loads and was patched by a one-off script over the generated C, which
# every relift since had undone.
#
# So: when something between a setter and a consumer in the same block writes
# a register or memory the setter's operands read, snapshot those operands
# into _fsa/_fsb at the setter -- before it for cmp/test (which do not change
# them), after it for result-based setters (whose conditions are built from the
# post-op destination) -- and give the consumers the snapshots. Snapshots keep
# the operand's width, because the CMP_* macros take signedness from sizeof().

_GP_FAMILY = {}
for _fam, _names in (("eax", ("al", "ah", "ax", "eax")),
                     ("ebx", ("bl", "bh", "bx", "ebx")),
                     ("ecx", ("cl", "ch", "cx", "ecx")),
                     ("edx", ("dl", "dh", "dx", "edx")),
                     ("esi", ("si", "esi")), ("edi", ("di", "edi")),
                     ("ebp", ("bp", "ebp")), ("esp", ("sp", "esp"))):
    for _n in _names:
        _GP_FAMILY[_n] = _fam
_GP_SIZE = {"al": 1, "ah": 1, "bl": 1, "bh": 1, "cl": 1, "ch": 1, "dl": 1,
            "dh": 1, "ax": 2, "bx": 2, "cx": 2, "dx": 2, "si": 2, "di": 2,
            "bp": 2, "sp": 2}

_SNAP_PRE = frozenset({"cmp", "test"})
_SNAP_POST = frozenset({"sub", "add", "and", "or", "xor", "inc", "dec", "neg",
                        "adc", "sbb"})
_SETCC = frozenset({"sete", "setne", "setb", "setae", "setbe", "seta", "setl",
                    "setge", "setle", "setg", "sets", "setns", "setz", "setnz",
                    "setc", "setnc", "setnae", "setnb", "setna", "setnbe",
                    "setnge", "setnl", "setng", "setnle"})
_CF_CONSUMERS = frozenset({"sbb", "adc", "rcl", "rcr"})
# Instructions whose first operand is read, not written.
_NO_DST_WRITE = frozenset({
    "cmp", "test", "bt", "nop", "push", "jmp", "prefetchnta", "prefetcht0",
    "prefetcht1", "prefetcht2", "comiss", "comisd", "ucomiss", "ucomisd",
    "fld", "fild", "fbld", "fcom", "fcomp", "fucom", "fucomp", "ficom",
    "ficomp", "fldcw", "fldenv", "frstor"})
_STRING_OPS = frozenset({"movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                         "lodsb", "lodsw", "lodsd", "scasb", "scasw", "scasd",
                         "cmpsb", "cmpsw", "cmpsd"})


def _op_size(op):
    if op.type == "reg":
        return _GP_SIZE.get(op.reg, 4)
    if op.type == "mem":
        return op.mem_size or 4
    if op.type == "snap":
        return op.snap_size
    return None


def _op_reads(op):
    """(GP register families, reads memory?) that evaluating `op` depends on."""
    regs = set()
    if op.type == "reg":
        f = _GP_FAMILY.get(op.reg)
        if f:
            regs.add(f)
        return regs, False
    if op.type == "mem":
        for r in (op.mem_base, op.mem_index):
            f = _GP_FAMILY.get(r) if r else None
            if f:
                regs.add(f)
        return regs, True
    return regs, False


def _insn_writes(ins):
    """Over-approximate (GP register families written, writes memory?).

    Over-approximating only costs an unneeded snapshot; missing a write is
    the bug this exists to prevent."""
    m = ins.mnemonic
    ops = ins.operands
    regs = set()
    mem = False

    def fam(op):
        return _GP_FAMILY.get(op.reg) if op.type == "reg" else None

    if m.startswith("rep") or m in _STRING_OPS:
        return {"esi", "edi", "ecx", "eax"}, True
    if m in ("push", "pushfd", "pushf", "pushad", "pushal"):
        return {"esp"}, True
    if m in ("pop", "popfd", "popf"):
        regs.add("esp")
        if ops and fam(ops[0]):
            regs.add(fam(ops[0]))
        if ops and ops[0].type == "mem":
            mem = True
        return regs, mem
    if m in ("popad", "popal"):
        return set(_GP_FAMILY.values()), False
    if m == "call":
        return {"eax", "ecx", "edx", "esp"}, True
    if m == "leave":
        return {"esp", "ebp"}, False
    if m == "enter":
        return {"esp", "ebp"}, True
    if m in ("cdq", "cwd"):
        return {"edx"}, False
    if m in ("cwde", "cbw", "lahf"):
        return {"eax"}, False
    if m in ("mul", "div", "idiv", "rdtsc") or (m == "imul" and len(ops) == 1):
        return {"eax", "edx"}, False
    if m == "cpuid":
        return {"eax", "ebx", "ecx", "edx"}, False
    if m in ("fnstsw", "fstsw") and (not ops or ops[0].type == "reg"):
        return {"eax"}, False
    if m in ("xchg", "xadd", "cmpxchg", "cmpxchg8b"):
        for op in ops:
            if fam(op):
                regs.add(fam(op))
            elif op.type == "mem":
                mem = True
        if m in ("cmpxchg", "cmpxchg8b"):
            regs |= {"eax", "edx"}
        return regs, mem
    if ops and m not in _NO_DST_WRITE and not m.startswith("j"):
        if fam(ops[0]):
            regs.add(fam(ops[0]))
        elif ops[0].type == "mem":
            mem = True
    return regs, mem


def _is_flag_consumer(ins):
    m = ins.mnemonic
    if ins.is_cond_jump:
        return m not in ("jecxz", "jcxz")
    return m in _SETCC or m.startswith("cmov") or m in _CF_CONSUMERS


def _sets_flags(ins):
    m = ins.mnemonic
    if m in FLAG_SETTERS or m in _EFLAGS_SETTERS or m in _FLAGS_UNDEFINED:
        return True
    if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi",
             "sahf", "popfd", "popf"):
        return True
    return m.startswith("rep") and ("cmps" in m or "scas" in m)


def _needs_flag_snapshot(insns, i, ops=None, live_out=True):
    """True if, between the flag setter insns[i] and a flag consumer, something
    writes what the setter's operands read.

    The consumer may be in a later block: a block that ends with the flags
    still live hands them to its successors, which rebuild the condition from
    the same operands. So reaching the end of the block with the operands
    overwritten needs a snapshot too.

    With `ops`, checks operands that arrived from a predecessor, from insns[i]
    onward (i itself included)."""
    reads_regs, reads_mem = set(), False
    for op in (ops if ops is not None else insns[i].operands)[:2]:
        r, mm = _op_reads(op)
        reads_regs |= r
        reads_mem = reads_mem or mm
    if not reads_regs and not reads_mem:
        return False
    clobbered = False
    for k in range(i if ops is not None else i + 1, len(insns)):
        ins = insns[k]
        if _is_flag_consumer(ins) and clobbered:
            return True
        if _sets_flags(ins):
            return False
        w_regs, w_mem = _insn_writes(ins)
        if (w_regs & reads_regs) or (w_mem and reads_mem):
            clobbered = True
    return clobbered and ops is None and live_out


def _emit_flag_snapshot(ops):
    """Copy the setter's operands into _fsa/_fsb. Returns (stmts, new_ops)."""
    stmts, new_ops, seen = [], [], {}
    names = ["_fsa", "_fsb"]
    n = 0
    for op in ops[:2]:
        if op.type not in ("reg", "mem"):
            new_ops.append(op)
            continue
        expr = _fmt_operand_read(op)
        if expr in seen:
            new_ops.append(seen[expr])
            continue
        name = names[n]
        n += 1
        size = _op_size(op) or 4
        stmts.append(f"{name} = (uint32_t)({expr}); /* flag operand snapshot */")
        cast = {1: "uint8_t", 2: "uint16_t"}.get(size)
        snap = Operand(type="snap", reg=f"(({cast}){name})" if cast else name)
        snap.snap_size = size
        seen[expr] = snap
        new_ops.append(snap)
    new_ops.extend(ops[2:])
    return stmts, new_ops


def _make_cf_expr(setter, ops):
    """CF as a C expression, from the last flag setter, for sbb/adc/rcl/rcr.

    `_cf` used to be a local initialised to 0 and never assigned, so every
    `sbb reg, reg` produced 0 -- including MSVC's boolean idiom
    `cmp al, 2 / sbb eax, eax / neg eax` (= al < 2), which is how DirectSound's
    lock decides whether it raised IRQL. It never did. Operands here are the
    ones _make_condition would use: post-op for result-based setters."""
    if setter == FIN_SETTER:
        return f"{ops[0]}_jb"
    if setter in ("test", "and", "or", "xor"):
        return "0"
    if not ops:
        return None
    size = _op_size(ops[0]) or 4
    mask = {1: "0xFFu", 2: "0xFFFFu"}.get(size, "0xFFFFFFFFu")
    a = _fmt_operand_read(ops[0])
    b = _fmt_operand_read(ops[1]) if len(ops) >= 2 else None
    if setter == "cmp" and b is not None:
        return f"((((uint32_t)({a})) & {mask}) < (((uint32_t)({b})) & {mask}))"
    if setter == "sub" and b is not None:
        # a is post-op: a_pre = a + b, and a_pre < b iff the subtraction borrowed.
        return (f"(((((uint32_t)({a})) + ((uint32_t)({b}))) & {mask}) "
                f"< (((uint32_t)({b})) & {mask}))")
    if setter == "add" and b is not None and a != b:
        # post-op sum below an addend iff the addition carried.
        return f"((((uint32_t)({a})) & {mask}) < (((uint32_t)({b})) & {mask}))"
    if setter == "neg":
        return f"((((uint32_t)({a})) & {mask}) != 0u)"
    return None


def lift_basic_block(lifter, bb, flag_state=None, live_out=True):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # Track the last instruction that set flags
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []

    # Flags that arrived from a predecessor are rebuilt from the setter's
    # operands; if this block overwrites them before consuming, copy them
    # first, exactly as for a setter inside the block.
    if (last_flag_setter and last_flag_setter != FIN_SETTER
            and last_flag_setter in (_SNAP_PRE | _SNAP_POST)
            and all(isinstance(o, Operand) for o in last_flag_ops)
            and _needs_flag_snapshot(insns, 0, ops=last_flag_ops)):
        snap_stmts, last_flag_ops = _emit_flag_snapshot(last_flag_ops)
        stmts.extend(snap_stmts)

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            stmts.append(stmt)
            # Preserve the flag-setter from the cmp/test since jcc
            # doesn't modify flags - subsequent jcc can reuse them
            flag_insn = insns[i]
            last_flag_setter = flag_insn.mnemonic
            last_flag_ops = list(flag_insn.operands)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # Snapshot the setter's operands if something before its consumer
        # overwrites them -- see _needs_flag_snapshot.
        snap_ops = None
        snap_post = False
        if curr.mnemonic in _SNAP_PRE or curr.mnemonic in _SNAP_POST:
            if _needs_flag_snapshot(insns, i, live_out=live_out):
                if curr.mnemonic in _SNAP_PRE:
                    snap_stmts, snap_ops = _emit_flag_snapshot(curr.operands)
                    stmts.extend(snap_stmts)
                else:
                    snap_post = True

        # sbb/adc/rcl/rcr read CF, which is never materialised: compute it
        # from the previous setter before this instruction consumes it.
        if curr.mnemonic in ("sbb", "adc") and last_flag_setter:
            cf = _make_cf_expr(last_flag_setter, last_flag_ops)
            if cf is not None:
                stmts.append(f"_cf = {cf}; /* CF from {last_flag_setter} */")

        # Lift the instruction normally
        results = lifter.lift_instruction(insns[i])
        stmts.extend(results)

        if snap_post:
            snap_stmts, snap_ops = _emit_flag_snapshot(curr.operands)
            stmts.extend(snap_stmts)

        # Track flag-setting instructions
        if curr.mnemonic in FLAG_SETTERS:
            last_flag_setter = curr.mnemonic
            last_flag_ops = snap_ops if snap_ops is not None else list(curr.operands)
        elif curr.mnemonic in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif curr.mnemonic in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _EFLAGS_PRESERVE:
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            if "cmps" in raw_m or "scas" in raw_m:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif "cmps" in rest or "scas" in rest:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
