"""Static PowerPC -> C++ recompiler for Star Wars Trilogy.

Generates one C++ function per PowerPC function discovered by analyze.py.
All CPU state lives in Ctx, so a branch to another function is
translated as "call + return" (tail call) without losing semantics.

Usage: python tools/recomp.py [data/crom.bin] [build/gen]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze import Memory, discover  # noqa: E402
from ppc import decode, sx  # noqa: E402

# What the translation leaves out, listed in the summary by address: OE=1 (XER
# overflow never computed) and Rc=1 on floating-point arithmetic (CR1 not
# updated). Neither has been seen to matter in this game; a change here would
# show up. 0xFFFFFFFF words (table filler inside function ranges, decoded as
# fnmadd.) are not counted.
LEFT_OUT = {"oe": set(), "fp_rc": set()}

FUNCS_PER_FILE = 40

SPR_LR, SPR_CTR, SPR_XER = 8, 9, 1
SPR_SIDE_EFFECTS = {22, 268, 269, 284, 285}   # DEC and TB go through the runtime
# Game hooks, found by reading the game's code (an empty set turns the hook
# off; IDLE_LOOPS is still to be found for Star Wars Trilogy):
# - IDLE_LOOPS: back branches of loops that only wait for the next interrupt
#   (vblank waits); while the runtime asks for it they jump to the next event
# - AIM_MULS: the mullis that turn the lever into the sight's yaw (x * 28)
#   and pitch (y * 21) in fn_0007C894 (0x10000 = 360 degrees); the runtime
#   adds the lever's precision between steps and widens the yaw so the sight
#   reaches the sides of a widescreen view (rt_aim_angle)
IDLE_LOOPS = set()
AIM_MULS = {0x0007C8C8: 0, 0x0007C8BC: 1}   # address: axis (0 = yaw from x * 28, 1 = pitch from y * 21)
# - GROUND_AIM_MULS: on foot and on the speeders (Hoth, Endor) the sight is a
#   position, not an angle: fn_00053F78 multiplies the lever (copied to
#   0x2260E0/E2) by 0.647 (x) and 0.490 (y); the runtime widens x and adds the
#   lever's precision (rt_ground_aim)
GROUND_AIM_MULS = {0x00053FB4: 0, 0x00053FEC: 1}
# - BASE_AIM_MULS: in Hoth's base (the tunnels and the hangar, on foot)
#   fn_000E2998 places the sight itself: the lever's signed bytes (0x22153C/D)
#   times 244.8 / 128 (x, negated) and 188.8 / 128 (y), from 0x23B618 +0x4C
#   / +0x50, at 128 units ahead; the multiplier is the instruction's frC.
#   Widened and made finer as on the snow (rt_base_aim)
BASE_AIM_MULS = {0x000E2A3C: 0, 0x000E2A38: 1}


def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


def rz(n):
    """(rA|0): r0 as the base means 0."""
    return "0" if n == 0 else f"c.r[{n}]"


class FuncGen:
    def __init__(self, mem, entries, jumptables, entry):
        self.mem = mem
        self.entries = entries
        self.jt = jumptables
        self.entry = entry
        self.body = self._collect()
        self.labels = set()
        self.lines = []

    def _collect(self):
        seen = {}
        work = [self.entry]
        while work:
            a = work.pop()
            while self.mem.valid(a) and a not in seen:
                if a != self.entry and a in self.entries:
                    break
                ins = decode(self.mem.r32(a), a)
                seen[a] = ins
                if ins.mnem == ".word":
                    break
                if ins.branch is not None and not ins.call:
                    work.append(ins.branch)
                if a in self.jt:
                    work.extend(self.jt[a])
                if ins.ends_block:
                    break
                a += 4
        return seen

    # ---- targets ----
    def goto(self, t, src):
        """Code to branch to t (within the function or as a tail call)."""
        if t in self.body:
            self.labels.add(t)
            poll = f"{'IDLE' if src in IDLE_LOOPS else 'POLL'}(0x{src:08X}); " if t <= src else ""
            return f"{poll}goto L_{t:08X};"
        if t in self.entries:
            return f"fn_{t:08X}(c); return;"
        return f"rt_call(c, 0x{t:08X}); return;"

    def call(self, t):
        if t in self.entries:
            return f"fn_{t:08X}(c);"
        return f"rt_call(c, 0x{t:08X});"

    def bc_cond(self, bo, bi):
        pre = ""
        parts = []
        if not bo & 4:
            pre = "c.ctr--; "
            parts.append("c.ctr == 0" if bo & 2 else "c.ctr != 0")
        if not bo & 16:
            parts.append(f"crbit(c, {bi})" if bo & 8 else f"!crbit(c, {bi})")
        return pre, " && ".join(parts) if parts else "true"

    # ---- generation ----
    def generate(self):
        out = []
        addrs = sorted(self.body)
        code = {}
        for a in addrs:
            code[a] = self.insn(self.body[a])
            if a in AIM_MULS:
                rd = (self.body[a].word >> 21) & 31
                code[a] = code[a] + [f"c.r[{rd}] = rt_aim_angle({AIM_MULS[a]}, c.r[{rd}]);"]
            if a in GROUND_AIM_MULS:
                fd = (self.body[a].word >> 21) & 31
                code[a] = code[a] + [f"c.f[{fd}].d = rt_ground_aim({GROUND_AIM_MULS[a]}, c.f[{fd}].d);"]
            if a in BASE_AIM_MULS:
                w = self.body[a].word
                fd, fc = (w >> 21) & 31, (w >> 6) & 31
                code[a] = code[a] + [f"c.f[{fd}].d = rt_base_aim({BASE_AIM_MULS[a]}, c.f[{fd}].d, c.f[{fc}].d);"]
        # The labels in use are now known
        out.append(f"void fn_{self.entry:08X}(Ctx& c) {{")
        out.append(f"    ENTER(0x{self.entry:08X});")
        # The body is emitted in address order, but it may contain code below
        # the entry point (shared tails reached by branches): jump to the entry.
        if addrs and addrs[0] != self.entry:
            self.labels.add(self.entry)
            out.append(f"    goto L_{self.entry:08X};")
        pending = 0
        for i, a in enumerate(addrs):
            ins = self.body[a]
            if a in self.labels:
                if pending:
                    out.append(f"    c.cycles += {pending};")
                    pending = 0
                out.append(f"L_{a:08X}:")
            pending += 1
            stmts = code[a]
            flow = ins.branch is not None or ins.indirect or ins.mnem in ("sc", ".word")
            if flow and pending:
                out.append(f"    c.cycles += {pending};")
                pending = 0
            out.append(f"    /* {a:08X} {ins.text()} */")
            for s in stmts:
                out.append("    " + s)
            nxt = (a + 4) & 0xFFFFFFFF   # wrap at the top of the address space
            if not ins.ends_block and ins.mnem != ".word" and nxt not in self.body:
                if pending:
                    out.append(f"    c.cycles += {pending};")
                    pending = 0
                out.append("    " + self.goto(nxt, a))
        if pending:
            out.append(f"    c.cycles += {pending};")
        out.append("}")
        return out

    def insn(self, ins):
        w, a = ins.word, ins.addr
        op = w >> 26
        d = (w >> 21) & 31
        ra = (w >> 16) & 31
        rb = (w >> 11) & 31
        rc = (w >> 6) & 31
        rec = w & 1
        simm = sx(w & 0xFFFF, 16)
        uimm = w & 0xFFFF
        R = lambda n: f"c.r[{n}]"  # noqa: E731
        F = lambda n: f"c.f[{n}].d"  # noqa: E731
        FU = lambda n: f"c.f[{n}].u"  # noqa: E731
        ea_d = f"{rz(ra)} + {simm}" if ra else f"uint32_t({simm})"
        ea_x = f"{rz(ra)} + c.r[{rb}]"
        cr0 = [f"cr0(c, {R(ra)});"] if rec else []
        cr0d = [f"cr0(c, {R(d)});"] if rec else []

        if ins.mnem == ".word":
            return [f"rt_bad(c, 0x{a:08X}); return;"]

        # ---- branches ----
        if op == 18:
            if w & 1:
                return [f"c.lr = 0x{a + 4:08X};", self.call(ins.branch)]
            return [self.goto(ins.branch, a)]
        if op == 16:
            bo, bi = d, ra
            pre, cond = self.bc_cond(bo, bi)
            if w & 1:
                return [f"{pre}if ({cond}) {{ c.lr = 0x{a + 4:08X}; {self.call(ins.branch)} }}"]
            if cond == "true":
                return [pre + self.goto(ins.branch, a)]
            return [f"{pre}if ({cond}) {{ {self.goto(ins.branch, a)} }}"]
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo in (16, 528):
                bo, bi = d, ra
                pre, cond = self.bc_cond(bo, bi)
                lk = w & 1
                if xo == 16:
                    if lk:
                        act = f"{{ uint32_t t = c.lr; c.lr = 0x{a + 4:08X}; rt_call(c, t); }}"
                    else:
                        act = "return;"
                else:
                    if lk:
                        act = f"{{ c.lr = 0x{a + 4:08X}; rt_call(c, c.ctr); }}"
                    else:
                        act = self.switch_ctr(a)
                if cond == "true":
                    return [pre + act]
                return [f"{pre}if ({cond}) {act}"]
            if xo == 0:
                return [f"setcrf(c, {d >> 2}, (c.cr >> {28 - 4 * (ra >> 2)}) & 0xF);"]
            if xo == 50:
                return ["return; /* rfi */"]
            if xo == 150:
                return []
            crops = {257: "{a} & {b}", 129: "{a} & ~{b}", 289: "~({a} ^ {b})", 225: "~({a} & {b})",
                     33: "~({a} | {b})", 449: "{a} | {b}", 417: "{a} | ~{b}", 193: "{a} ^ {b}"}
            if xo in crops:
                e = crops[xo].format(a=f"crbit(c, {ra})", b=f"crbit(c, {rb})")
                return [f"setcrbit(c, {d}, {e});"]
        # ---- integer with immediate ----
        if op == 14:
            return [f"{R(d)} = {rz(ra)} + {simm};" if ra else f"{R(d)} = uint32_t({simm});"]
        if op == 15:
            v = (uimm << 16) & 0xFFFFFFFF
            return [f"{R(d)} = {rz(ra)} + 0x{v:08X}u;" if ra else f"{R(d)} = 0x{v:08X}u;"]
        if op in (12, 13):
            s = [f"{R(d)} = addca(c, {R(ra)}, 0x{simm & 0xFFFFFFFF:08X}u, 0);"]
            return s + ([f"cr0(c, {R(d)});"] if op == 13 else [])
        if op == 8:
            return [f"{R(d)} = addca(c, ~{R(ra)}, 0x{simm & 0xFFFFFFFF:08X}u, 1);"]
        if op == 7:
            return [f"{R(d)} = {R(ra)} * uint32_t({simm});"]   # unsigned: wraps like the hardware
        if op == 10:
            return [f"cmpu(c, {d >> 2}, {R(ra)}, 0x{uimm:X}u);"]
        if op == 11:
            return [f"cmps(c, {d >> 2}, int32_t({R(ra)}), {simm});"]
        if op == 24:
            return [] if w == 0x60000000 else [f"{R(ra)} = {R(d)} | 0x{uimm:X}u;"]
        if op == 25:
            return [f"{R(ra)} = {R(d)} | 0x{uimm << 16:08X}u;"]
        if op == 26:
            return [f"{R(ra)} = {R(d)} ^ 0x{uimm:X}u;"]
        if op == 27:
            return [f"{R(ra)} = {R(d)} ^ 0x{uimm << 16:08X}u;"]
        if op == 28:
            return [f"{R(ra)} = {R(d)} & 0x{uimm:X}u;", f"cr0(c, {R(ra)});"]
        if op == 29:
            return [f"{R(ra)} = {R(d)} & 0x{uimm << 16:08X}u;", f"cr0(c, {R(ra)});"]
        if op in (20, 21, 23):
            mb, me = (w >> 6) & 31, (w >> 1) & 31
            m = mask(mb, me)
            rot = f"rotl({R(d)}, {rb})" if op != 23 else f"rotl({R(d)}, {R(rb)})"
            if op == 20:
                s = f"{R(ra)} = ({rot} & 0x{m:08X}u) | ({R(ra)} & 0x{~m & 0xFFFFFFFF:08X}u);"
            else:
                s = f"{R(ra)} = {rot} & 0x{m:08X}u;"
            return [s] + cr0
        if op == 3:
            return []  # twi: debug traps, ignored
        if op == 17:
            return [f"rt_sc(c, 0x{a:08X});"]
        # ---- D-form load/store ----
        if 32 <= op <= 55:
            upd = op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55)
            ea = f"c.r[{ra}] + {simm}" if upd else ea_d
            body = {
                32: f"{R(d)} = rd32(ea);", 33: f"{R(d)} = rd32(ea);",
                34: f"{R(d)} = rd8(ea);", 35: f"{R(d)} = rd8(ea);",
                36: f"wr32(ea, {R(d)});", 37: f"wr32(ea, {R(d)});",
                38: f"wr8(ea, uint8_t({R(d)}));", 39: f"wr8(ea, uint8_t({R(d)}));",
                40: f"{R(d)} = rd16(ea);", 41: f"{R(d)} = rd16(ea);",
                42: f"{R(d)} = uint32_t(int16_t(rd16(ea)));", 43: f"{R(d)} = uint32_t(int16_t(rd16(ea)));",
                44: f"wr16(ea, uint16_t({R(d)}));", 45: f"wr16(ea, uint16_t({R(d)}));",
                46: f"for (uint32_t i = {d}, p = ea; i < 32; i++, p += 4) c.r[i] = rd32(p);",
                47: f"for (uint32_t i = {d}, p = ea; i < 32; i++, p += 4) wr32(p, c.r[i]);",
                48: f"{F(d)} = f2d(rd32(ea));", 49: f"{F(d)} = f2d(rd32(ea));",
                50: f"{FU(d)} = rd64(ea);", 51: f"{FU(d)} = rd64(ea);",
                52: f"wr32(ea, d2fbits({F(d)}));", 53: f"wr32(ea, d2fbits({F(d)}));",
                54: f"wr64(ea, {FU(d)});", 55: f"wr64(ea, {FU(d)});",
            }[op]
            s = f"{{ uint32_t ea = {ea}; {body}"
            if upd:
                s += f" c.r[{ra}] = ea;"
            return [s + " }"]
        if op == 31:
            return self.op31(ins, w, a, d, ra, rb, rec, R, F, FU, ea_x, cr0, cr0d)
        if op == 59:
            xo = (w >> 1) & 31
            e = {18: f"{F(ra)} / {F(rb)}", 20: f"{F(ra)} - {F(rb)}", 21: f"{F(ra)} + {F(rb)}",
                 22: f"std::sqrt({F(rb)})", 24: f"1.0 / {F(rb)}", 25: f"{F(ra)} * {F(rc)}",
                 28: f"{F(ra)} * {F(rc)} - {F(rb)}", 29: f"{F(ra)} * {F(rc)} + {F(rb)}",
                 30: f"-({F(ra)} * {F(rc)} - {F(rb)})", 31: f"-({F(ra)} * {F(rc)} + {F(rb)})"}.get(xo)
            if e:
                if rec and w != 0xFFFFFFFF:
                    LEFT_OUT["fp_rc"].add(a)
                return [f"{F(d)} = rs({e});"]
        if op == 63:
            xo5 = (w >> 1) & 31
            xo = (w >> 1) & 0x3FF
            e = {18: f"{F(ra)} / {F(rb)}", 20: f"{F(ra)} - {F(rb)}", 21: f"{F(ra)} + {F(rb)}",
                 22: f"std::sqrt({F(rb)})", 23: f"({F(ra)} >= 0.0 ? {F(rc)} : {F(rb)})",
                 25: f"{F(ra)} * {F(rc)}", 26: f"frsqrte({F(rb)})",
                 28: f"{F(ra)} * {F(rc)} - {F(rb)}", 29: f"{F(ra)} * {F(rc)} + {F(rb)}",
                 30: f"-({F(ra)} * {F(rc)} - {F(rb)})", 31: f"-({F(ra)} * {F(rc)} + {F(rb)})"}.get(xo5)
            if e and xo5 >= 18:
                if rec and w != 0xFFFFFFFF:
                    LEFT_OUT["fp_rc"].add(a)
                return [f"{F(d)} = {e};"]
            if xo in (0, 32):
                return [f"fcmp(c, {d >> 2}, {F(ra)}, {F(rb)});"]
            simple = {12: f"{F(d)} = rs({F(rb)});", 14: f"{FU(d)} = fctiw({F(rb)}, false);",
                      15: f"{FU(d)} = fctiw({F(rb)}, true);",
                      40: f"{FU(d)} = {FU(rb)} ^ 0x8000000000000000ull;",
                      72: f"{FU(d)} = {FU(rb)};",
                      136: f"{FU(d)} = {FU(rb)} | 0x8000000000000000ull;",
                      264: f"{FU(d)} = {FU(rb)} & 0x7FFFFFFFFFFFFFFFull;",
                      583: f"{FU(d)} = c.fpscr;",
                      38: f"c.fpscr |= 0x{1 << (31 - d):08X}u;",
                      70: f"c.fpscr &= ~0x{1 << (31 - d):08X}u;"}
            if xo in simple:
                return [simple[xo]]
            if xo == 711:
                fm = (w >> 17) & 0xFF
                m = 0
                for i in range(8):
                    if fm & (0x80 >> i):
                        m |= 0xF << (28 - 4 * i)
                return [f"c.fpscr = (c.fpscr & ~0x{m:08X}u) | (uint32_t({FU(rb)}) & 0x{m:08X}u);"]
            if xo == 134:
                fld, imm = d >> 2, (w >> 12) & 15
                return [f"c.fpscr = (c.fpscr & ~0x{0xF << (28 - 4 * fld):08X}u) | 0x{imm << (28 - 4 * fld):08X}u;"]
            if xo == 64:
                return [f"setcrf(c, {d >> 2}, (c.fpscr >> {28 - 4 * (ra >> 2)}) & 0xF);"]
        return [f"rt_bad(c, 0x{a:08X}); return;"]

    def switch_ctr(self, a):
        targets = sorted(set(self.jt.get(a, [])))
        cases = []
        for t in targets:
            cases.append(f"case 0x{t:08X}u: {self.goto(t, a)}")
        cases.append("default: rt_call(c, c.ctr); return;")
        return "switch (c.ctr) { " + " ".join(cases) + " }"

    def op31(self, ins, w, a, d, ra, rb, rec, R, F, FU, ea_x, cr0, cr0d):
        xo = (w >> 1) & 0x3FF
        xo9 = (w >> 1) & 0x1FF
        A, B, D = R(ra), R(rb), R(d)
        arith = {
            8: [f"{D} = addca(c, ~{A}, {B}, 1);"],
            10: [f"{D} = addca(c, {A}, {B}, 0);"],
            11: [f"{D} = uint32_t((uint64_t({A}) * {B}) >> 32);"],
            40: [f"{D} = {B} - {A};"],
            75: [f"{D} = uint32_t((int64_t(int32_t({A})) * int32_t({B})) >> 32);"],
            104: [f"{D} = 0u - {A};"],
            136: [f"{D} = addca(c, ~{A}, {B}, ca(c));"],
            138: [f"{D} = addca(c, {A}, {B}, ca(c));"],
            200: [f"{D} = addca(c, ~{A}, 0, ca(c));"],
            202: [f"{D} = addca(c, {A}, 0, ca(c));"],
            232: [f"{D} = addca(c, ~{A}, 0xFFFFFFFFu, ca(c));"],
            234: [f"{D} = addca(c, {A}, 0xFFFFFFFFu, ca(c));"],
            235: [f"{D} = {A} * {B};"],
            266: [f"{D} = {A} + {B};"],
            459: [f"{D} = divwu({A}, {B});"],
            491: [f"{D} = divw(int32_t({A}), int32_t({B}));"],
        }
        if xo9 in arith and xo not in (0, 32):
            if xo & 0x200:
                LEFT_OUT["oe"].add(a)
            return arith[xo9] + cr0d
        logic = {28: f"{D} & {B}", 60: f"{D} & ~{B}", 124: f"~({D} | {B})", 284: f"~({D} ^ {B})",
                 316: f"{D} ^ {B}", 412: f"{D} | ~{B}", 444: f"{D} | {B}", 476: f"~({D} & {B})",
                 24: f"(({B} & 0x20) ? 0u : {D} << ({B} & 31))",
                 536: f"(({B} & 0x20) ? 0u : {D} >> ({B} & 31))",
                 792: f"sraw(c, {D}, {B})", 824: f"sraw(c, {D}, {rb})",
                 26: f"clz32({D})", 922: f"uint32_t(int16_t({D}))", 954: f"uint32_t(int8_t({D}))"}
        if xo in logic:
            return [f"{A} = {logic[xo]};"] + cr0
        if xo == 0:
            return [f"cmps(c, {d >> 2}, int32_t({A}), int32_t({B}));"]
        if xo == 32:
            return [f"cmpu(c, {d >> 2}, {A}, {B});"]
        upd = xo in (55, 119, 183, 247, 311, 375, 439, 567, 631, 695, 759)
        mem = {
            20: f"{D} = rd32(ea); c.reserve = ea;", 23: f"{D} = rd32(ea);", 55: f"{D} = rd32(ea);",
            87: f"{D} = rd8(ea);", 119: f"{D} = rd8(ea);", 279: f"{D} = rd16(ea);", 311: f"{D} = rd16(ea);",
            343: f"{D} = uint32_t(int16_t(rd16(ea)));", 375: f"{D} = uint32_t(int16_t(rd16(ea)));",
            534: f"{D} = bswap32(rd32(ea));", 790: f"{D} = bswap16(rd16(ea));",
            150: f"wr32(ea, {D}); setcrf(c, 0, 2 | so(c));",
            151: f"wr32(ea, {D});", 183: f"wr32(ea, {D});", 215: f"wr8(ea, uint8_t({D}));",
            247: f"wr8(ea, uint8_t({D}));", 407: f"wr16(ea, uint16_t({D}));", 439: f"wr16(ea, uint16_t({D}));",
            662: f"wr32(ea, bswap32({D}));", 918: f"wr16(ea, bswap16(uint16_t({D})));",
            535: f"{F(d)} = f2d(rd32(ea));", 567: f"{F(d)} = f2d(rd32(ea));",
            599: f"{FU(d)} = rd64(ea);", 631: f"{FU(d)} = rd64(ea);",
            663: f"wr32(ea, d2fbits({F(d)}));", 695: f"wr32(ea, d2fbits({F(d)}));",
            727: f"wr64(ea, {FU(d)});", 759: f"wr64(ea, {FU(d)});",
            983: f"wr32(ea, uint32_t({FU(d)}));",
            1014: "for (int i = 0; i < 32; i += 4) wr32((ea & ~31u) + i, 0);",
        }
        if xo in mem:
            ea = f"c.r[{ra}] + c.r[{rb}]" if upd else ea_x
            s = f"{{ uint32_t ea = {ea}; {mem[xo]}"
            if upd:
                s += f" c.r[{ra}] = ea;"
            return [s + " }"]
        if xo in (54, 86, 246, 278, 470, 982, 306, 566, 370, 598, 854, 4, 310, 438, 978, 1010):
            return []  # cache, TLB, sync and traps: no effect on PC
        if xo == 19:
            return [f"{D} = c.cr;"]
        if xo == 144:
            crm = (w >> 12) & 0xFF
            m = 0
            for i in range(8):
                if crm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return [f"c.cr = (c.cr & ~0x{m:08X}u) | ({D} & 0x{m:08X}u);"]
        if xo == 83:
            return [f"{D} = c.msr;"]
        if xo == 146:
            return [f"rt_mtmsr(c, {D});"]
        if xo == 210:
            return [f"c.sr[{ra & 15}] = {D};"]
        if xo == 242:
            return [f"c.sr[{B} >> 28] = {D};"]
        if xo == 595:
            return [f"{D} = c.sr[{ra & 15}];"]
        if xo == 659:
            return [f"{D} = c.sr[{B} >> 28];"]
        if xo in (339, 371):
            spr = ra | (rb << 5)
            if spr == SPR_LR:
                return [f"{D} = c.lr;"]
            if spr == SPR_CTR:
                return [f"{D} = c.ctr;"]
            if spr == SPR_XER:
                return [f"{D} = c.xer;"]
            if spr in SPR_SIDE_EFFECTS:
                return [f"{D} = rt_mfspr(c, {spr});"]
            return [f"{D} = c.spr[{spr}];"]
        if xo == 467:
            spr = ra | (rb << 5)
            if spr == SPR_LR:
                return [f"c.lr = {D};"]
            if spr == SPR_CTR:
                return [f"c.ctr = {D};"]
            if spr == SPR_XER:
                return [f"c.xer = {D};"]
            if spr in SPR_SIDE_EFFECTS:
                return [f"rt_mtspr(c, {spr}, {D});"]
            return [f"c.spr[{spr}] = {D};"]
        if xo == 512:
            return [f"setcrf(c, {d >> 2}, c.xer >> 28); c.xer &= 0x0FFFFFFFu;"]
        if xo == 597:
            return [f"rt_lswi(c, {rz(ra)}, {d}, {rb or 32});"]
        if xo == 725:
            return [f"rt_stswi(c, {rz(ra)}, {d}, {rb or 32});"]
        return [f"rt_bad(c, 0x{a:08X}); return;"]


def main():
    rom_path = sys.argv[1] if len(sys.argv) > 1 else "data/crom.bin"
    outdir = sys.argv[2] if len(sys.argv) > 2 else "build/gen"
    os.makedirs(outdir, exist_ok=True)
    mem = Memory(open(rom_path, "rb").read())
    d = discover(mem)
    entries = sorted(e for e in d["entries"] if mem.valid(e))
    entry_set = set(entries)
    # Entries found by heuristics that land in data (invalid
    # instructions) are discarded. (The generators built here are not reused
    # below: a function's body and calls depend on the final entry set.)
    dropped = {e for e in entries if e in d["heuristic"] and any(
        i.mnem == ".word" for i in FuncGen(mem, entry_set, d["jumptables"], e).body.values())}
    entries = [e for e in entries if e not in dropped]
    entry_set = set(entries)
    print(f"{len(dropped)} heuristic entries dropped for landing in data")
    bad = 0
    files = []
    for k in range(0, len(entries), FUNCS_PER_FILE):
        chunk = entries[k:k + FUNCS_PER_FILE]
        name = f"fn_{k // FUNCS_PER_FILE:03d}.cpp"
        lines = ['#include "fns.h"', ""]
        for e in chunk:
            g = FuncGen(mem, entry_set, d["jumptables"], e)
            code = g.generate()
            bad += sum(1 for ln in code if "rt_bad" in ln and "/*" not in ln)
            lines += code + [""]
        new = "\n".join(lines)
        path = os.path.join(outdir, name)
        if not os.path.exists(path) or open(path).read() != new:
            open(path, "w").write(new)
        files.append(name)
    with open(os.path.join(outdir, "fns.h"), "w") as fo:
        fo.write('#pragma once\n#include "ppc_rt.h"\n\n')
        for e in entries:
            fo.write(f"void fn_{e:08X}(Ctx& c);\n")
        fo.write("\nstruct FnEntry { uint32_t addr; void (*fn)(Ctx&); };\n")
        fo.write("extern const FnEntry g_fn_table[];\nextern const int g_fn_count;\n")
    with open(os.path.join(outdir, "table.cpp"), "w") as fo:
        fo.write('#include "fns.h"\n\nconst FnEntry g_fn_table[] = {\n')
        for e in entries:
            fo.write(f"    {{0x{e:08X}u, fn_{e:08X}}},\n")
        fo.write(f"}};\nconst int g_fn_count = {len(entries)};\n")
    with open(os.path.join(outdir, "files.cmake"), "w") as fo:
        fo.write("set(GEN_SOURCES\n" + "".join(f"    ${{GEN_DIR}}/{n}\n" for n in files + ["table.cpp"]) + ")\n")
    print(f"{len(entries)} functions in {len(files)} files, {bad} untranslated instructions")
    if LEFT_OUT["oe"] or LEFT_OUT["fp_rc"]:
        print(f"warning: {len(LEFT_OUT['oe'])} instructions with OE=1 (XER overflow not computed), "
              f"{len(LEFT_OUT['fp_rc'])} floating-point ones with Rc=1 (CR1 not updated)")


if __name__ == "__main__":
    main()
