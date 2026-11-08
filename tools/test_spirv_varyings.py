#!/usr/bin/env python3
"""Tests runtime/nvn/vulkan/spirv_varyings.c (the varying renumbering PanVK < 26.1 needs) on synthetic SPIR-V pairs:
a last pre-rasterization stage (vertex, tessellation evaluation or geometry shader) and a fragment shader. Builds the C
file with tools/test_spirv_varyings.c, assembles each pair with spirv-as, renumbers it, validates the results with
spirv-val (Vulkan 1.3) and checks that the two stages still agree: one old -> new Location map, one-to-one, the same on
both sides, nothing but those Location literals changed. Also every case the pass must refuse (other producer stages,
several entry points, geometry Stream other than 0, per-primitive / per-vertex fragment inputs, block member
Locations, decoration groups), which must leave both modules byte-identical. tools/test_spirv_varyings_render.py
renders the same kind of pairs on a Vulkan device.

Needs a C compiler (CC, default cc) and SPIRV-Tools (spirv-as, spirv-dis, spirv-val) with Vulkan 1.3 and
SPV_EXT_mesh_shader (2023 or newer).
Usage: python3 tools/test_spirv_varyings.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIMIT = 16
ENV = "vulkan1.3"

# type name -> (SPIR-V type id, Locations taken, needs Flat in a fragment shader)
TYPES = {"float": ("%f32", 1, False), "vec2": ("%v2", 1, False), "vec4": ("%v4", 1, False),
         "mat2": ("%m2", 2, False), "mat4": ("%m4", 4, False), "vec4[3]": ("%a3", 3, False),
         "float[4]": ("%af4", 4, False), "S": ("%S", 2, False), "double": ("%f64", 1, True),
         "dvec3": ("%dv3", 2, True), "dvec4": ("%dv4", 2, True), "dmat2": ("%dm2", 2, True),
         "ivec2": ("%iv2", 1, True)}

MODELS = {"v": "Vertex", "c": "TessellationControl", "e": "TessellationEvaluation", "g": "Geometry", "m": "MeshEXT",
          "f": "Fragment"}


def module(stage, vars_, opts=None):
    """vars_: (name, type, location, component or None, interpolation or None): the stage's Output variables (its
    Input variables for 'f'). stage: a MODELS key. opts: block_member_loc, stream {name: n}, entries2, group (a name),
    perprim / pervertex (a fragment input name)."""
    opts = opts or {}
    sc = "Input" if stage == "f" else "Output"
    caps = ["Shader", "Float64"]
    exts, modes, dec, var, names, iface = [], [], [], [], [], []

    def add_var(name, ptr_sc, ptype, decs=()):
        names.append(name)
        iface.append("%" + name)
        var.append("%%p_%s = OpTypePointer %s %s" % (name, ptr_sc, ptype))
        var.append("%%%s = OpVariable %%p_%s %s" % (name, name, ptr_sc))
        dec.extend("OpDecorate %%%s %s" % (name, d) for d in decs)

    if stage in "vegm":
        add_var("pv", "Output", "%a3pv" if stage == "m" else "%PV")
        dec += ["OpMemberDecorate %PV 0 BuiltIn Position", "OpDecorate %PV Block"]
    if stage == "v":
        add_var("a_in", "Input", "%v4", ["Location 3"])
    elif stage == "e":
        caps.append("Tessellation")
        modes += ["Triangles", "SpacingEqual", "VertexOrderCcw"]
        # inputs from the tessellation control shader at high Locations: arrayed per vertex, and a patch input. They
        # aren't the fragment shader's interface, so they must keep their Locations.
        add_var("tin", "Input", "%a32", ["Location 20"])
        add_var("pin", "Input", "%v4", ["Location 29", "Patch"])
    elif stage == "g":
        caps.append("Geometry")
        modes += ["Triangles", "Invocations 1", "OutputTriangleStrip", "OutputVertices 3"]
        add_var("gin", "Input", "%a3", ["Location 18"])  # (likewise)
        if opts.get("stream"):
            caps.append("GeometryStreams")
    elif stage == "c":
        caps.append("Tessellation")
        modes += ["OutputVertices 3"]
    elif stage == "m":
        caps.append("MeshShadingEXT")
        exts.append("SPV_EXT_mesh_shader")
        modes += ["LocalSize 1 1 1", "OutputVertices 3", "OutputPrimitivesEXT 1", "OutputTrianglesEXT"]
    elif stage == "f":
        modes.append("OriginUpperLeft")
        add_var("frag", "Input", "%v4", ["BuiltIn FragCoord"])
        add_var("c0", "Output", "%v4", ["Location 0"])
    if opts.get("block_member_loc") is not None:
        add_var("blk", sc, "%Blk")
        dec += ["OpDecorate %Blk Block", "OpMemberDecorate %%Blk 0 Location %d" % opts["block_member_loc"]]
    for n, t, loc, comp, interp in vars_:
        tid, _, flat = TYPES[t]
        if stage in "cm" or n == opts.get("pervertex"):
            tid = "%a3"  # (arrayed per vertex)
        d = ["Location %d" % loc]
        if comp is not None:
            d.append("Component %d" % comp)
        if stage == "f" and (interp or flat) and n != opts.get("perprim"):
            d.append(interp or "Flat")
            if interp == "Sample":
                caps.append("SampleRateShading")
        if n in opts.get("stream", {}):
            d.append("Stream %d" % opts["stream"][n])
        if n == opts.get("perprim"):
            caps.append("MeshShadingEXT")
            exts.append("SPV_EXT_mesh_shader")
            d.append("PerPrimitiveEXT")
        if n == opts.get("pervertex"):
            caps.append("FragmentBarycentricKHR")
            exts.append("SPV_KHR_fragment_shader_barycentric")
            d.append("PerVertexKHR")
        if opts.get("group") == n:
            dec.append("OpDecorate %%grp Location %d" % loc)
            d = d[1:]
        add_var(n, sc, tid, d)
    if opts.get("group"):  # (the group's decorations, the group, then whatever applies it)
        grp = [l for l in dec if l.startswith("OpDecorate %grp ")]
        dec = grp + ["%grp = OpDecorationGroup"] + [l for l in dec if l not in grp] + \
            ["OpGroupDecorate %%grp %%%s" % opts["group"]]
    if "S" in [v[1] for v in vars_]:
        dec.append("OpDecorate %S Block")
    head = ["OpCapability %s" % c for c in dict.fromkeys(caps)]
    head += ['OpExtension "%s"' % e for e in dict.fromkeys(exts)]
    head.append("OpMemoryModel Logical GLSL450")
    head.append('OpEntryPoint %s %%main "main" %s' % (MODELS[stage], " ".join(iface)))
    if opts.get("entries2"):
        head.append('OpEntryPoint %s %%main2 "main2" %s' % (MODELS[stage], " ".join(iface)))
    for fn in ["%main"] + (["%main2"] if opts.get("entries2") else []):
        head += ["OpExecutionMode %s %s" % (fn, m) for m in modes]
    head += ['OpName %%%s "%s"' % (n, n) for n in names]
    types = ["%void = OpTypeVoid", "%fn = OpTypeFunction %void", "%f32 = OpTypeFloat 32", "%f64 = OpTypeFloat 64",
             "%i32 = OpTypeInt 32 1", "%u32 = OpTypeInt 32 0", "%v2 = OpTypeVector %f32 2",
             "%v4 = OpTypeVector %f32 4", "%dv2 = OpTypeVector %f64 2", "%dv3 = OpTypeVector %f64 3",
             "%dv4 = OpTypeVector %f64 4", "%iv2 = OpTypeVector %i32 2", "%m2 = OpTypeMatrix %v2 2",
             "%m4 = OpTypeMatrix %v4 4", "%dm2 = OpTypeMatrix %dv2 2", "%c3 = OpConstant %u32 3",
             "%c4 = OpConstant %u32 4", "%c32 = OpConstant %u32 32", "%a3 = OpTypeArray %v4 %c3",
             "%af4 = OpTypeArray %f32 %c4", "%a32 = OpTypeArray %v4 %c32", "%S = OpTypeStruct %v4 %f32",
             "%PV = OpTypeStruct %v4", "%a3pv = OpTypeArray %PV %c3", "%Blk = OpTypeStruct %v4"]
    body = ["%main = OpFunction %void None %fn", "%entry = OpLabel", "OpReturn", "OpFunctionEnd"]
    if opts.get("entries2"):
        body += ["%main2 = OpFunction %void None %fn", "%entry2 = OpLabel", "OpReturn", "OpFunctionEnd"]
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


passed = 0


def check(tmp, label, pstage, pvars, fvars, expect, fixed=None, popts=None, fopts=None, fstage="f"):
    """pstage: the producer's MODELS key. expect: the result code. fixed: {('p'|'f', name): new Location} spot checks."""
    global passed
    paths = {}
    for side, stage, vars_, opts in (("p", pstage, pvars, popts), ("f", fstage, fvars, fopts)):
        asm = os.path.join(tmp, "%s.%s.spvasm" % (label, side))
        with open(asm, "w") as f:
            f.write(module(stage, vars_, opts))
        paths[side] = os.path.join(tmp, "%s.%s.spv" % (label, side))
        run("spirv-as", "--target-env", ENV, "-o", paths[side], asm)
        run("spirv-val", "--target-env", ENV, paths[side])
    outp, outf = paths["p"] + ".out", paths["f"] + ".out"
    res = run(os.path.join(tmp, "drv"), paths["p"], paths["f"], str(LIMIT), outp, outf).strip()
    code = int(res.split()[0])
    assert code == expect, "%s: result %s, expected %d" % (label, res, expect)
    for p in (outp, outf):
        run("spirv-val", "--target-env", ENV, p)
    if code <= 0:
        for a, b in ((paths["p"], outp), (paths["f"], outf)):
            assert open(a, "rb").read() == open(b, "rb").read(), "%s: changed although result %d" % (label, code)
        print("ok  %-18s %s" % (label, res))
        passed += 1
        return
    # the old -> new Location of every Location each stage covers must be one consistent, one-to-one map
    newloc = {}
    for side, vars_, before, after in (("p", pvars, paths["p"], outp), ("f", fvars, paths["f"], outf)):
        d0, rest0 = decorations(before)
        d1, rest1 = decorations(after)
        assert rest0 == rest1, "%s: something other than Location changed" % label
        mine = set(n for n, _, _, _, _ in vars_)
        for n in d0:
            if n not in mine:  # (inputs of a tessellation / geometry shader, fragment outputs, ...)
                assert d0[n] == d1[n], "%s: %s %s changed" % (label, side, n)
        for n, t, loc, comp, _ in vars_:
            assert d0[n]["Location"] == loc
            assert d1[n].get("Component") == d0[n].get("Component"), "%s: %s Component changed" % (label, n)
            new = d1[n]["Location"]
            if fixed and (side, n) in fixed:
                assert new == fixed[(side, n)], "%s: %s %s at %d, expected %d" % (label, side, n, new, fixed[(side, n)])
            for k in range(TYPES[t][1]):
                old = loc + k
                assert newloc.setdefault(old, new + k) == new + k, "%s: Location %d maps two ways" % (label, old)
        if side == "f":
            slots = len(set(l + k for _, t, l, _, _ in vars_ for k in range(TYPES[t][1])))
            ends = max(d1[n]["Location"] + TYPES[t][1] for n, t, _, _, _ in vars_)
            if slots <= LIMIT:
                assert ends <= LIMIT, "%s: fragment inputs still reach Location %d" % (label, ends - 1)
    assert len(set(newloc.values())) == len(newloc), "%s: two Locations merged" % label
    print("ok  %-18s %s" % (label, res))
    passed += 1


