"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import json
import os
import re

# Import the functions, not the VA constants: configure_from_xbe() rebinds those
# at startup, so a by-value import would freeze the fallback layout.
from .config import va_to_file_offset, is_code_address
from .disasm import Disassembler
from .lifter import (Lifter, lift_basic_block, detect_seh_helpers,
                     FIN_SETTER, _make_condition, _is_flag_consumer,
                     _sets_flags)


def _fixup_icall_esp_save(lines):
    """
    Post-process generated C lines to insert _icall_esp save points.

    When RECOMP_ICALL_SAFE is used, we need to save g_esp BEFORE any
    args are pushed so the macro can restore it on lookup failure.

    Scans backwards from each RECOMP_ICALL_SAFE line to find consecutive
    PUSH32 lines (the arg pushes), then inserts a save before the first.
    """
    import re
    result = []
    # Find indices of all ICALL_SAFE lines
    icall_indices = []
    for i, line in enumerate(lines):
        if 'RECOMP_ICALL_SAFE(' in line:
            icall_indices.append(i)

    if not icall_indices:
        return lines  # nothing to do

    # For each ICALL, determine where to insert the save
    insert_before = set()  # map: line_index → True (insert save before this line)
    for icall_idx in icall_indices:
        # The ICALL line itself contains "PUSH32(esp, 0); RECOMP_ICALL_SAFE(...)"
        # Look backwards for consecutive lines containing PUSH32(esp,
        first_push_idx = icall_idx
        j = icall_idx - 1
        while j >= 0:
            stripped = lines[j].strip()
            # Skip blank lines
            if not stripped:
                j -= 1
                continue
            # Check if this is a PUSH32 line (arg push)
            if stripped.startswith('PUSH32(esp,'):
                first_push_idx = j
                j -= 1
                continue
            # Check if this is a non-push instruction that could be part of
            # arg evaluation (e.g., "eax = MEM32(...);") - these are interleaved
            # with pushes in the x86 code. We need to look past them.
            # Stop at labels, gotos, other control flow, or other ICALL lines.
            if (re.match(r'^loc_[0-9A-Fa-f]+:', stripped) or
                'goto ' in stripped or
                'RECOMP_ICALL' in stripped or
                'return;' in stripped or
                stripped.startswith('if (') or
                stripped.startswith('POP32(') or
                stripped.startswith('PUSH32(esp, 0); sub_')):
                break
            # It's an interleaved computation - skip past it
            j -= 1
            continue

        insert_before.add(first_push_idx)

    # Build result with saves inserted
    for i, line in enumerate(lines):
        if i in insert_before:
            # Determine indentation from the current line
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}{{ uint32_t _icall_esp = g_esp;")
        result.append(line)
        if 'RECOMP_ICALL_SAFE(' in line:
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}}}")

    return result


def _flag_key(state):
    if state is None:
        return None
    setter, ops = state
    if setter == FIN_SETTER:
        return (setter, tuple(ops))
    return (setter, tuple((o.type, o.reg, o.imm, o.mem_base, o.mem_index,
                           o.mem_scale, o.mem_disp, o.mem_size, o.mem_segment)
                          for o in ops))


_FIN_NAME = re.compile(r"\b(_fin[0-9A-F]+)_(j[a-z]+)\b")


