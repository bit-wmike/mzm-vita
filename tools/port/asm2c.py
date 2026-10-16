#!/usr/bin/env python3
"""Static translator for the few hand-written ARM/Thumb routines of the game.

The sound engine core (asm/audio_internal.s) and the PCM mixer
(asm/soundcode.s) are hand-written assembly. For the host/Vita port they are
translated instruction by instruction into C: every ARM register becomes a
32-bit variable, the CPU flags are computed exactly as on the ARM7TDMI and
every branch becomes a goto. The result behaves like the original code by
construction, on any 32-bit target.

Input is the object file assembled with arm-none-eabi-as; the instructions are
read back with objdump (canonical syntax, numeric operands, relocations).

Only the instruction forms that occur in these two files are supported; any
other form aborts the translation instead of silently producing wrong code.
"""
import argparse
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

REGS = {"r%d" % i: "r%d" % i for i in range(13)}
REGS.update({"sl": "r10", "fp": "r11", "ip": "r12", "sp": "sp", "lr": "lr", "pc": "pc",
             "r13": "sp", "r14": "lr", "r15": "pc"})

CONDS = {
    "eq": "z", "ne": "!z", "cs": "c", "hs": "c", "cc": "!c", "lo": "!c", "mi": "n", "pl": "!n",
    "vs": "v", "vc": "!v", "hi": "(c && !z)", "ls": "(!c || z)", "ge": "(n == v)", "lt": "(n != v)",
    "gt": "(!z && n == v)", "le": "(z || n != v)", "al": "1",
}

# External C functions called from the assembly, with their real prototypes.
EXTERNAL_CALLS = {
    "BitFill": "BitFill((u8)r0, r1, P(r2), r3, (u8)LD32(sp));",
    "ClearRegistersForPsg": "ClearRegistersForPsg((struct PsgSoundData *)P(r0), (u8)r1);",
    "DoSoundAction": "DoSoundAction(r0);",
    "MidiKey2Freq": "r0 = MidiKey2Freq((u32 *)P(r0), (u8)r1, (u8)r2);",
    "UpdateMusic": "UpdateMusic();",
    "UpdatePsgSounds": "UpdatePsgSounds();",
    "UpdateTrack": "UpdateTrack((struct TrackData *)P(r0));",
    "unk_20a4": "unk_20a4((struct SoundChannel *)P(r0));",
}


# Linker symbols whose *address* is the value (see linker.ld / structs/audio.h).
VALUE_SYMBOLS = {"gNumMusicPlayers", "gUnk_Audio0x64", "gUnk_Audio0x194F700", "gUnk_Audio0x50"}


class Insn:
    def __init__(self, addr, mnem, ops, comment, thumb, size):
        self.addr, self.mnem, self.ops, self.comment, self.thumb, self.size = addr, mnem, ops, comment, thumb, size


def die(msg):
    sys.exit("asm2c: " + msg)


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def reg(r):
    r = r.strip()
    if r not in REGS:
        die("not a register: %r" % r)
    return REGS[r]


def imm(s):
    s = s.strip()
    if not s.startswith("#"):
        die("not an immediate: %r" % s)
    return int(s[1:], 0) & 0xFFFFFFFF


def reglist(s):
    s = s.strip()
    assert s[0] == "{" and s[-1] == "}", s
    out = []
    for part in s[1:-1].split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-")
            a, b = int(reg(a)[1:]), int(reg(b)[1:])
            out += ["r%d" % i for i in range(a, b + 1)]
        else:
            out.append(reg(part))
    order = lambda r: {"sp": 13, "lr": 14, "pc": 15}.get(r, None) if not r.startswith("r") else int(r[1:])
    return sorted(out, key=order)