def v(name, loc, t="vec4", comp=None, interp=None):
    return (name, t, loc, comp, interp)


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Werror", "-o", os.path.join(tmp, "drv"),
                        os.path.join(ROOT, "tools/test_spirv_varyings.c"),
                        os.path.join(ROOT, "runtime/nvn/vulkan/spirv_varyings.c")], check=True)
        for p in "veg":
            # the Field's pairs: outputs {0, 24-27}, the fragment shader reads four of them
            check(tmp, p + "-field-small", p, [v("o0", 0), v("o24", 24), v("o25", 25), v("o26", 26), v("o27", 27)],
                  [v("o0", 0), v("o24", 24, interp="NoPerspective"), v("o25", 25), v("o27", 27, interp="Flat")], 1,
                  {("f", "o0"): 0, ("f", "o24"): 1, ("f", "o25"): 2, ("f", "o27"): 3, ("p", "o26"): 4})
            big = [1, 5, 6, 7, 9, 10, 11, 12, 14, 24, 25, 27]
            check(tmp, p + "-field-large", p, [v("o%d" % l, l) for l in big], [v("o%d" % l, l) for l in big], 1,
                  {("f", "o%d" % l): i for i, l in enumerate(big)})
            # Locations 16-30: several Locations per variable, shared Locations (Component, 64-bit pairs), an array
            # element read on its own, a block, every interpolation, a fragment input the producer doesn't write,
            # unread outputs
            check(tmp, p + "-mixed", p,
                  [v("extra", 2), v("iv", 16, "ivec2", comp=2), v("arr", 17, "vec4[3]"), v("m", 20, "mat2"),
                   v("d", 22, "dvec3"), v("dd1", 24, "double", comp=0), v("dd2", 24, "double", comp=2),
                   v("blk", 25, "S"), v("cen", 27), v("smp", 28, "vec2", comp=0), v("smp2", 28, "float", comp=2),
                   v("np", 29), v("fa", 30, "float", comp=0), v("fb", 30, "float", comp=1)],
                  [v("iv", 16, "ivec2", comp=2), v("arr1", 18), v("m", 20, "mat2"), v("d", 22, "dvec3"),
                   v("dd2", 24, "double", comp=2), v("blk", 25, "S"), v("cen", 27, interp="Centroid"),
                   v("smp", 28, "vec2", comp=0, interp="Sample"), v("smp2", 28, "float", comp=2, interp="Sample"),
                   v("np", 29, interp="NoPerspective"), v("fb", 30, "float", comp=1), v("lonely", 31)], 1,
                  {("f", "iv"): 0, ("f", "arr1"): 2, ("p", "arr"): 1, ("f", "m"): 4, ("f", "d"): 6, ("f", "dd2"): 8,
                   ("f", "blk"): 9, ("f", "cen"): 11, ("f", "smp"): 12, ("f", "np"): 13, ("f", "fb"): 14,
                   ("p", "fa"): 14, ("f", "lonely"): 15, ("p", "extra"): 16})
            # matrices, a 64-bit matrix, a float array at Component 1
            check(tmp, p + "-matrices", p,
                  [v("mx", 16, "mat4"), v("dm", 20, "dmat2"), v("af", 22, "float[4]", comp=1), v("x", 26)],
                  [v("mx", 16, "mat4"), v("dm", 20, "dmat2"), v("af", 22, "float[4]", comp=1), v("x", 26)], 1,
                  {("f", "mx"): 0, ("f", "dm"): 4, ("f", "af"): 6, ("f", "x"): 10})
            # nothing at Location 16 or above in the fragment shader: left alone (byte-identical)
            check(tmp, p + "-below-16", p, [v("o%d" % l, l) for l in (0, 1, 2, 20)],
                  [v("o%d" % l, l) for l in (0, 1, 2)] + [v("d", 4, "dvec4"), v("m", 10, "mat4")], 0)
            # more than 16 fragment inputs (the driver doesn't take the buffer path then): renumbered, consistent
            many = list(range(10, 30))
            check(tmp, p + "-many", p, [v("o%d" % l, l) for l in many], [v("o%d" % l, l) for l in many], 1)
        # geometry shader Streams: 0 is the rasterized one; any other is refused
        check(tmp, "g-stream0", "g", [v("o20", 20), v("o24", 24)], [v("o24", 24)], 1, {("f", "o24"): 0},
              popts={"stream": {"o20": 0, "o24": 0}})
        check(tmp, "g-stream1", "g", [v("o20", 20), v("o24", 24)], [v("o24", 24)], -1,
              popts={"stream": {"o20": 1}})
        # refused: producers that aren't a last pre-rasterization stage, a consumer that isn't a fragment shader
        check(tmp, "c-producer", "c", [v("o24", 24)], [v("o24", 24)], -1)
        check(tmp, "m-producer", "m", [v("o24", 24)], [v("o24", 24)], -1)
        check(tmp, "v-to-v", "v", [v("o24", 24)], [v("o24", 24)], -1, fstage="v")
        # refused: several entry points, per-primitive or per-vertex fragment inputs, block member Locations,
        # decoration groups
        check(tmp, "v-two-entries", "v", [v("o24", 24)], [v("o24", 24)], -1, popts={"entries2": True})
        check(tmp, "f-two-entries", "v", [v("o24", 24)], [v("o24", 24)], -1, fopts={"entries2": True})
        check(tmp, "f-perprimitive", "v", [v("o24", 24), v("o25", 25)], [v("o24", 24), v("o25", 25)], -1,
              fopts={"perprim": "o25"})
        check(tmp, "f-pervertex", "v", [v("o24", 24), v("o25", 25)], [v("o24", 24), v("o25", 25)], -1,
              fopts={"pervertex": "o25"})
        check(tmp, "member-loc", "v", [v("o0", 0)], [v("o0", 0), v("o24", 24)], -1, popts={"block_member_loc": 24})
        check(tmp, "e-member-loc", "e", [v("o0", 0)], [v("o0", 0), v("o24", 24)], -1, popts={"block_member_loc": 24})
        check(tmp, "decoration-group", "v", [v("o24", 24), v("o25", 25)], [v("o24", 24), v("o25", 25)], -1,
              popts={"group": "o25"})
    print("all %d passed" % passed)


if __name__ == "__main__":
    sys.exit(main())
