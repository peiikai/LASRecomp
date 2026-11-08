#!/usr/bin/env python3
"""Tests runtime/nvn/vulkan/spirv_varyings.c (the PanVK < 26.1 varying renumbering) on synthetic vertex + fragment
SPIR-V pairs: builds the C file with tools/test_spirv_varyings.c, assembles each pair with spirv-as, renumbers it,
validates the results with spirv-val and checks that the two stages still agree.

Needs a C compiler (CC, default cc) and SPIRV-Tools (spirv-as, spirv-dis, spirv-val) on PATH.
Usage: python3 tools/test_spirv_varyings.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIMIT = 16
ENV = "vulkan1.2"  # (SPIR-V 1.5: old enough for distro SPIRV-Tools; the remap is the same for any version)

# type name -> (SPIR-V type id, Locations taken, needs Flat in a fragment shader)
TYPES = {"vec4": ("%v4", 1, False), "float": ("%f32", 1, False), "mat2": ("%m2", 2, False),
         "vec4[3]": ("%a3", 3, False), "dvec4": ("%dv4", 2, True), "ivec2": ("%iv2", 1, True)}


def module(stage, vars_, block_member_loc=None):
    """vars_: (name, type, location, component or None, interpolation or None). stage 'v' or 'f'."""
    sc = "Output" if stage == "v" else "Input"
    names = [v[0] for v in vars_]
    head = ["OpCapability Shader", "OpCapability Float64", "OpMemoryModel Logical GLSL450"]
    dec, var = [], []
    if stage == "v":
        iface = ["%a_in", "%pv"] + ["%" + n for n in names]
        head.append('OpEntryPoint Vertex %main "main" ' + " ".join(iface))
        dec += ["OpDecorate %a_in Location 3", "OpMemberDecorate %PV 0 BuiltIn Position", "OpDecorate %PV Block"]
        var += ["%p_in_v4 = OpTypePointer Input %v4", "%a_in = OpVariable %p_in_v4 Input",
                "%p_pv = OpTypePointer Output %PV", "%pv = OpVariable %p_pv Output"]
    else:
        iface = ["%frag", "%c0"] + ["%" + n for n in names]
        head.append('OpEntryPoint Fragment %main "main" ' + " ".join(iface))
        head.append("OpExecutionMode %main OriginUpperLeft")
        dec += ["OpDecorate %frag BuiltIn FragCoord", "OpDecorate %c0 Location 0"]
        var += ["%p_in_v4 = OpTypePointer Input %v4", "%frag = OpVariable %p_in_v4 Input",
                "%p_out_v4 = OpTypePointer Output %v4", "%c0 = OpVariable %p_out_v4 Output"]
    if block_member_loc is not None:
        iface_blk = "%blk"
        head[-1 if stage == "v" else -2] += " " + iface_blk
        dec += ["OpDecorate %Blk Block", "OpMemberDecorate %%Blk 0 Location %d" % block_member_loc]
        var += ["%%p_blk = OpTypePointer %s %%Blk" % sc, "%%blk = OpVariable %%p_blk %s" % sc]
    for n, t, loc, comp, interp in vars_:
        tid, _, flat = TYPES[t]
        head.append('OpName %%%s "%s"' % (n, n))
        dec.append("OpDecorate %%%s Location %d" % (n, loc))
        if comp is not None:
            dec.append("OpDecorate %%%s Component %d" % (n, comp))
        if stage == "f" and (interp or flat):
            dec.append("OpDecorate %%%s %s" % (n, interp or "Flat"))
        var.append("%%p_%s = OpTypePointer %s %s" % (n, sc, tid))
        var.append("%%%s = OpVariable %%p_%s %s" % (n, n, sc))
    types = ["%void = OpTypeVoid", "%fn = OpTypeFunction %void", "%f32 = OpTypeFloat 32", "%f64 = OpTypeFloat 64",
             "%i32 = OpTypeInt 32 1", "%u32 = OpTypeInt 32 0", "%v2 = OpTypeVector %f32 2",
             "%v4 = OpTypeVector %f32 4", "%dv4 = OpTypeVector %f64 4", "%iv2 = OpTypeVector %i32 2",
             "%m2 = OpTypeMatrix %v2 2", "%c3 = OpConstant %u32 3", "%a3 = OpTypeArray %v4 %c3",
             "%PV = OpTypeStruct %v4", "%Blk = OpTypeStruct %v4"]
    body = ["%main = OpFunction %void None %fn", "%entry = OpLabel", "OpReturn", "OpFunctionEnd"]
    return "\n".join(head + dec + types + var + body) + "\n"


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAILED: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout


def decorations(spv):
    """{name: {decoration: value}} for the named variables, and the disassembly without Location lines."""
    dis = run("spirv-dis", spv)
    out = {}
    for m in re.finditer(r"OpDecorate %(\w+) (Location|Component) (\d+)", dis):
        out.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3))
    rest = "\n".join(l for l in dis.splitlines() if not re.search(r"OpDecorate %\w+ Location", l))
    return out, rest


def check(tmp, label, vs_vars, fs_vars, expect, fixed=None, vs_block=None):
    """expect: the result code. fixed: {('v'|'f', name): new Location} spot checks."""
    paths = {}
    for stage, vars_, blk in (("v", vs_vars, vs_block), ("f", fs_vars, None)):
        asm = os.path.join(tmp, "%s.%s.spvasm" % (label, stage))
        with open(asm, "w") as f:
            f.write(module(stage, vars_, blk))
        paths[stage] = os.path.join(tmp, "%s.%s.spv" % (label, stage))
        run("spirv-as", "--target-env", ENV, "-o", paths[stage], asm)
        run("spirv-val", "--target-env", ENV, paths[stage])
    outv, outf = paths["v"] + ".out", paths["f"] + ".out"
    res = run(os.path.join(tmp, "drv"), paths["v"], paths["f"], str(LIMIT), outv, outf).strip()
    code = int(res.split()[0])
    assert code == expect, "%s: result %s, expected %d" % (label, res, expect)
    for p in (outv, outf):
        run("spirv-val", "--target-env", ENV, p)
    if code <= 0:
        for a, b in ((paths["v"], outv), (paths["f"], outf)):
            assert open(a, "rb").read() == open(b, "rb").read(), "%s: changed although result %d" % (label, code)
        print("ok  %-14s %s" % (label, res))
        return
    # the old -> new Location of every Location each stage covers must be one consistent, one-to-one map
    newloc = {}
    for stage, vars_, before, after in (("v", vs_vars, paths["v"], outv), ("f", fs_vars, paths["f"], outf)):
        d0, rest0 = decorations(before)
        d1, rest1 = decorations(after)
        assert rest0 == rest1, "%s: something other than Location changed" % label
        for n, t, loc, comp, _ in vars_:
            assert d0[n]["Location"] == loc
            assert d1[n].get("Component") == d0[n].get("Component"), "%s: %s Component changed" % (label, n)
            new = d1[n]["Location"]
            if fixed and (stage, n) in fixed:
                assert new == fixed[(stage, n)], "%s: %s %s at %d, expected %d" % (label, stage, n, new, fixed[(stage, n)])
            for k in range(TYPES[t][1]):
                old = loc + k
                assert newloc.setdefault(old, new + k) == new + k, "%s: Location %d maps two ways" % (label, old)
        if stage == "f":
            slots = sum(TYPES[t][1] for _, t, _, _, _ in vars_)
            ends = max(d1[n]["Location"] + TYPES[t][1] for n, t, _, _, _ in vars_)
            if slots <= LIMIT:
                assert ends <= LIMIT, "%s: fragment inputs still reach Location %d" % (label, ends - 1)
    assert len(set(newloc.values())) == len(newloc), "%s: two Locations merged" % label
    print("ok  %-14s %s" % (label, res))


def v(name, loc, t="vec4", comp=None, interp=None):
    return (name, t, loc, comp, interp)


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Werror", "-o", os.path.join(tmp, "drv"),
                        os.path.join(ROOT, "tools/test_spirv_varyings.c"),
                        os.path.join(ROOT, "runtime/nvn/vulkan/spirv_varyings.c")], check=True)
        # the Field's pairs: vertex outputs {0, 24-27}, the fragment shader reads four of them
        check(tmp, "field-small", [v("o0", 0), v("o24", 24), v("o25", 25), v("o26", 26), v("o27", 27)],
              [v("o0", 0), v("o24", 24, interp="NoPerspective"), v("o25", 25), v("o27", 27, interp="Flat")], 1,
              {("f", "o0"): 0, ("f", "o24"): 1, ("f", "o25"): 2, ("f", "o27"): 3, ("v", "o26"): 4})
        big = [1, 5, 6, 7, 9, 10, 11, 12, 14, 24, 25, 27]
        check(tmp, "field-large", [v("o%d" % l, l) for l in big], [v("o%d" % l, l) for l in big], 1,
              {("f", "o%d" % l): i for i, l in enumerate(big)})
        # several Locations per variable, shared Locations (Component), an array element read on its own, a
        # fragment input the vertex shader doesn't write, an unread vertex output
        check(tmp, "mixed",
              [v("extra", 2), v("iv", 5, "ivec2", comp=2), v("arr", 17, "vec4[3]"), v("m", 20, "mat2"),
               v("d", 22, "dvec4"), v("fa", 30, "float", comp=0), v("fb", 30, "float", comp=1)],
              [v("iv", 5, "ivec2", comp=2), v("arr1", 18), v("m", 20, "mat2"), v("d", 22, "dvec4"),
               v("lonely", 28), v("fb", 30, "float", comp=1)], 1,
              {("f", "iv"): 0, ("f", "arr1"): 2, ("v", "arr"): 1, ("f", "m"): 4, ("f", "d"): 6, ("f", "lonely"): 8,
               ("f", "fb"): 9, ("v", "fa"): 9, ("v", "extra"): 10})
        # nothing at Location 16 or above in the fragment shader: left alone
        check(tmp, "not-needed", [v("o%d" % l, l) for l in (0, 1, 2, 20)], [v("o%d" % l, l) for l in (0, 1, 2)], 0)
        # more than 16 fragment inputs (the driver doesn't take the buffer path then): renumbered, still consistent
        many = list(range(10, 30))
        check(tmp, "many", [v("o%d" % l, l) for l in many], [v("o%d" % l, l) for l in many], 1)
        # a block with member Locations isn't handled: both modules unchanged
        check(tmp, "member-loc", [v("o0", 0)], [v("o0", 0), v("o24", 24)], -1, vs_block=24)
    print("all passed")


if __name__ == "__main__":
    sys.exit(main())