def _incoming_flag_states(lifter, blocks, func_start):
    """Flag state entering each block, taken from its real predecessors.

    The translator used to hand every block the flags left by the block just
    before it *in address order*. That is only a predecessor when it falls
    through. MSVC threads jumps, so a jcc often starts a block whose only way
    in is a branch from elsewhere:

        184AB  test ebx, ebx
        184AD  jge  184C5
        184AF  ...            ; error path, ends in  jmp 18557
        184C0  jmp  18557
        184C5  je   18557     ; flags are test ebx,ebx's

    The je was emitted as `if (esp == 0)` from the error path's `add esp, 4`,
    so a zero-length decode was never skipped; EA's stream mixer then called
    its float-to-PCM converter with a NULL destination and a pointer for a
    count, which wrote 16-bit samples from address 0 upward across .text, the
    kernel import table and .data.

    A fixpoint over the CFG: a block's in-state is the common out-state of
    its predecessors. Where they genuinely disagree -- MSVC tail-merges a jcc
    that two different compares both reach:

        32508  cmp eax, 3 / jl 32516 / cmp eax, 4 / 32510 jg 32516
        324E6  cmp eax, 3 / jmp 32510

    -- no single setter is right, so every predecessor evaluates the
    conditions the block needs into `_finXXXX_jcc` locals just before it
    leaves, and the block reads those. A block with no known predecessor
    (switch-table target) starts unknown.

    Returns (in_state, fin): fin maps a predecessor block start to the
    statements it must run before its terminator.
    """
    preds = {bb.start: [] for bb in blocks}
    for bb in blocks:
        for s in bb.successors:
            if s in preds and bb.start not in preds[s]:
                preds[s].append(bb.start)

    # Backward liveness of the flags across block boundaries: a block reads
    # its incoming flags if a consumer comes before any setter, or if it sets
    # none and a successor reads them. Only then does a setter whose operands
    # are overwritten later in its block need a snapshot for the successor.
    reads_in, passes = {}, {}
    for bb in blocks:
        r, p_ = False, True
        for ins in bb.instructions:
            if _is_flag_consumer(ins):
                r = True
                break
            if _sets_flags(ins) or ins.is_call:
                p_ = False
                break
        reads_in[bb.start], passes[bb.start] = r, p_
    changed = True
    while changed:
        changed = False
        for bb in blocks:
            if reads_in[bb.start] or not passes[bb.start]:
                continue
            if any(reads_in.get(s) for s in bb.successors):
                reads_in[bb.start] = True
                changed = True
    live = {bb.start: any(reads_in.get(s) for s in bb.successors)
            for bb in blocks}

    def lift(bb, st):
        return lift_basic_block(lifter, bb, flag_state=st,
                                live_out=live[bb.start])

    def solve(forced):
        out_state, in_state, conflicts = {}, {}, set()
        for _ in range(len(blocks) + 2):
            changed = False
            for bb in blocks:
                if bb.start in forced:
                    st = (FIN_SETTER, [forced[bb.start]])
                else:
                    ps = [p for p in preds[bb.start] if p in out_state]
                    if not ps:
                        st = None
                    else:
                        keys = {_flag_key(out_state[p]) for p in ps}
                        st = out_state[ps[0]]
                        if len(keys) != 1:
                            conflicts.add(bb.start)
                in_state[bb.start] = st
                _, out = lift(bb, st)
                if (bb.start not in out_state
                        or _flag_key(out_state[bb.start]) != _flag_key(out)):
                    out_state[bb.start] = out
                    changed = True
            if not changed:
                break
        # A conflict seen mid-iteration may have resolved; keep only real ones.
        real = set()
        for c in conflicts:
            ps = [p for p in preds[c] if p in out_state]
            if len({_flag_key(out_state[p]) for p in ps}) > 1:
                real.add(c)
        return in_state, out_state, real

    forced = {}
    while True:
        in_state, out_state, conflicts = solve(forced)
        new = [c for c in conflicts if c not in forced]
        if not new:
            break
        for c in new:
            forced[c] = "_fin%X" % c

    # Which materialised conditions are read, closed over predecessors whose
    # own out-state is materialised.
    prefix_block = {v: k for k, v in forced.items()}
    needed = set()
    for bb in blocks:
        stmts, _ = lift(bb, in_state[bb.start])
        needed |= set(_FIN_NAME.findall("\n".join(stmts)))
    fin, unknown = {}, 0
    work, done = list(needed), set()
    while work:
        prefix, m = work.pop()
        if (prefix, m) in done or prefix not in prefix_block:
            continue
        done.add((prefix, m))
        c = prefix_block[prefix]
        for p in preds[c]:
            st = out_state.get(p)
            r = _make_condition(m, st[0], st[1]) if st else None
            if r is None:
                unknown += 1
                line = (f"{prefix}_{m} = 0; /* flags for loc_{c:08X}: unknown "
                        f"on the path from loc_{p:08X} */")
            else:
                line = (f"{prefix}_{m} = ({r[0]}) ? 1 : 0; "
                        f"/* flags for loc_{c:08X} */")
                for more in _FIN_NAME.findall(r[0]):
                    work.append(more)
            fin.setdefault(p, []).append(line)
    for p in fin:
        fin[p].sort()

    stats = os.environ.get("XLIFT_FLAG_STATS")
    if stats:
        old, prev = {}, None
        for bb in blocks:
            old[bb.start] = prev
            _, prev = lift(bb, prev)
        changed_blocks = []
        for bb in blocks:
            a, _ = lift(bb, old[bb.start])
            b, _ = lift(bb, in_state[bb.start])
            if a != b:
                changed_blocks.append(bb.start)
        if changed_blocks or forced:
            with open(stats, "a", encoding="utf-8") as f:
                f.write("0x%08X changed=%s materialised=%s unknown=%d\n" % (
                    func_start, ",".join("0x%X" % x for x in changed_blocks),
                    ",".join("0x%X" % x for x in sorted(forced)), unknown))
    return in_state, fin, live


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None, seh_prolog=None, seh_epilog=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        seh_prolog/seh_epilog: override the detected SEH helper addresses
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db,
                             xbe_data=xbe_data, seh_prolog=seh_prolog,
                             seh_epilog=seh_epilog)

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        end = func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end

        # Disassemble
        instructions = self.disasm.disassemble_function(raw_bytes, start, end)
        if not instructions:
            return None

        # Collect switch table targets as extra block leaders
        switch_leaders = set()
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                targets = self.lifter._analyze_switch_table(insn.operands)
                for t in targets:
                    if start <= t < end:
                        switch_leaders.add(t)

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(
            instructions, start, end,
            extra_leaders=switch_leaders if switch_leaders else None)
        if not blocks:
            return None

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info (kept for comments)
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Volatile registers (eax, ecx, edx, esp) are globals - don't declare
        # them as locals. The RECOMP_GENERATED_CODE #define maps register names
        # to the global variables via preprocessor macros.
        volatile_regs = {"eax", "ecx", "edx", "esp"}

        # Ensure ebp tracked if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Ensure ebp tracked if function has tail jumps (lifter emits
        # g_seh_ebp = ebp before external jmp and indirect jmp).
        #
        # BUG (found on SSX Tricky, fixed alongside the matching lifter.py
        # fix in _lift_jcc): this only checked unconditional `jmp`. A
        # *conditional* jump to an external target (`je sub_XXXXX` etc.) is
        # just as much a tail call, and _lift_jcc emits the identical
        # `g_seh_ebp = ebp; target(); return;` pattern for it -- but without
        # this check also covering conditional jumps, a function whose
        # *only* external branch is conditional (has_tail_jump False, ebp
        # otherwise unused) would never get `ebp` declared as a local at
        # all, and the emitted `g_seh_ebp = ebp;` would reference an
        # undeclared variable -- a compile error, not a silent bug, which
        # is how this would have been caught immediately on the next full
        # rebuild if left unfixed.
        has_tail_jump = any(
            (insn.mnemonic == "jmp" or insn.is_cond_jump) and (
                (insn.jump_target and not (start <= insn.jump_target < end))
                or (not insn.jump_target and insn.mnemonic == "jmp")  # indirect jmp
            )
            for insn in instructions
        )
        if has_tail_jump:
            used_regs.add("ebp")

        # Ensure ebp tracked if the function's last basic block "falls off
        # the end" into whatever function starts right at its own end
        # address, instead of terminating in a ret or an (already-handled)
        # external jmp.
        #
        # BUG (found on SSX Tricky, fixed): build_basic_blocks() only
        # records a fallthrough/branch successor when the target address is
        # still inside [func_start, func_end) (see disasm.py's
        # build_basic_blocks: the `if last.end_address < func_end` /
        # `elif last.is_cond_jump` checks). When a function's real x86 code
        # is a fall-through predecessor of a sibling function the detector
        # split out right at this function's own end address -- a normal
        # instruction, or the not-taken side of a conditional jump, whose
        # natural continuation is func_end itself -- nothing records that
        # edge, and nothing downstream ever emits a call into the sibling
        # function for it. The lifter already has the exact right handling
        # for this (see _lift_jmp's `_is_external_target` branch, used for
        # explicit tail jmps); it just never gets *reached* for this case,
        # because there's no explicit jmp instruction to trigger it. Confirmed
        # against several real instances by disassembling the original bytes
        # directly: the "next" instruction genuinely is the sibling
        # function's first instruction, with no ret/jmp in between. Silently
        # produces a function that returns without doing what the tail
        # actually does -- for a callee-cleans-its-own-stack function
        # (`ret N`), without popping N bytes; for a shared final-block
        # fragment (`leave; ret`), without restoring esp/ebp at all -- which
        # reliably corrupts the caller's stack on every single call through
        # that path (sub_001544C3/sub_0015CAC7/
        # CRT_ftol_TruncateToInt64/sub_0012A627/sub_00150DB0/sub_00150B39/
        # sub_0015457F and the six sub_0015F07D callers, found and hand-fixed
        # one at a time before this general fix; sub_0015457F specifically
        # meant a whole game's heap arena was silently never allocated).
        #
        # Fix: after lifting all blocks, if the function's last block ends
        # on something other than a ret or unconditional jmp (both already
        # handled correctly) and that instruction's natural continuation
        # reaches func_end, emit the same "call the sibling function; return"
        # pattern _lift_jmp already uses for an explicit external tail jmp.
        _last_block_falls_through_past_end = False
        if blocks:
            _last_insn_in_func = blocks[-1].last_insn
            if (_last_insn_in_func is not None
                    and not _last_insn_in_func.is_terminator
                    and _last_insn_in_func.end_address >= end):
                _last_block_falls_through_past_end = True
        if _last_block_falls_through_past_end:
            used_regs.add("ebp")

        # Ensure ebp tracked if function calls __SEH_prolog or __SEH_epilog
        # (lifter emits ebp = g_seh_ebp readback after these calls).
        #
        # BUG (found on SSX Tricky, fixed): this used to be a hardcoded
        # {0x00244784, 0x002447BF} -- Burnout 3's specific __SEH_prolog/
        # __SEH_epilog addresses, left over from the tool's reference
        # implementation. For any other game (SSX Tricky's real addresses
        # are 0x0015DEBC/0x0015DEF5, dynamically detected and logged at
        # startup as "SEH helpers: ..."), that hardcoded set never matches,
        # so this used_regs.add("ebp") never fires even though the emission
        # code below (which correctly uses the dynamically-detected
        # self.lifter.SEH_PROLOG/SEH_EPILOG) still emits the "ebp = g_seh_ebp"
        # readback line -- producing a declared-nowhere compile error
        # ('ebp' undeclared) for any such function. Use the same
        # dynamically-detected addresses the emission side already uses,
        # instead of Burnout 3's hardcoded ones.
        seh_funcs = {a for a in (getattr(self.lifter, "SEH_PROLOG", None),
                                  getattr(self.lifter, "SEH_EPILOG", None)) if a is not None}
        if any(insn.call_target in seh_funcs for insn in instructions):
            used_regs.add("ebp")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # All translated functions are void(void).
        # Arguments pass via the global simulated stack (push instructions).
        # Return values pass via g_eax (the global eax register).
        ret_type = "void"
        param_str = "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")
        body_decl_idx = len(lines)  # where late-discovered locals are declared

        # ebp is the only callee-saved register declared as a local.
        # ebx, esi, edi are global via #define macros (g_ebx, g_esi, g_edi)
        # and must NOT be declared locally, otherwise the local shadows
        # the global and cross-function register passing breaks.
        # Volatile registers (eax, ecx, edx, esp) are also global via macros.
        reg_decls = []
        if "ebp" in used_regs:
            reg_decls.append("ebp")
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # Add _flags variable if function has conditional instructions
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")

        # Add _cf for carry-dependent instructions (sbb, adc)
        has_carry = any(insn.mnemonic in ("sbb", "adc")
                        for insn in instructions)
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")

        # Add _fpu_cmp for FPU compare instructions (both old and new style)
        has_fpu_cmp = any(insn.mnemonic in ("fcompi", "fcomip", "fucomi",
                                             "fucompi", "fucomip", "fcomi",
                                             "fcom", "fcomp", "fcompp",
                                             "fucom", "fucomp", "fucompp",
                                             "ficom", "ficomp", "ftst",
                                             "fnstsw", "fstsw", "fxam")
                          for insn in instructions)
        if has_fpu_cmp:
            lines.append(f"    int _fpu_cmp = 0; /* FPU compare result: -1/0/1 */")

        # SSE/MMX registers are NOT declared here: xmm0-7 and
        # mm0-7 are macros for the per-thread register files g_xmm / g_mm in
        # recomp_types.h. Function-local copies lost every value live across
        # a call, a tail jump or a fall-through into a split fragment -- the
        # same defect the x87 stack used to have.

        # FPU stack: the per-thread x87 register file. `_fp_stack`/`_fp_top`
        # alias g_fp_stack/g_fp_top in recomp_types.h; a private copy per
        # function lost every value live across a call or a function split
        # (104 fragments read a stack their predecessor filled).
        if has_fpu:
            lines.append(f"    #define fp_push(v) (_fp_stack[--_fp_top & 7] = (g_x87_st0 = (v)))")
            lines.append(f"    #define fp_pop() (_fp_top++)")
            lines.append(f"    #define fp_popp() (fp_pop())")
            lines.append(f"    #define fp_top() _fp_stack[_fp_top & 7]")
            lines.append(f"    #define fp_st1() _fp_stack[(_fp_top + 1) & 7]")

        # Any function that uses ebp: initialize the local from g_seh_ebp.
        #
        # BUG (found on SSX Tricky, fixed): this used to be gated on
        # `frame_type == "fpo_leaf" and not has_prologue`, i.e. only
        # functions that use ebp as inherited scratch *without* their own
        # prologue got this seed. But a function *with* its own real
        # `push ebp; mov ebp, esp` prologue (has_prologue=True) still needs
        # it: the `push ebp` instruction's operand is the *caller's* ebp,
        # read before this function's own frame is established -- and in
        # this translation, ebp is a per-function C local that only ever
        # gets a value from either that same seed or this function's own
        # `mov ebp, esp` (which comes *after* the push). Excluding
        # has_prologue functions meant that push captured whatever
        # uninitialized garbage happened to be on the native C call stack
        # at that point instead of the real caller's ebp -- silently
        # corrupting what a later `pop ebp`/`leave` restores for the
        # caller, and any MEM32(ebp + offset) read via a tail-call chain
        # that carries the (garbage) value onward via `g_seh_ebp = ebp;`.
        # Confirmed live on SSX Tricky: sub_0017FE15 (has_prologue=True,
        # frame_type != fpo_leaf) pushed garbage that read as Xbox VA
        # ~0x1541A9 several tail-calls later in sub_0017FE66, causing a
        # `MEM32(that garbage + 0x9C)` read that itself produced another
        # garbage pointer (0x78000000) and crashed on the next dereference
        # Seeding
        # ebp here is always correct regardless of frame_type/has_prologue:
        # a function that reads ebp before writing it now gets the right
        # value; a function that writes ebp before ever reading it (pure
        # scratch use) just has the seed immediately overwritten.
        if "ebp" in used_regs:
            lines.append(f"    ebp = g_seh_ebp; /* inherit caller's frame */")

        lines.append(f"")

        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)
        # Add switch table targets (indirect jmp with intra-function table)
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                switch_targets = self.lifter._analyze_switch_table(insn.operands)
                for t in switch_targets:
                    label_addrs.add(t)

        in_states, fin, flags_live = _incoming_flag_states(self.lifter, blocks, start)
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                # The trailing ';' is load-bearing: C requires a statement after
                # a label, and a block whose instructions all emit comments only
                # (a lone `cmp`, which just sets flags for the next jcc) would
                # otherwise produce `loc_X:` immediately before `}` and fail to
                # compile. The null statement costs nothing and is always valid.
                lines.append(f"loc_{bb.start:08X}: ;")

            # A block that opens with a flag consumer (test eax,eax / ja X /
            # jb Y -- the jb reuses ja's flags) takes them from its real
            # predecessors, not from whichever block precedes it in address
            # order; see _incoming_flag_states.
            stmts, _ = lift_basic_block(
                self.lifter, bb, flag_state=in_states.get(bb.start),
                live_out=flags_live.get(bb.start, True))
            if bb.start in fin:
                # Before the terminator: the flags and their operands are
                # still the ones this block leaves with.
                last = bb.instructions[-1] if bb.instructions else None
                at = (len(stmts) - 1 if last is not None and stmts
                      and (last.is_jump or last.is_cond_jump) else len(stmts))
                stmts = stmts[:at] + fin[bb.start] + stmts[at:]
            for stmt in stmts:
                lines.append(f"    {stmt}")

            lines.append(f"")

        # Flag-operand snapshots (lifter._emit_flag_snapshot) are only known
        # once the body is lifted, so declare them now if any were used.
        if any("_fsa" in l or "_fsb" in l for l in lines[body_decl_idx:]):
            lines.insert(body_decl_idx,
                         "    uint32_t _fsa = 0, _fsb = 0; "
                         "/* flag-operand snapshots */")
        fin_names = sorted({"%s_%s" % t for l in lines[body_decl_idx:]
                            for t in _FIN_NAME.findall(l)})
        if fin_names:
            lines.insert(body_decl_idx,
                         "    int " + ", ".join(n + " = 0" for n in fin_names)
                         + "; /* conditions from disagreeing predecessors */")

        # Emit the missing fall-through-into-sibling-function link detected
        # above, using the exact same pattern _lift_jmp uses for an explicit
        # external tail jmp. This is what real x86 execution does here: fall
        # straight through into the next function's first instruction with
        # no call/ret boundary, so from the callee's perspective this reads
        # exactly like a tail call into it.
        if _last_block_falls_through_past_end:
            _fallthrough_name = self.lifter._call_target_name(end)
            lines.append(
                f"    g_seh_ebp = ebp; {_fallthrough_name}(); return; "
                f"/* implicit fall-through into 0x{end:08X} (no ret/jmp here in "
                f"the original bytes -- this function's real code just "
                f"continues directly into the next one) */")
            lines.append(f"")

        # Insert _icall_esp save points before RECOMP_ICALL_SAFE arg pushes.
        # The pattern is: optional PUSH32 args, then PUSH32(esp, 0); RECOMP_ICALL_SAFE(...).
        # We insert "uint32_t _icall_esp = g_esp;" before the first arg push.
        lines = _fixup_icall_esp_save(lines)

        # Validate: comment out goto targets that reference missing labels
        # (dead code after unconditional jumps may reference non-existent labels)
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    f"(void)0; /* goto {target} - dead code, label not in function */")

        # Ensure labels at end of function have a statement after them.
        # In C, a label must be followed by a statement; a comment alone is not
        # enough.  Walk backwards from the end and if the last real content is a
        # label (with only blank lines / comments after it), insert "(void)0;".
        _last_label_idx = None
        _has_stmt_after = False
        for _ri in range(len(lines) - 1, -1, -1):
            _s = lines[_ri].strip()
            if not _s:
                continue
            if _s.startswith("/*") and _s.endswith("*/"):
                continue
            if re.match(r'^loc_[0-9A-Fa-f]+:', _s):
                _last_label_idx = _ri
                break
            _has_stmt_after = True
            break
        if _last_label_idx is not None and not _has_stmt_after:
            lines.insert(_last_label_idx + 1, "    (void)0;")

        # Undefine FPU macros
        if has_fpu:
            lines.append(f"    #undef fp_push")
            lines.append(f"    #undef fp_pop")
            lines.append(f"    #undef fp_popp")
            lines.append(f"    #undef fp_top")
            lines.append(f"    #undef fp_st1")

        lines.append(f"}}")
        lines.append(f"")

        # Publish ebp before every call. A callee seeds its own ebp from
        # g_seh_ebp ("inherit caller's frame" above), which only holds the
        # caller's ebp if the caller wrote it -- tail jumps did, calls never
        # did. On hardware the callee always sees the caller's EBP; here a
        # fragment such as the CRT's atan2 classifier (0x0015EED3, reached by
        # `call` from the dispatcher at 0x0015F33B) read a stale frame: its
        # `fldcw [ebp-0xA2]` loaded garbage and its fxam results landed in
        # the wrong slots. That made atan2/acos return NaN, the race camera
        # went NaN and the course never drew.
        if "ebp" in used_regs:
            call_rx = re.compile(r"^(\s*(?:\{ uint32_t _icall_esp = g_esp;\s*)?)"
                                  r"(PUSH32\(esp, 0\); (?:\w+\(\)|RECOMP_ICALL))")
            for i, ln in enumerate(lines):
                if "g_seh_ebp = ebp" in ln:
                    continue
                m = call_rx.match(ln)
                if m:
                    lines[i] = m.group(1) + "g_seh_ebp = ebp; " + ln[len(m.group(1)):]

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None, seh_prolog=None, seh_epilog=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Detect the SEH helpers once here rather than per-Lifter, so the
        # result can be reported and overridden from the command line.
        if seh_prolog is None or seh_epilog is None:
            found_prolog, found_epilog = detect_seh_helpers(
                self.func_db, self.xbe_data, verbose=True)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.seh_prolog = seh_prolog
        self.seh_epilog = seh_epilog

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db,
            seh_prolog=seh_prolog, seh_epilog=seh_epilog)

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string.
        All translated functions are void(void) - args pass via stack,
        return values via g_eax."""
        return f"void {name}(void)"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(" * Burnout 3: Takedown - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#define RECOMP_GENERATED_CODE')
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", verbose=False):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        # Translate all functions first, collecting results
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)

            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # Any address called but never defined needs a stub, or the link fails.
        # These are almost all mid-function entry points the function detector
        # did not split out: a call lands a few bytes inside (or just past) a
        # function it already found. Emitting an empty stub keeps the build
        # linking; hitting one at runtime is a silent no-op, so they are
        # reported and written to their own file rather than hidden among the
        # translated chunks.
        defined = {name for _, name, _ in translations}
        unresolved = {
            addr: name
            for addr, name in self.translator.lifter.referenced_calls.items()
            if name not in defined
        }
        stats["unresolved_stubs"] = len(unresolved)

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            " * Burnout 3: Takedown - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")

        if unresolved:
            header_lines.append("")
            header_lines.append("/* Unresolved call targets (stubbed) */")
            for addr in sorted(unresolved):
                header_lines.append(f"void {unresolved[addr]}(void);")

        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # Split translations into chunks and write .c files
        generated_files = [header_path]
        chunks = [translations[i:i+chunk_size]
                  for i in range(0, len(translations), chunk_size)]

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * Burnout 3 - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                '#include <math.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Emit the stub bodies for call targets with no definition.
        if unresolved:
            stub_path = os.path.join(output_dir, f"{prefix}_stubs_unresolved.c")
            stub_lines = [
                "/**",
                " * Unresolved call target stubs",
                f" * {len(unresolved)} addresses called by translated code but not",
                " * detected as functions - typically mid-function entry points.",
                " * Auto-generated by tools/recomp.",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                "",
            ]
            for addr in sorted(unresolved):
                stub_lines.append(
                    f"void {unresolved[addr]}(void) {{ /* 0x{addr:08X}: not detected */ }}"
                )
            stub_lines.append("")

            with open(stub_path, "w", encoding="utf-8") as f:
                f.write("\n".join(stub_lines))
            generated_files.append(stub_path)

            if verbose:
                print(f"  Wrote {stub_path} ({len(unresolved)} stubs)",
                      file=sys.stderr)

        # Generate dispatch table
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(translations, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            " * Burnout 3 - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            "#define RECOMP_DISPATCH_H",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        # recomp_lookup binary-searches this table, so it must be emitted
        # in ascending address order. It was not, and a binary search over
        # an unsorted array fails silently -- it reports "not found" for
        # entries it steps past. Forty functions were unreachable that way.
        for addr, name, _ in sorted(translations, key=lambda t: t[0]):
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* Binary search for a function by Xbox VA */",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    size_t lo = 0, hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else",
            "            return g_recomp_table[mid].func;",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