class Translator:
    def __init__(self, obj, objdump):
        self.obj = obj
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            text = elf.get_section_by_name(".text")
            self.text = text.data()
            self.relocs = {}
            for rs in elf.iter_sections():
                if isinstance(rs, RelocationSection) and rs.name == ".rel.text":
                    symtab = elf.get_section(rs["sh_link"])
                    for r in rs.iter_relocations():
                        self.relocs[r["r_offset"]] = (symtab.get_symbol(r["r_info_sym"]).name, r["r_info_type"])
            symtab = elf.get_section_by_name(".symtab")
            self.mapping = []
            self.funcs = {}
            for s in symtab.iter_symbols():
                if s.name in ("$t", "$a", "$d") or s.name.startswith(("$t.", "$a.", "$d.")):
                    self.mapping.append((s["st_value"], s.name[:2]))
                elif s["st_info"]["type"] == "STT_FUNC":
                    self.funcs[s["st_value"] & ~1] = s.name
            self.mapping.sort()
        self.insns = []
        dump = subprocess.check_output([objdump, "-d", "--no-show-raw-insn", obj], text=True)
        for line in dump.splitlines():
            m = re.match(r"^\s+([0-9a-f]+):\t(\S+)\s*(.*)$", line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            mnem = m.group(2)
            rest = m.group(3)
            comment = ""
            if "@" in rest:
                rest, comment = rest.split("@", 1)
            ops = split_ops(rest.strip()) if rest.strip() else []
            thumb = self.mode_at(addr) == "$t"
            if mnem in (".word", ".short", ".byte") or self.mode_at(addr) == "$d":
                continue
            self.insns.append(Insn(addr, mnem, ops, comment.strip(), thumb, 0))
        for i, ins in enumerate(self.insns):
            ins.size = (self.insns[i + 1].addr - ins.addr) if i + 1 < len(self.insns) else (2 if ins.thumb else 4)

    def mode_at(self, addr):
        mode = None
        for a, m in self.mapping:
            if a <= addr:
                mode = m
            else:
                break
        return mode

    def word_at(self, addr):
        """Value of a literal pool word, as a C expression."""
        if addr in self.relocs:
            name, rtype = self.relocs[addr]
            if rtype != 2:
                die("unexpected relocation type at pool 0x%x" % addr)
            addend = int.from_bytes(self.text[addr:addr + 4], "little")
            self.data_refs.add(name)
            if name in VALUE_SYMBOLS:
                return "((u32)(uintptr_t)(%s) + %du)" % (name, addend)
            return "((u32)(uintptr_t)&(%s) + %du)" % (name, addend)
        v = int.from_bytes(self.text[addr:addr + 4], "little")
        if 0x04000000 <= v < 0x04000400:
            return "((u32)(uintptr_t)(gHostIo + 0x%x))" % (v - 0x04000000)
        return "0x%08xu" % v

    def functions(self):
        """Thumb function entry points delimit the C functions."""
        starts = sorted(a for a, n in self.funcs.items() if self.mode_at(a) == "$t")
        out = []
        for i, a in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else 1 << 30
            out.append((self.funcs[a], a, [x for x in self.insns if a <= x.addr < end]))
        return out


class FuncGen:
    def __init__(self, tr, name, insns, internal):
        self.tr, self.name, self.insns, self.internal = tr, name, insns, internal
        self.lines = []
        self.targets = set()
        self.adr_const = {}

    def emit(self, s):
        self.lines.append("    " + s)

    # ----- operand helpers -------------------------------------------------
    def operand2(self, ops, setc):
        """ARM operand 2 (or thumb rm/imm). Returns (expr, carry_expr or None)."""
        if len(ops) == 1:
            o = ops[0]
            if o.startswith("#"):
                return "0x%xu" % imm(o), None
            return reg(o), None
        r = reg(ops[0])
        m = re.match(r"(lsl|lsr|asr|ror)\s+(#\S+|\w+)$", ops[1])
        if not m:
            die("bad shifted operand %r" % ops)
        kind, amt = m.group(1), m.group(2)
        if not amt.startswith("#"):
            die("register-shifted operand2 not supported: %r" % ops)
        return self.shift_imm(kind, r, imm(amt))

    def shift_imm(self, kind, r, k):
        if kind == "lsl":
            if k == 0:
                return r, None
            return "(%s << %d)" % (r, k), "((%s >> %d) & 1)" % (r, 32 - k)
        if kind == "lsr":
            if k == 0 or k == 32:
                return "0u", "(%s >> 31)" % r
            return "(%s >> %d)" % (r, k), "((%s >> %d) & 1)" % (r, k - 1)
        if kind == "asr":
            if k == 0 or k == 32:
                return "(u32)((s32)%s >> 31)" % r, "(%s >> 31)" % r
            return "(u32)((s32)%s >> %d)" % (r, k), "(((u32)((s32)%s >> %d)) & 1)" % (r, k - 1)
        die("shift %s" % kind)

    def mem_addr(self, ins, op, rest):
        """Returns (address expr, writeback statement or None)."""
        m = re.match(r"^\[(\w+)\](!?)$", op)
        if m and rest is None:
            return reg(m.group(1)), None
        if m and rest is not None:  # post-indexed: [rn], #imm / [rn], rm
            rn = reg(m.group(1))
            off = ("0x%xu" % imm(rest)) if rest.startswith("#") else reg(rest)
            return rn, "%s += %s;" % (rn, off)
        m = re.match(r"^\[(\w+),\s*(#-?\w+|-?\w+)\](!?)$", op)
        if not m:
            die("addressing mode %r at 0x%x" % (op, ins.addr))
        rn, off, wb = m.group(1), m.group(2), m.group(3)
        if rn == "pc":
            die("unexpected pc-relative %r" % op)
        rn = reg(rn)
        if off.startswith("#"):
            v = int(off[1:], 0)
            expr = "(%s + %du)" % (rn, v & 0xFFFFFFFF) if v >= 0 else "(%s - %du)" % (rn, -v)
        else:
            expr = "(%s + %s)" % (rn, reg(off))
        if wb:
            return rn, "PRE:%s = %s;" % (rn, expr)
        return expr, None

    def set_nz(self, d):
        self.emit("n = %s >> 31; z = (%s == 0);" % (d, d))

    # ----- instruction translation ----------------------------------------
    def translate(self, ins):
        mn, ops = ins.mnem, ins.ops
        cond = None
        base = mn
        if not ins.thumb:
            m = re.match(r"^(stmfd|ldmfd|stmia|ldmia|ldm|stm|push|pop|umull|mla|mul|ldrsb|ldrsh|ldrb|ldrh|strb|strh|ldr|str|"
                         r"and|eor|sub|rsb|add|adc|sbc|rsc|tst|teq|cmp|cmn|orr|mov|bic|mvn|lsl|lsr|asr|ror|bx|bl|b|nop)"
                         r"(s?)(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(s?)$", mn)
            if not m:
                die("unknown ARM mnemonic %r" % mn)
            base = m.group(1) + (m.group(2) or m.group(4) or "")
            cond = m.group(3)
            if base == "b" and cond is None and False:
                pass
        else:
            m = re.match(r"^b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(\.n|\.w)?$", mn)
            if m:
                base, cond = "b", m.group(1)
            else:
                base = mn.replace(".n", "").replace(".w", "")
        if cond and cond != "al" and base != "b":
            self.emit("if (%s) {" % CONDS[cond])
            self.lines_indent = True
            self.do(ins, base, ops, None)
            self.emit("}")
        else:
            self.do(ins, base, ops, cond)

    def do(self, ins, base, ops, cond):
        e = self.emit
        T = ins.thumb
        if base == "nop":
            return
        if base == "b":
            tgt = int(ops[0].split()[0], 16)
            self.targets.add(tgt)
            if cond and cond != "al":
                e("if (%s) goto L_%x;" % (CONDS[cond], tgt))
            else:
                e("goto L_%x;" % tgt)
            return
        if base == "bl":
            name = self.tr.relocs.get(ins.addr, (None,))[0]
            if name is None:
                die("bl without relocation at 0x%x" % ins.addr)
            if name in self.internal:
                e("r0 = asmfn_%s(r0, r1, r2, r3);" % name)
            elif name in EXTERNAL_CALLS:
                e(EXTERNAL_CALLS[name])
                self.calls.add(name)
            else:
                die("call to unknown function %s" % name)
            return
        if base == "bx":
            r = reg(ops[0])
            if r in self.adr_const:
                tgt = self.adr_const[r]
                self.targets.add(tgt)
                e("goto L_%x;" % tgt)
            else:
                e("return r0;")
            return
        if base in ("push", "stmfd") or (base == "stmdb" and ops[0] == "sp!"):
            lst = reglist(ops[-1])
            e("sp -= %d;" % (4 * len(lst)))
            for i, r in enumerate(lst):
                e("ST32(sp + %d, %s);" % (4 * i, r))
            return
        if base in ("pop", "ldmfd"):
            lst = reglist(ops[-1])
            ret = False
            for i, r in enumerate(lst):
                if r == "pc":
                    ret = True
                    continue
                e("%s = LD32(sp + %d);" % (r, 4 * i))
            e("sp += %d;" % (4 * len(lst)))
            if ret:
                e("return r0;")
            return
        if base in ("ldm", "ldmia", "stmia", "stm"):
            rn = ops[0]
            wb = rn.endswith("!")
            rn = reg(rn.rstrip("!"))
            lst = reglist(ops[1])
            if rn in lst and wb:
                die("ldm/stm base in list")
            load = base.startswith("ld")
            for i, r in enumerate(lst):
                if load:
                    e("%s = LD32(%s + %d);" % (r, rn, 4 * i))
                else:
                    e("ST32(%s + %d, %s);" % (rn, 4 * i, r))
            if wb:
                e("%s += %d;" % (rn, 4 * len(lst)))
            return
        if base in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh"):
            rd = reg(ops[0])
            if re.match(r"^\[pc", ops[1]):
                m = re.search(r"([0-9a-f]+)\s*<", ins.comment) or re.search(r"\(([0-9a-f]+)", ins.comment)
                if not m:
                    die("pc-relative load without target at 0x%x" % ins.addr)
                tgt = int(m.group(1), 16)
                if base != "ldr":
                    die("pc-relative %s" % base)
                e("%s = %s;" % (rd, self.tr.word_at(tgt)))
                return
            addr, wb = self.mem_addr(ins, ops[1], ops[2] if len(ops) > 2 else None)
            if wb and wb.startswith("PRE:"):
                e(wb[4:])
                wb = None
            acc = {"ldr": "%s = LD32(%s);", "ldrb": "%s = LD8(%s);", "ldrh": "%s = LD16(%s);",
                   "ldrsb": "%s = (u32)(s32)(s8)LD8(%s);", "ldrsh": "%s = (u32)(s32)(s16)LD16(%s);"}
            if base in acc:
                if wb and rd in addr:
                    die("load into base with writeback")
                e(acc[base] % (rd, addr))
            else:
                fn = {"str": "ST32", "strb": "ST8", "strh": "ST16"}[base]
                e("%s(%s, %s);" % (fn, addr, rd))
            if wb:
                e(wb)
            return
        if base in ("mul", "muls"):
            rd, rm, rs = reg(ops[0]), reg(ops[1]), reg(ops[2] if len(ops) > 2 else ops[0])
            e("%s = %s * %s;" % (rd, rm, rs))
            if base == "muls":
                self.set_nz(rd)
            return
        if base == "mla":
            rd, rm, rs, rn = [reg(x) for x in ops]
            e("%s = %s * %s + %s;" % (rd, rm, rs, rn))
            return
        if base == "umull":
            lo, hi, rm, rs = [reg(x) for x in ops]
            e("{ u64 t_ = (u64)%s * (u64)%s; %s = (u32)t_; %s = (u32)(t_ >> 32); }" % (rm, rs, lo, hi))
            return
        # ---- data processing ----
        S = base.endswith("s") and base not in ("bics",) and base[:-1] in (
            "and", "eor", "sub", "rsb", "add", "orr", "mov", "bic", "mvn", "lsl", "lsr", "asr", "neg", "adc", "sbc")
        op = base[:-1] if S else base
        if T and op in ("add", "sub") and not S:
            # thumb add/sub without flags: sp adjust, hi registers, adr
            if ops[0] == "sp" and len(ops) == 2:
                e("sp %s= %du;" % ("+" if op == "add" else "-", imm(ops[1])))
                return
            if len(ops) == 3 and ops[1] == "pc" and op == "add":
                rd = reg(ops[0])
                tgt = ((ins.addr + 4) & ~3) + imm(ops[2])
                self.adr_const[rd] = tgt
                e("/* adr %s, 0x%x */" % (rd, tgt))
                return
            if len(ops) == 2:
                rd = reg(ops[0])
                e("%s %s= %s;" % (rd, "+" if op == "add" else "-", reg(ops[1]) if not ops[1].startswith("#") else "0x%xu" % imm(ops[1])))
                return
            if len(ops) == 3 and ops[1] == "sp":
                rd = reg(ops[0])
                e("%s = sp + %du;" % (rd, imm(ops[2])))
                return
            die("thumb %s %r" % (base, ops))
        if not T and op == "add" and len(ops) == 3 and ops[1] == "pc":
            rd = reg(ops[0])
            tgt = ins.addr + 8 + imm(ops[2])
            self.adr_const[rd] = tgt
            e("/* adr %s, 0x%x */" % (rd, tgt))
            return
        if op == "mov" and T and not S and len(ops) == 2:
            e("%s = %s;" % (reg(ops[0]), reg(ops[1])))
            return
        if op in ("lsl", "lsr", "asr"):
            rd = reg(ops[0])
            if len(ops) == 2 and not ops[1].startswith("#"):
                # thumb register shift: rd = rd <shift> rs
                rs = reg(ops[1])
                e("{ u32 a_ = %s & 0xff; u32 c_ = c; %s = SHIFT_%s(%s, a_, &c_); c = c_; }" % (rs, rd, op.upper(), rd))
                if S:
                    self.set_nz(rd)
                return
            rm = reg(ops[1])
            k = imm(ops[2])
            val, carry = self.shift_imm(op, rm, k)
            if S:
                if carry:
                    e("c = %s;" % carry)
                e("%s = %s;" % (rd, val))
                self.set_nz(rd)
            else:
                e("%s = %s;" % (rd, val))
            return
        if op in ("mov", "mvn"):
            rd = reg(ops[0])
            val, carry = self.operand2(ops[1:], S)
            if op == "mvn":
                val = "~(%s)" % val
            if S and carry:
                e("c = %s;" % carry)
            e("%s = %s;" % (rd, val))
            if S:
                self.set_nz(rd)
            return
        if op == "neg":
            rd, rm = reg(ops[0]), reg(ops[1])
            e("{ u32 a_ = 0, b_ = %s, r_ = a_ - b_; c = (a_ >= b_); v = ((a_ ^ b_) & (a_ ^ r_)) >> 31; %s = r_; }" % (rm, rd))
            self.set_nz(rd)
            return
        if op in ("cmp", "cmn", "tst"):
            rn = reg(ops[0])
            val, carry = self.operand2(ops[1:], True)
            if op == "cmp":
                e("{ u32 a_ = %s, b_ = %s, r_ = a_ - b_; c = (a_ >= b_); v = ((a_ ^ b_) & (a_ ^ r_)) >> 31; n = r_ >> 31; z = (r_ == 0); }" % (rn, val))
            elif op == "cmn":
                e("{ u32 a_ = %s, b_ = %s, r_ = a_ + b_; c = (r_ < a_); v = ((a_ ^ r_) & (b_ ^ r_)) >> 31; n = r_ >> 31; z = (r_ == 0); }" % (rn, val))
            else:
                if carry:
                    e("c = %s;" % carry)
                e("{ u32 r_ = %s & %s; n = r_ >> 31; z = (r_ == 0); }" % (rn, val))
            return
        if op in ("add", "sub", "rsb", "and", "orr", "eor", "bic"):
            rd = reg(ops[0])
            if T and len(ops) == 2:
                rn, rest = rd, ops[1:]
            else:
                rn, rest = reg(ops[1]), ops[2:]
            val, carry = self.operand2(rest, S)
            if op in ("add", "sub", "rsb"):
                a, b = (rn, val) if op != "rsb" else (val, rn)
                if op == "add":
                    if S:
                        e("{ u32 a_ = %s, b_ = %s, r_ = a_ + b_; c = (r_ < a_); v = ((a_ ^ r_) & (b_ ^ r_)) >> 31; %s = r_; }" % (a, b, rd))
                    else:
                        e("%s = %s + %s;" % (rd, a, b))
                else:
                    if S:
                        e("{ u32 a_ = %s, b_ = %s, r_ = a_ - b_; c = (a_ >= b_); v = ((a_ ^ b_) & (a_ ^ r_)) >> 31; %s = r_; }" % (a, b, rd))
                    else:
                        e("%s = %s - %s;" % (rd, a, b))
            else:
                sym = {"and": "&", "orr": "|", "eor": "^"}
                if op == "bic":
                    e("%s = %s & ~(%s);" % (rd, rn, val))
                else:
                    if S and carry:
                        e("c = %s;" % carry)
                    e("%s = %s %s %s;" % (rd, rn, sym[op], val))
            if S:
                self.set_nz(rd)
            return
        die("unsupported instruction %s %s at 0x%x" % (ins.mnem, ops, ins.addr))

    def generate(self):
        self.calls = set()
        body = []
        for ins in self.insns:
            self.lines = []
            self.translate(ins)
            body.append((ins.addr, ins, self.lines))
        addrs = {a for a, _, _ in body}
        for t in self.targets:
            if t not in addrs:
                die("%s: branch target 0x%x outside function" % (self.name, t))
        out = ["u32 asmfn_%s(u32 a0, u32 a1, u32 a2, u32 a3)" % self.name, "{",
               "    u32 r0 = a0, r1 = a1, r2 = a2, r3 = a3, r4 = 0, r5 = 0, r6 = 0, r7 = 0;",
               "    u32 r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, lr = 0x0000dead;",
               "    u32 n = 0, z = 0, c = 0, v = 0;",
               "    u32 stack_[64];",
               "    u32 sp = (u32)(uintptr_t)&stack_[64];"]
        for addr, ins, lines in body:
            if addr in self.targets:
                out.append("L_%x:" % addr)
            out.append("    /* %04x %s %s */" % (addr, ins.mnem, ", ".join(ins.ops)))
            out += lines
        out.append("    return r0;")
        out.append("}")
        return out


SIG_RE = re.compile(r"@\s*Signature:\s*(.+?)\s*([A-Za-z_]\w*)\s*\((.*)\)\s*$")


def wrappers(src, names):
    out = []
    for line in open(src):
        m = SIG_RE.search(line)
        if not m or m.group(2) not in names:
            continue
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        ret = ret.replace("PSGSoundData", "PsgSoundData")
        plist = [p.strip().replace("PSGSoundData", "PsgSoundData") for p in params.split(",")] if params and params != "void" else []
        decl, args = [], []
        for i, p in enumerate(plist):
            # Split type and optional name
            mm = re.match(r"^(.*?)(\b[A-Za-z_]\w*)?$", p)
            typ = p
            pn = "p%d" % i
            if re.match(r"^(const\s+)?(struct\s+\w+|u8|u16|u32|s8|s16|s32|void)\s*\**\s*[A-Za-z_]\w*$", p):
                typ = re.sub(r"\s*[A-Za-z_]\w*$", "", p)
            decl.append("%s %s" % (typ, pn))
            if "*" in typ:
                args.append("(u32)(uintptr_t)%s" % pn)
            else:
                args.append("(u32)%s" % pn)
        while len(args) < 4:
            args.append("0")
        call = "asmfn_%s(%s)" % (name, ", ".join(args))
        out.append("%s %s(%s)" % (ret, name, ", ".join(decl) if decl else "void"))
        if ret == "void":
            out.append("{\n    %s;\n}\n" % call)
        elif "*" in ret:
            out.append("{\n    return (%s)(uintptr_t)%s;\n}\n" % (ret, call))
        else:
            out.append("{\n    return (%s)%s;\n}\n" % (ret, call))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--out", required=True)
    ap.add_argument("pairs", nargs="+", help="source.s=object.o")
    args = ap.parse_args()

    trs = []
    internal = set()
    for p in args.pairs:
        src, obj = p.split("=")
        tr = Translator(obj, args.objdump)
        tr.data_refs = set()
        tr.src = src
        trs.append(tr)
        for name, _, _ in tr.functions():
            internal.add(name)

    out = ["/* Generated by tools/port/asm2c.py from %s - do not edit. */" % ", ".join(t.src for t in trs),
           '#include "port/asm_rt.h"', ""]
    funcs_out, protos = [], []
    for tr in trs:
        for name, _, insns in tr.functions():
            g = FuncGen(tr, name, insns, internal)
            funcs_out += g.generate() + [""]
            protos.append("u32 asmfn_%s(u32 a0, u32 a1, u32 a2, u32 a3);" % name)
    refs = set()
    for tr in trs:
        refs |= tr.data_refs
    out += protos + [""]
    out += funcs_out
    for tr in trs:
        out += wrappers(tr.src, internal)
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print("asm2c: %d functions" % len(protos))


if __name__ == "__main__":
    main()
