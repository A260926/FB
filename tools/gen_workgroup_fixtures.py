#!/usr/bin/env python3
"""Generate checks/workgroup_fixtures.h.

Development helper only.  The generated header is checked in and the test
suite itself needs nothing but a C compiler.  Enum values are read from
inc/spvm/spirv.h so the fixtures stay in sync with the VM headers.

Run from the repository root:

    python3 tools/gen_workgroup_fixtures.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "inc", "spvm", "spirv.h")
OUTPUT = os.path.join(ROOT, "checks", "workgroup_fixtures.h")

ENUM = {}
with open(HEADER, "r") as f:
    for line in f:
        m = re.match(r"\s*(Spv\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*,?", line)
        if m:
            ENUM[m.group(1)] = int(m.group(2), 0)


def E(name):
    return ENUM[name]


class Id(int):
    """Mark an operand as an id so the listing prints it as %name."""


class Module:
    def __init__(self, cname, title):
        self.cname = cname
        self.title = title
        self.words = []
        self.next_id = 1
        self.symbols = {}
        self.listing = []

    def nid(self, name):
        if name in self.symbols:
            return Id(self.symbols[name])
        i = self.next_id
        self.next_id += 1
        self.symbols[name] = i
        return Id(i)

    def sym(self, value):
        for name, i in self.symbols.items():
            if i == value:
                return "%" + name
        return "%%%d" % value

    # --- instructions --------------------------------------------------
    def emit(self, opname, operands, result=None, text=None, result_at=0):
        wc = len(operands) + 1
        self.words.append((wc << 16) | E(opname))
        self.words.extend(int(op) for op in operands)

        if text is None:
            shown = list(operands)
            # ids that are result or result type are printed on the left or
            # as part of the type annotation
            if result is not None:
                if result_at == 0 and shown and shown[0] == result:
                    shown = shown[1:]
                elif result_at == 1 and len(shown) > 1 and shown[1] == result:
                    shown = shown[:1] + shown[2:]
            rendered = " ".join(self.sym(op) if isinstance(op, Id) else str(op)
                                for op in shown)
            name = opname[3:] if opname.startswith("Spv") else opname
            lhs = ("%s = " % self.sym(result)) if result is not None else ""
            text = lhs + name + ((" " + rendered) if rendered else "")
        self.listing.append(text)

    def header(self, bound):
        return [0x07230203, 0x00010000, 0x00000000, bound, 0x00000000]

    def string_words(self, value):
        data = value.encode("ascii") + b"\0"
        while len(data) % 4:
            data += b"\0"
        return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]

    def string(self, value):
        """Return (words, text) for a quoted SPIR-V literal string."""
        return self.string_words(value), '"%s"' % value

    # --- module header --------------------------------------------------
    def capability(self, cap):
        self.emit("SpvOpCapability", [E(cap)], text="OpCapability " + cap)

    def memory_model(self):
        self.emit("SpvOpMemoryModel",
                  [E("SpvAddressingModelLogical"), E("SpvMemoryModelGLSL450")],
                  text="OpMemoryModel Logical GLSL450")

    def entry_point(self, name, fn, interface, local_size):
        words, quoted = self.string(name)
        self.emit("SpvOpEntryPoint", [E("SpvExecutionModelGLCompute"), fn] + words + interface,
                  text="OpEntryPoint GLCompute %s %s %s" % (
                      self.sym(fn), quoted,
                      " ".join(self.sym(i) for i in interface)))
        self.emit("SpvOpExecutionMode",
                  [fn, E("SpvExecutionModeLocalSize")] + list(local_size),
                  text="OpExecutionMode %s LocalSize %d %d %d"
                       % ((self.sym(fn),) + tuple(local_size)))

    def name(self, ident, name):
        words, quoted = self.string(name)
        self.emit("SpvOpName", [ident] + words,
                  text="OpName %s %s" % (self.sym(ident), quoted))

    def builtin(self, ident, builtin):
        self.emit("SpvOpDecorate", [ident, E("SpvDecorationBuiltIn"), E(builtin)],
                  text="OpDecorate %s BuiltIn %s" % (self.sym(ident), builtin))

    # --- types / constants / variables ---------------------------------
    def t(self, opname, name, *operands, **kwargs):
        ident = self.nid(name)
        self.emit(opname, [ident] + list(operands), result=ident,
                  text=kwargs.get("text"))
        return ident
    def type_void(self, name="void"):
        return self.t("SpvOpTypeVoid", name)

    def type_bool(self, name="bool"):
        return self.t("SpvOpTypeBool", name)

    def type_int(self, name, signed=False):
        return self.t("SpvOpTypeInt", name, 32, 1 if signed else 0)

    def type_vec(self, name, elem, count):
        return self.t("SpvOpTypeVector", name, elem, count)

    def type_array(self, name, elem, length_id):
        return self.t("SpvOpTypeArray", name, elem, length_id)

    def type_struct(self, name, members):
        return self.t("SpvOpTypeStruct", name, *members)

    def type_pointer(self, name, storage, pointee):
        ident = self.nid(name)
        self.emit("SpvOpTypePointer", [ident, E("SpvStorageClass" + storage), pointee],
                  result=ident,
                  text="%s = OpTypePointer %s %s" % (
                      self.sym(ident), storage, self.sym(pointee)))
        return ident

    def type_function(self, name, ret, *params):
        return self.t("SpvOpTypeFunction", name, ret, *params)

    def const(self, name, typ, value, text=None):
        ident = self.nid(name)
        self.emit("SpvOpConstant", [typ, ident, value], result=ident,
                  result_at=1, text=text)
        return ident

    def var(self, name, ptr_type, storage, text=None):
        ident = self.nid(name)
        self.emit("SpvOpVariable",
                  [ptr_type, ident, E("SpvStorageClass" + storage)],
                  result=ident, result_at=1,
                  text=text or ("%s = OpVariable %s %s" % (
                      self.sym(ident), self.sym(ptr_type), storage)))
        return ident

    def function(self, name, ret_type, fn_type):
        ident = self.nid(name)
        self.emit("SpvOpFunction", [ret_type, ident, 0, fn_type], result=ident,
                  result_at=1,
                  text="%s = OpFunction %s None %s" % (
                      self.sym(ident), self.sym(ret_type), self.sym(fn_type)))
        return ident

    def param(self, name, typ):
        ident = self.nid(name)
        self.emit("SpvOpFunctionParameter", [typ, ident], result=ident, result_at=1)
        return ident

    def label(self, name):
        ident = self.nid(name)
        self.emit("SpvOpLabel", [ident], result=ident)
        return ident

    # --- bodies ----------------------------------------------------------
    def ld(self, name, typ, ptr):
        ident = self.nid(name)
        self.emit("SpvOpLoad", [typ, ident, ptr], result=ident, result_at=1)
        return ident

    def st(self, ptr, val):
        self.emit("SpvOpStore", [ptr, val])

    def access(self, name, typ, base, *indices):
        ident = self.nid(name)
        self.emit("SpvOpAccessChain", [typ, ident, base] + list(indices),
                  result=ident, result_at=1)
        return ident

    def binop(self, opname, name, typ, a, b):
        ident = self.nid(name)
        self.emit(opname, [typ, ident, a, b], result=ident, result_at=1)
        return ident

    def compare(self, opname, name, typ, a, b):
        ident = self.nid(name)
        if typ == "bool":
            typ = self.bool_type
        self.emit(opname, [typ, ident, a, b], result=ident, result_at=1)
        return ident

    def make_bool_type(self, name="bool"):
        self.bool_type = self.t("SpvOpTypeBool", name)
        return self.bool_type

    def extract(self, name, typ, vec, index):
        ident = self.nid(name)
        self.emit("SpvOpCompositeExtract", [typ, ident, vec, index], result_at=1,
                  text="%s = OpCompositeExtract %s %s %d" % (
                      self.sym(ident), self.sym(typ), self.sym(vec), index))
        return ident

    def call(self, name, ret_type, function, *args):
        ident = self.nid(name)
        self.emit("SpvOpFunctionCall", [ret_type, ident, function] + list(args),
                  result=ident, result_at=1)
        return ident

    def copy_memory(self, target, source):
        self.emit("SpvOpCopyMemory", [target, source])

    def barrier(self, scope, semantics, text=None):
        self.emit("SpvOpControlBarrier", [scope, scope, semantics], text=text)

    def branch(self, label):
        self.emit("SpvOpBranch", [label], text="OpBranch " + self.sym(label))

    def branch_cond(self, cond, true_label, false_label):
        self.emit("SpvOpBranchConditional", [cond, true_label, false_label],
                  text="OpBranchConditional %s %s %s" % (
                      self.sym(cond), self.sym(true_label), self.sym(false_label)))

    def ret(self):
        self.emit("SpvOpReturn", [], text="OpReturn")

    def end(self):
        self.emit("SpvOpFunctionEnd", [], text="OpFunctionEnd")


SCOPE_WG = E("SpvScopeWorkgroup")
SEM_AR_WGMEM = (E("SpvMemorySemanticsAcquireReleaseMask") |
                E("SpvMemorySemanticsWorkgroupMemoryMask"))


def _decls(m, name, ptr_type, storage):
    m.name(m.nid(name), name)


# ----------------------------------------------------------------------
def build_swap():
    m = Module("swap", "four invocations swapping neighbours through a shared array")

    m.capability("SpvCapabilityShader")
    m.memory_model()

    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    v2u = m.type_vec("v2uint", uint, 2)
    c4 = m.const("c4", uint, 4)
    arr4 = m.type_array("arr4_uint", uint, c4)
    st = m.type_struct("st", [uint, uint])
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    p_in_u = m.type_pointer("p_in_uint", "Input", uint)
    p_wg_u = m.type_pointer("p_wg_uint", "Workgroup", uint)
    p_wg_arr4 = m.type_pointer("p_wg_arr4_uint", "Workgroup", arr4)
    p_wg_v2 = m.type_pointer("p_wg_v2uint", "Workgroup", v2u)
    p_wg_st = m.type_pointer("p_wg_st", "Workgroup", st)
    p_pv_u = m.type_pointer("p_private_uint", "Private", uint)
    fnv_pu_u = m.type_function("fn_void_ptrworkgroup_uint_uint", void, p_wg_u, uint)

    u0 = m.const("u0", uint, 0)
    u1 = m.const("u1", uint, 1)
    u2 = m.const("u2", uint, 2)
    u3 = m.const("u3", uint, 3)
    u10 = m.const("u10", uint, 10)
    u77 = m.const("u77", uint, 77)
    u99 = m.const("u99", uint, 99)
    sc = m.const("scope_workgroup", uint, SCOPE_WG,
                 text="%scope_workgroup = OpConstant %uint 2 ; Workgroup")
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM,
                  text="%sem_ar_wgmem = OpConstant %uint 264 ; AcquireRelease|WorkgroupMemory")

    lid = m.var("lid", p_in_v3, "Input")
    lii = m.var("lii", p_in_u, "Input")
    sh = m.var("sh", p_wg_arr4, "Workgroup")
    vec = m.var("vec", p_wg_v2, "Workgroup")
    svar = m.var("arg", p_wg_st, "Workgroup")
    out_val = m.var("out_val", p_pv_u, "Private")
    out_idx = m.var("out_idx", p_pv_u, "Private")
    out_zero = m.var("out_zero", p_pv_u, "Private")
    out_vec = m.var("out_vec", p_pv_u, "Private")
    out_struct = m.var("out_struct", p_pv_u, "Private")
    out_vec_zero = m.var("out_vec_zero", p_pv_u, "Private")
    out_struct_zero = m.var("out_struct_zero", p_pv_u, "Private")
    tmpvar = m.var("tmpvar", p_pv_u, "Private")

    m.entry_point("main", m.nid("main"), [lid, lii], (4, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.builtin(lii, "SpvBuiltInLocalInvocationIndex")
    for ident, name in [(lid, "lid"), (lii, "lii"), (sh, "sh"), (vec, "vec"),
                        (svar, "arg"), (out_val, "out_val"),
                        (out_idx, "out_idx"), (out_zero, "out_zero"),
                        (out_vec, "out_vec"), (out_struct, "out_struct"),
                        (out_vec_zero, "out_vec_zero"),
                        (out_struct_zero, "out_struct_zero"),
                        (tmpvar, "tmpvar")]:
        m.name(ident, name)

    # helper writing through a plain Workgroup pointer parameter
    helper = m.function("store_value", void, fnv_pu_u)
    hp = m.param("hp", p_wg_u)
    hv = m.param("hv", uint)
    m.label("hlabel")
    m.st(hp, hv)
    m.ret()
    m.end()

    m.function("main", void, fnv)
    m.label("entry")

    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    ii = m.ld("lii_v", uint, lii)

    p_i = m.access("p_i", p_wg_u, sh, i)
    m.st(out_zero, m.ld("zero", uint, p_i))

    # vector element 1 and struct member 0 must also start zeroed
    p_vec1 = m.access("p_vec1", p_wg_u, vec, u1)
    m.st(out_vec_zero, m.ld("vec_zero", uint, p_vec1))
    p_arg0 = m.access("p_arg0", p_wg_u, svar, u0)
    m.st(out_struct_zero, m.ld("arg_zero", uint, p_arg0))

    own = m.binop("SpvOpIAdd", "own", uint, i, u10)
    m.st(p_i, own)

    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("special"), m.nid("merge_special"))

    m.label("special")
    p_vec0 = m.access("p_vec0", p_wg_u, vec, u0)
    m.st(p_vec0, u77)
    p_arg1 = m.access("p_arg1", p_wg_u, svar, u1)
    m.call("special_call", void, helper, p_arg1, u99)
    m.branch(m.nid("merge_special"))

    m.label("merge_special")
    m.barrier(sc, sem, text="OpControlBarrier %scope_workgroup %scope_workgroup %sem_ar_wgmem")

    nbr = m.binop("SpvOpBitwiseXor", "nbr", uint, i, u1)
    p_n = m.access("p_n", p_wg_u, sh, nbr)
    m.st(tmpvar, m.ld("tmp_other", uint, p_n))

    m.barrier(sc, sem)
    low = m.binop("SpvOpBitwiseAnd", "lowbit", uint, i, u1)
    even = m.compare("SpvOpIEqual", "is_even", "bool", low, u0)
    m.branch_cond(even, m.nid("even"), m.nid("odd"))

    m.label("even")
    m.call("even_call", void, helper, p_i, m.ld("tmp_a", uint, tmpvar))
    m.branch(m.nid("merge_write"))

    m.label("odd")
    m.copy_memory(p_i, tmpvar)
    m.branch(m.nid("merge_write"))

    m.label("merge_write")
    m.barrier(sc, sem)

    final = m.ld("final", uint, p_i)
    m.st(out_val, final)
    m.st(out_idx, ii)
    p_vec_r = m.access("p_vec_r", p_wg_u, vec, u0)
    m.st(out_vec, m.ld("vec_r", uint, p_vec_r))
    p_arg_r = m.access("p_arg_r", p_wg_u, svar, u1)
    m.st(out_struct, m.ld("arg_r", uint, p_arg_r))

    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_reduce():
    m = Module("reduce", "four invocations, two barrier rounds halving a shared array")

    m.capability("SpvCapabilityShader")
    m.memory_model()

    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    c2 = m.const("c2", uint, 2)
    c4 = m.const("c4", uint, 4)
    arr4 = m.type_array("arr4_uint", uint, c4)
    arr2 = m.type_array("arr2_uint", uint, c2)
    st = m.type_struct("acc_st", [uint, arr2])
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    p_in_u = m.type_pointer("p_in_uint", "Input", uint)
    p_wg_u = m.type_pointer("p_wg_uint", "Workgroup", uint)
    p_wg_arr4 = m.type_pointer("p_wg_arr4_uint", "Workgroup", arr4)
    p_wg_st = m.type_pointer("p_wg_st", "Workgroup", st)
    p_pv_u = m.type_pointer("p_private_uint", "Private", uint)
    fnv_st_u = m.type_function("fn_void_ptrworkgroup_st_uint", void, p_wg_st, uint)

    u0 = m.const("u0", uint, 0)
    u1 = m.const("u1", uint, 1)
    u100 = m.const("u100", uint, 100)
    u555 = m.const("u555", uint, 555)
    sc = m.const("scope_workgroup", uint, SCOPE_WG,
                 text="%scope_workgroup = OpConstant %uint 2 ; Workgroup")
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM,
                  text="%sem_ar_wgmem = OpConstant %uint 264 ; AcquireRelease|WorkgroupMemory")

    lid = m.var("lid", p_in_v3, "Input")
    lii = m.var("lii", p_in_u, "Input")
    wgid = m.var("wgid", p_in_v3, "Input")
    data = m.var("data", p_wg_arr4, "Workgroup")
    acc = m.var("acc", p_wg_st, "Workgroup")
    out_sum = m.var("out_sum", p_pv_u, "Private")
    out_idx = m.var("out_idx", p_pv_u, "Private")
    out_total = m.var("out_total", p_pv_u, "Private")
    out_nested = m.var("out_nested", p_pv_u, "Private")
    out_zero = m.var("out_zero", p_pv_u, "Private")
    tmpvar = m.var("tmpvar", p_pv_u, "Private")

    m.entry_point("main", m.nid("main"), [lid, lii, wgid], (4, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.builtin(lii, "SpvBuiltInLocalInvocationIndex")
    m.builtin(wgid, "SpvBuiltInWorkgroupId")
    for ident, name in [(lid, "lid"), (lii, "lii"), (wgid, "wgid"), (data, "data"),
                        (acc, "acc"), (out_sum, "out_sum"), (out_idx, "out_idx"),
                        (out_total, "out_total"), (out_nested, "out_nested"),
                        (out_zero, "out_zero"), (tmpvar, "tmpvar")]:
        m.name(ident, name)

    helper = m.function("set_total", void, fnv_st_u)
    hp = m.param("hp", p_wg_st)
    hv = m.param("hv", uint)
    m.label("hlabel")
    hp0 = m.access("hp0", p_wg_u, hp, u0)
    m.st(hp0, hv)
    m.ret()
    m.end()

    m.function("main", void, fnv)
    m.label("entry")

    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    ii = m.ld("lii_v", uint, lii)
    wg_v = m.ld("wg_v", v3u, wgid)
    wx = m.extract("wx", uint, wg_v, 0)

    # All invocations read the (zero initialized) nested struct member before
    # the first barrier.  The only write to it happens after that barrier, so
    # this stays race free: reads happen-before barrier 1, the write after.
    p_nested = m.access("p_nested", p_wg_u, acc, u1, u1)
    m.st(out_zero, m.ld("zero", uint, p_nested))

    p_i = m.access("p_i", p_wg_u, data, i)
    val = m.binop("SpvOpIAdd", "val", uint, i, u1)
    base = m.binop("SpvOpIMul", "base", uint, wx, u100)
    m.st(p_i, m.binop("SpvOpIAdd", "val_base", uint, val, base))

    m.barrier(sc, sem, text="OpControlBarrier %scope_workgroup %scope_workgroup %sem_ar_wgmem")

    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("special"), m.nid("merge_special"))

    m.label("special")
    m.st(tmpvar, u555)
    m.copy_memory(p_nested, tmpvar)
    m.branch(m.nid("merge_special"))

    m.label("merge_special")
    low = m.compare("SpvOpSLessThan", "is_low", "bool", i, c2)
    m.branch_cond(low, m.nid("r1work"), m.nid("r1merge"))

    m.label("r1work")
    pa = m.access("pa", p_wg_u, data, i)
    pj = m.access("pj", p_wg_u, data, m.binop("SpvOpIAdd", "i_plus_2", uint, i, c2))
    a = m.ld("a", uint, pa)
    b = m.ld("b", uint, pj)
    m.st(pa, m.binop("SpvOpIAdd", "sum_r1", uint, a, b))
    m.branch(m.nid("r1merge"))

    m.label("r1merge")
    m.barrier(sc, sem)

    zero = m.compare("SpvOpIEqual", "is_zero", "bool", i, u0)
    m.branch_cond(zero, m.nid("r2work"), m.nid("r2merge"))

    m.label("r2work")
    p0 = m.access("p0", p_wg_u, data, u0)
    p1 = m.access("p1", p_wg_u, data, u1)
    a0 = m.ld("a0", uint, p0)
    b1 = m.ld("b1", uint, p1)
    s0 = m.binop("SpvOpIAdd", "sum_r2", uint, a0, b1)
    m.st(p0, s0)
    m.call("total_call", void, helper, acc, s0)
    m.branch(m.nid("r2merge"))

    m.label("r2merge")
    m.barrier(sc, sem)

    pf = m.access("pf", p_wg_u, data, u0)
    m.st(out_sum, m.ld("sum", uint, pf))
    m.st(out_idx, ii)
    pt = m.access("pt", p_wg_u, acc, u0)
    m.st(out_total, m.ld("total", uint, pt))
    pn = m.access("pn", p_wg_u, acc, u1, u1)
    m.st(out_nested, m.ld("nested", uint, pn))

    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_builtins():
    m = Module("builtins", "3D builtin numbering in a 2x2x2 workgroup")

    m.capability("SpvCapabilityShader")
    m.memory_model()

    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    p_in_u = m.type_pointer("p_in_uint", "Input", uint)
    p_pv_u = m.type_pointer("p_private_uint", "Private", uint)

    lid = m.var("lid", p_in_v3, "Input")
    lii = m.var("lii", p_in_u, "Input")
    wgid = m.var("wgid", p_in_v3, "Input")
    gid = m.var("gid", p_in_v3, "Input")

    outputs = {}
    names = ["out_lx", "out_ly", "out_lz", "out_index",
             "out_wx", "out_wy", "out_wz", "out_gx", "out_gy", "out_gz"]
    for name in names:
        outputs[name] = m.var(name, p_pv_u, "Private")

    m.entry_point("main", m.nid("main"), [lid, lii, wgid, gid], (2, 2, 2))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.builtin(lii, "SpvBuiltInLocalInvocationIndex")
    m.builtin(wgid, "SpvBuiltInWorkgroupId")
    m.builtin(gid, "SpvBuiltInGlobalInvocationId")
    for ident, name in [(lid, "lid"), (lii, "lii"), (wgid, "wgid"), (gid, "gid")]:
        m.name(ident, name)
    for name in names:
        m.name(outputs[name], name)

    m.function("main", void, fnv)
    m.label("entry")

    def store_components(var, typ, prefix, output_suffixes):
        value = m.ld(prefix + "_v", typ, var)
        for comp in range(3):
            component = m.extract("%s_%d" % (prefix, comp), uint, value, comp)
            m.st(outputs[prefix + output_suffixes[comp]], component)

    store_components(lid, v3u, "out_l", "xyz")
    store_components(wgid, v3u, "out_w", "xyz")
    store_components(gid, v3u, "out_g", "xyz")
    m.st(outputs["out_index"], m.ld("lii_v", uint, lii))
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_early_return():
    m = Module("early", "invocation 0 returns before the barrier")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    u0 = m.const("u0", uint, 0)
    sc = m.const("scope_workgroup", uint, SCOPE_WG)
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM)
    lid = m.var("lid", p_in_v3, "Input")
    m.entry_point("main", m.nid("main"), [lid], (2, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.name(lid, "lid")

    m.function("main", void, fnv)
    m.label("entry")
    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("done"), m.nid("wait"))
    m.label("done")
    m.ret()
    m.label("wait")
    m.barrier(sc, sem)
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_callsite():
    m = Module("callsite", "same helper barrier reached from two call sites")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    fnv = m.type_function("fn_void", void)
    fnv_u = m.type_function("fn_void_uint", void, uint)
    u0 = m.const("u0", uint, 0)
    sc = m.const("scope_workgroup", uint, SCOPE_WG)
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM)
    lid = m.var("lid", p_in_v3, "Input")
    m.entry_point("main", m.nid("main"), [lid], (2, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.name(lid, "lid")

    helper = m.function("helper", void, fnv_u)
    m.param("hv", uint)
    m.label("hlabel")
    m.barrier(sc, sem)
    m.ret()
    m.end()

    m.function("main", void, fnv)
    m.label("entry")
    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("site_a"), m.nid("site_b"))
    m.label("site_a")
    m.call("call_a", void, helper, i)
    m.branch(m.nid("merge"))
    m.label("site_b")
    m.call("call_b", void, helper, i)
    m.branch(m.nid("merge"))
    m.label("merge")
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_barsites():
    m = Module("barsites", "invocations reach different barriers in round 0")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    u0 = m.const("u0", uint, 0)
    sc = m.const("scope_workgroup", uint, SCOPE_WG)
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM)
    lid = m.var("lid", p_in_v3, "Input")
    m.entry_point("main", m.nid("main"), [lid], (2, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.name(lid, "lid")

    m.function("main", void, fnv)
    m.label("entry")
    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("bar_a"), m.nid("bar_b"))
    m.label("bar_a")
    m.barrier(sc, sem, text="OpControlBarrier <A> %scope_workgroup %scope_workgroup %sem_ar_wgmem")
    m.branch(m.nid("merge"))
    m.label("bar_b")
    m.barrier(sc, sem, text="OpControlBarrier <B> %scope_workgroup %scope_workgroup %sem_ar_wgmem")
    m.branch(m.nid("merge"))
    m.label("merge")
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_loop():
    m = Module("loop", "the same barrier instruction reached twice in a loop")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    p_pv_u = m.type_pointer("p_private_uint", "Private", uint)
    u0 = m.const("u0", uint, 0)
    u1 = m.const("u1", uint, 1)
    u2 = m.const("u2", uint, 2)
    sc = m.const("scope_workgroup", uint, SCOPE_WG)
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM)
    lid = m.var("lid", p_in_v3, "Input")
    counter = m.var("counter", p_pv_u, "Private")
    out_counter = m.var("out_counter", p_pv_u, "Private")
    m.entry_point("main", m.nid("main"), [lid], (2, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    for ident, name in [(lid, "lid"), (counter, "counter"),
                        (out_counter, "out_counter")]:
        m.name(ident, name)

    m.function("main", void, fnv)
    m.label("entry")
    m.st(counter, u0)
    m.branch(m.nid("loop"))
    m.label("loop")
    m.barrier(sc, sem, text="OpControlBarrier <loop> %scope_workgroup %scope_workgroup %sem_ar_wgmem")
    c = m.ld("counter_v", uint, counter)
    c2 = m.binop("SpvOpIAdd", "counter_next", uint, c, u1)
    m.st(counter, c2)
    again = m.compare("SpvOpSLessThan", "again", "bool", c2, u2)
    m.branch_cond(again, m.nid("loop"), m.nid("exit"))
    m.label("exit")
    m.st(out_counter, m.ld("counter_final", uint, counter))
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_bad_image():
    m = Module("bad_image", "module containing an image type is rejected")
    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    m.t("SpvOpTypeImage", "image2d", uint, E("SpvDim2D"), 0, 0, 0, 1,
        E("SpvImageFormatUnknown"))
    m.entry_point("main", m.nid("main"), [], (1, 1, 1))
    m.function("main", void, fnv)
    m.label("entry")
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_recursive():
    m = Module("recursive", "recursive function calls are rejected")
    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    m.entry_point("main", m.nid("main"), [], (1, 1, 1))
    main = m.function("main", void, fnv)
    m.label("entry")
    m.call("recurse", void, main)
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_no_local_size():
    m = Module("no_local_size", "entry point without a numeric LocalSize is rejected")
    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    m.emit("SpvOpEntryPoint",
           [E("SpvExecutionModelGLCompute"), m.nid("main")] + m.string_words("main"),
           text='OpEntryPoint GLCompute %main "main"')
    m.function("main", void, fnv)
    m.label("entry")
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_two_entry_points():
    m = Module("two_entry_points", "more than one entry point is rejected")
    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    first = m.function("first", void, fnv)
    m.label("first_entry")
    m.ret()
    m.end()
    second = m.function("second", void, fnv)
    m.label("second_entry")
    m.ret()
    m.end()
    m.emit("SpvOpEntryPoint",
           [E("SpvExecutionModelGLCompute"), first] + m.string_words("first"),
           text='OpEntryPoint GLCompute %first "first"')
    m.emit("SpvOpEntryPoint",
           [E("SpvExecutionModelGLCompute"), second] + m.string_words("second"),
           text='OpEntryPoint GLCompute %second "second"')
    m.emit("SpvOpExecutionMode",
           [first, E("SpvExecutionModeLocalSize"), 1, 1, 1],
           text="OpExecutionMode %first LocalSize 1 1 1")
    return m


# ----------------------------------------------------------------------
def build_round_skip():
    m = Module("roundskip", "invocation 0 executes an extra barrier before the shared one")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    u0 = m.const("u0", uint, 0)
    sc = m.const("scope_workgroup", uint, SCOPE_WG)
    sem = m.const("sem_ar_wgmem", uint, SEM_AR_WGMEM,
                  text="%sem_ar_wgmem = OpConstant %uint 264 ; AcquireRelease|WorkgroupMemory")
    lid = m.var("lid", p_in_v3, "Input")
    m.entry_point("main", m.nid("main"), [lid], (2, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    m.name(lid, "lid")

    m.function("main", void, fnv)
    m.label("entry")
    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)
    is0 = m.compare("SpvOpIEqual", "is_inv0", "bool", i, u0)
    m.branch_cond(is0, m.nid("extra"), m.nid("shared"))

    m.label("extra")
    m.barrier(sc, sem, text="OpControlBarrier <extra> %scope_workgroup %scope_workgroup %sem_ar_wgmem")
    m.branch(m.nid("shared"))

    m.label("shared")
    m.barrier(sc, sem, text="OpControlBarrier <shared> %scope_workgroup %scope_workgroup %sem_ar_wgmem")
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def build_storage():
    m = Module("storage", "per-invocation Private, Function and Output storage")

    m.capability("SpvCapabilityShader")
    m.memory_model()
    void = m.type_void()
    m.make_bool_type()
    fnv = m.type_function("fn_void", void)
    uint = m.type_int("uint")
    v3u = m.type_vec("v3uint", uint, 3)
    p_in_v3 = m.type_pointer("p_in_v3uint", "Input", v3u)
    p_pv_u = m.type_pointer("p_private_uint", "Private", uint)
    p_fn_u = m.type_pointer("p_function_uint", "Function", uint)
    p_out_u = m.type_pointer("p_output_uint", "Output", uint)
    u0 = m.const("u0", uint, 0)
    u1 = m.const("u1", uint, 1)
    u3 = m.const("u3", uint, 3)
    u5 = m.const("u5", uint, 5)

    lid = m.var("lid", p_in_v3, "Input")
    private_v = m.var("private_v", p_pv_u, "Private")
    output_v = m.var("output_v", p_out_u, "Output")
    out_private = m.var("out_private", p_pv_u, "Private")
    out_output = m.var("out_output", p_pv_u, "Private")
    out_function = m.var("out_function", p_pv_u, "Private")

    m.entry_point("main", m.nid("main"), [lid, output_v], (4, 1, 1))
    m.builtin(lid, "SpvBuiltInLocalInvocationId")
    for ident, name in [(lid, "lid"), (private_v, "private_v"),
                        (output_v, "output_v"), (out_private, "out_private"),
                        (out_output, "out_output"), (out_function, "out_function")]:
        m.name(ident, name)

    m.function("main", void, fnv)
    # a Function storage variable lives in the entry point's frame
    function_v = m.var("function_v", p_fn_u, "Function")
    m.label("entry")
    lid_v = m.ld("lid_v", v3u, lid)
    i = m.extract("i", uint, lid_v, 0)

    m.st(private_v, i)
    doubled = m.binop("SpvOpIMul", "doubled", uint, i, u3)
    m.st(function_v, doubled)
    added = m.binop("SpvOpIAdd", "added", uint, i, u5)
    m.st(output_v, added)

    m.st(out_private, m.ld("private_r", uint, private_v))
    m.st(out_function, m.ld("function_r", uint, function_v))
    m.st(out_output, m.ld("output_r", uint, output_v))
    m.ret()
    m.end()
    return m


# ----------------------------------------------------------------------
def finish(module):
    module.words = module.header(module.next_id) + module.words


def render_c(module, out):
    out.append("/* ------------------------------------------------------------------")
    out.append(" * %s: %s" % (module.cname, module.title))
    out.append(" *")
    out.append(" * Readable SPIR-V (ids are the numeric ids used in the words):")
    for line in module.listing:
        out.append(" *   " + line)
    out.append(" */")
    out.append("static const spvm_word %s_words[] = {" % module.cname)
    words = module.words
    i = 0
    while i < len(words):
        wc = words[i] >> 16
        op = words[i] & 0xffff
        chunk = words[i:i + wc]
        name = None
        for sym, val in ENUM.items():
            if val == op:
                name = sym
                break
        out.append("\t%s, /* %s, %d word(s) */" % (
            ", ".join("0x%08xu" % w for w in chunk), name or ("op %d" % op), wc))
        i += wc
    out.append("};")
    out.append("")


def main():
    modules = [build_swap(), build_reduce(), build_builtins(),
               build_early_return(), build_callsite(), build_barsites(),
               build_loop(), build_round_skip(), build_storage(),
               build_bad_image(), build_recursive(), build_no_local_size(),
               build_two_entry_points()]
    for module in modules:
        finish(module)

    lines = []
    lines.append("/* Generated by tools/gen_workgroup_fixtures.py.\n"
                 " * C word fixtures with the readable SPIR-V in the comments.\n"
                 " * Do not edit by hand. */\n")
    lines.append("#ifndef SPVM_CHECK_WORKGROUP_FIXTURES_H")
    lines.append("#define SPVM_CHECK_WORKGROUP_FIXTURES_H\n")
    lines.append("#include <spvm/spirv.h>\n")
    for module in modules:
        render_c(module, lines)
    lines.append("#endif /* SPVM_CHECK_WORKGROUP_FIXTURES_H */")

    with open(OUTPUT, "w") as f:
        f.write("\n".join(lines) + "\n")

    print("wrote", OUTPUT)


if __name__ == "__main__":
    sys.exit(main())
