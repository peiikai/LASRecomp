#!/usr/bin/env python3
"""Renders with runtime/nvn/vulkan/spirv_varyings.c's varying renumbering off and on, on every Vulkan 1.3 device
found, and checks that the images are the same word for word (tools/test_spirv_varyings_render.c does the Vulkan
part, offscreen). Pipelines: VS -> FS, VS -> GS -> FS, VS -> TCS -> TES -> FS and VS -> TCS -> TES -> GS -> FS; the
last stage before the fragment shader writes distinct values into varyings at Locations 0 and 15-30 (vectors,
Component-packed scalars and vectors, an array, a matrix, 64-bit values, ints and uints, flat / noperspective /
centroid / sample), the earlier stages talk to each other at high Locations too (which must stay as they are), and the
fragment shader hashes every value it reads into an R32G32B32A32_UINT colour. A fragment shader reading two varyings
swapped must change the image, so the comparison would notice a wrong renumbering. With the Khronos validation layer
installed (or --validation on), any warning or error it reports fails the test: it checks the stage interfaces, and
a pair with an int and a float swapped must make it report something.

Needs a C compiler (CC, default cc), the Vulkan loader and headers (libvulkan-dev), glslangValidator or glslc, and a
Vulkan 1.3 device with geometry and tessellation shaders, shaderFloat64 and sampleRateShading (lavapipe will do:
mesa-vulkan-drivers). Optional: the validation layer (vulkan-validationlayers). With no Vulkan device it prints why
and exits 0 (skipped).
Usage: python3 tools/test_spirv_varyings_render.py [--device SUBSTRING] [--validation auto|on|off]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# the varyings between the last pre-rasterization stage and the fragment shader:
# (name, GLSL type, Location, Component or None, qualifier, read by the fragment shader)
VARYINGS = [
    ("q0", "vec4", 0, None, "", True),
    ("u15", "vec4", 15, None, "", False),
    ("a16", "vec4", 16, None, "", True),
    ("b17", "vec3", 17, 0, "", True),
    ("c17", "float", 17, 3, "", True),
    ("d18", "ivec2", 18, 0, "flat", True),
    ("e18", "ivec2", 18, 2, "flat", True),
    ("f19", "vec4[3]", 19, None, "noperspective", True),
    ("g22", "mat2", 22, None, "", True),
    ("h24", "dvec3", 24, None, "flat", True),
    ("i25", "double", 25, 2, "flat", True),
    ("j26", "vec2", 26, 0, "centroid", True),
    ("k26", "vec2", 26, 2, "centroid", True),
    ("l27", "vec4", 27, None, "sample", True),
    ("m28", "uint", 28, 0, "flat", True),
    ("n28", "uint", 28, 1, "flat", True),
    ("o29", "float", 29, 3, "", True),
    ("p30", "int", 30, None, "flat", True),
]
# scalar components and base type of each GLSL type
SHAPE = {"float": (1, "f"), "vec2": (2, "f"), "vec3": (3, "f"), "vec4": (4, "f"), "mat2": (4, "f"),
         "vec4[3]": (12, "f"), "int": (1, "i"), "ivec2": (2, "i"), "uint": (1, "u"), "double": (1, "d"),
         "dvec3": (3, "d")}


def decl(name, t, loc, comp, qual, io, swap=None):
    if swap and name in swap:
        loc = swap[name]
    lay = "location = %d" % loc + (", component = %d" % comp if comp is not None else "")
    base, arr = (t[:-3], t[-3:]) if t.endswith("]") else (t, "")
    return "layout(%s) %s%s %s %s%s;" % (lay, qual + " " if qual else "", io, base, name, arr)


def writer():
    """GLSL that declares the outputs and write_all(), which fills them from P (a position) and S (a seed)."""
    lines = [decl(n, t, l, c, q, "out") for n, t, l, c, q, _ in VARYINGS]
    lines += ["vec4 P;", "float S;",
              "float val(int k) { return S * (0.5 + float(k)) + P.x * float(k % 7) * 0.25 + P.y * float(k % 3) + 0.03125 * float(k); }",
              "void write_all() {"]
    k = 0
    for n, t, _, _, _, _ in VARYINGS:
        cnt, base = SHAPE[t]
        vals = []
        for _ in range(cnt):
            if base == "f":
                vals.append("val(%d)" % k)
            elif base == "i":
                vals.append("int(S * 7.0) * %d - %d" % (k + 1, k * 3))
            elif base == "u":
                vals.append("uint(S * 5.0) * %du + %du" % (k + 2, k * 1000))
            else:
                vals.append("double(val(%d)) + 1.0e-9lf * double(%d)" % (k, k + 1))
            k += 1
        if t == "vec4[3]":
            for j in range(3):
                lines.append("    %s[%d] = vec4(%s);" % (n, j, ", ".join(vals[4 * j:4 * j + 4])))
        elif cnt == 1:
            lines.append("    %s = %s;" % (n, vals[0]))
        else:
            lines.append("    %s = %s(%s);" % (n, t, ", ".join(vals)))
    lines.append("}")
    return "\n".join(lines) + "\n"


def fragment(swap=None):
    lines = ["#version 450", "layout(location = 0) out uvec4 o;", "uint h[4] = uint[4](0u, 0u, 0u, 0u);",
             "void addu(int g, uint x) { h[g] = (h[g] ^ x) * 16777619u; h[3] = (h[3] ^ x) * 16777619u; }",
             "void addf(int g, float x) { addu(g, floatBitsToUint(x)); }",
             "void addd(int g, double x) { uvec2 u = unpackDouble2x32(x); addu(g, u.x); addu(g, u.y); }"]
    lines += [decl(n, t, l, c, q, "in", swap) for n, t, l, c, q, r in VARYINGS if r]
    lines.append("void main() {")
    for g, (n, t, _, _, _, r) in enumerate(VARYINGS):
        if not r:
            continue
        cnt, base = SHAPE[t]
        g %= 3
        if t == "vec4[3]":
            elems = ["%s[%d][%d]" % (n, j // 4, j % 4) for j in range(12)]
        elif t == "mat2":
            elems = ["%s[%d][%d]" % (n, j // 2, j % 2) for j in range(4)]
        elif cnt == 1:
            elems = [n]
        else:
            elems = ["%s[%d]" % (n, j) for j in range(cnt)]
        fn = {"f": "addf(%d, %s);", "i": "addu(%d, uint(%s));", "u": "addu(%d, %s);", "d": "addd(%d, %s);"}[base]
        lines += ["    " + fn % (g, e) for e in elems]
    lines += ["    o = uvec4(h[0], h[1], h[2], h[3] | 1u);", "}"]
    return "\n".join(lines) + "\n"


POS = "const vec2 pos[3] = vec2[3](vec2(-0.9, -0.9), vec2(0.9, -0.6), vec2(-0.3, 0.95));\n"
SHADERS = {
    # the last stage before the fragment shader
    "vs_last": ("vert", "#version 450\n" + POS + writer() +
                "void main() { P = vec4(pos[gl_VertexIndex], 0.0, 1.0); S = float(gl_VertexIndex + 1);\n"
                "    gl_Position = P; write_all(); }\n"),
    # feeding a tessellation control shader (at Location 24) or a geometry shader (at 13: geometry inputs may be
    # limited to 64 components)
    "vs_pre_tcs": ("vert", "#version 450\n" + POS + "layout(location = 24) out vec4 vpos;\n"
                   "void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0); vpos = gl_Position * 0.5; }\n"),
    "vs_pre_gs": ("vert", "#version 450\n" + POS + "layout(location = 13) out vec4 vpos;\n"
                  "void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0); vpos = gl_Position * 0.5; }\n"),
    "tcs": ("tesc", "#version 450\nlayout(vertices = 3) out;\nlayout(location = 24) in vec4 vpos[];\n"
            "layout(location = 26) out vec4 tpos[];\nlayout(location = 20) patch out vec4 pp;\n"
            "void main() {\n    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
            "    tpos[gl_InvocationID] = vpos[gl_InvocationID] * 2.0;\n"
            "    if (gl_InvocationID == 0) {\n        gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0;\n"
            "        gl_TessLevelOuter[2] = 1.0; gl_TessLevelInner[0] = 1.0; pp = vec4(0.25, 0.5, 0.75, 1.0);\n"
            "    }\n}\n"),
    "tes_last": ("tese", "#version 450\nlayout(triangles, equal_spacing, ccw) in;\n"
                 "layout(location = 26) in vec4 tpos[];\nlayout(location = 20) patch in vec4 pp;\n" + writer() +
                 "void main() {\n    vec3 t = gl_TessCoord;\n"
                 "    P = t.x * gl_in[0].gl_Position + t.y * gl_in[1].gl_Position + t.z * gl_in[2].gl_Position;\n"
                 "    S = dot(t, vec3(1.0, 2.0, 3.0)) + pp.x + (t.x * tpos[0].x + t.y * tpos[1].y) * 0.125;\n"
                 "    gl_Position = P; write_all();\n}\n"),
    "tes_pre": ("tese", "#version 450\nlayout(triangles, equal_spacing, ccw) in;\n"
                "layout(location = 26) in vec4 tpos[];\nlayout(location = 20) patch in vec4 pp;\n"
                "layout(location = 13) out vec4 vpos;\n"
                "void main() {\n    vec3 t = gl_TessCoord;\n"
                "    gl_Position = t.x * gl_in[0].gl_Position + t.y * gl_in[1].gl_Position + t.z * gl_in[2].gl_Position;\n"
                "    vpos = (t.x * tpos[0] + t.y * tpos[1] + t.z * tpos[2]) * pp.y;\n}\n"),
    "gs_last": ("geom", "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                "layout(location = 13) in vec4 vpos[];\n" + writer() +
                "void main() {\n    for (int k = 0; k < 3; k++) {\n        P = gl_in[k].gl_Position;\n"
                "        S = float(k + 1) + vpos[k].x * 0.5;\n"
                "        gl_Position = P; write_all(); EmitVertex();\n    }\n    EndPrimitive();\n}\n"),
    "fs": ("frag", fragment()),
    # two varyings of the same type swapped: must give another image
    "fs_alt": ("frag", fragment({"a16": 27, "l27": 16})),
    # an int and a float swapped: a mismatch the validation layer must report (so it is checking the interfaces)
    "fs_bad": ("frag", fragment({"p30": 29, "o29": 30})),
}
PIPELINES = [
    ("VS -> FS", [("vs", "vs_last")]),
    ("VS -> GS -> FS", [("vs", "vs_pre_gs"), ("gs", "gs_last")]),
    ("VS -> TCS -> TES -> FS", [("vs", "vs_pre_tcs"), ("tcs", "tcs"), ("tes", "tes_last")]),
    ("VS -> TCS -> TES -> GS -> FS", [("vs", "vs_pre_tcs"), ("tcs", "tcs"), ("tes", "tes_pre"), ("gs", "gs_last")]),
]


def compile_glsl(tmp, name, stage, src):
    path = os.path.join(tmp, name + "." + stage)
    out = path + ".spv"
    with open(path, "w") as f:
        f.write(src)
    if shutil.which("glslangValidator"):
        cmd = ["glslangValidator", "-V", "--target-env", "vulkan1.3", "-S", stage, "-o", out, path]
    elif shutil.which("glslc"):
        cmd = ["glslc", "--target-env=vulkan1.3", "-fshader-stage=" + stage, "-o", out, path]
    else:
        sys.exit("FAILED: needs glslangValidator or glslc")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAILED: %s\n%s%s\n%s" % (" ".join(cmd), r.stdout, r.stderr, src))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", help="only devices whose name or driver contains this")
    ap.add_argument("--validation", choices=("auto", "on", "off"), default="auto")
    args = ap.parse_args()
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "render")
        pc = subprocess.run(["pkg-config", "--cflags", "--libs", "vulkan"], capture_output=True, text=True)
        flags = pc.stdout.split() if pc.returncode == 0 else ["-lvulkan"]
        r = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Werror", "-o", exe,
                            os.path.join(ROOT, "tools/test_spirv_varyings_render.c"),
                            os.path.join(ROOT, "runtime/nvn/vulkan/spirv_varyings.c")] + flags,
                           capture_output=True, text=True)
        if r.returncode:
            if "vulkan/vulkan.h" in r.stderr or "-lvulkan" in r.stderr:
                print("skipped: no Vulkan headers / loader to build against (libvulkan-dev)\n" + r.stderr)
                return 0
            sys.exit("FAILED to build the render test\n" + r.stderr)
        r = subprocess.run([exe, "--list"], capture_output=True, text=True)
        lines = r.stdout.splitlines()
        devs = [l.split(" ", 2) for l in lines if l.startswith("device ")]
        if r.returncode == 77 or not devs:
            print("skipped: no Vulkan 1.3 device (%s)" % (r.stdout.strip() or r.stderr.strip() or r.returncode))
            return 0
        if r.returncode:
            sys.exit("FAILED: %s --list\n%s%s" % (exe, r.stdout, r.stderr))
        have_layer = "validation yes" in lines
        if args.validation == "on" and not have_layer:
            sys.exit("FAILED: VK_LAYER_KHRONOS_validation is not installed")
        validate = have_layer and args.validation != "off"
        if not validate:
            print("note: running without the validation layer (%s)" % ("off" if have_layer else "not installed"))
        spv = {n: compile_glsl(tmp, n, st, src) for n, (st, src) in SHADERS.items()}
        failed = ran = 0
        for d in devs:
            index, desc = d[1], d[2]
            if args.device and args.device.lower() not in desc.lower():
                continue
            print("%s%s" % (desc, ", validation on" if validate else ""))
            for label, stages in PIPELINES:
                cmd = [exe, "--device", index] + (["--validation"] if validate else [])
                for flag, name in stages:
                    cmd += ["--" + flag, spv[name]]
                cmd += ["--fs", spv["fs"], "--fs-alt", spv["fs_alt"]]
                r = subprocess.run(cmd, capture_output=True, text=True)
                out = (r.stdout + r.stderr).strip()
                if r.returncode == 77:
                    print("  skip %-28s %s" % (label, out))
                    continue
                ran += 1
                if r.returncode:
                    failed += 1
                    print("  FAIL %-28s\n%s" % (label, out))
                else:
                    print("  ok   %-28s %s" % (label, out.splitlines()[-1][3:]))
            if validate:  # (the layer must see a mismatch, or its silence above means nothing)
                cmd = [exe, "--device", index, "--validation", "--vs", spv["vs_last"], "--fs", spv["fs_bad"]]
                r = subprocess.run(cmd, capture_output=True, text=True)
                m = re.search(r"(\d+) validation messages", r.stdout)
                ran += 1
                if r.returncode == 1 and m and int(m.group(1)) > 0:
                    print("  ok   %-28s the layer reports it (%s messages)" % ("mismatched VS -> FS", m.group(1)))
                else:
                    failed += 1
                    print("  FAIL %-28s the layer didn't report it\n%s%s" % ("mismatched VS -> FS", r.stdout, r.stderr))
        if failed:
            sys.exit("%d of %d failed" % (failed, ran))
        print("all %d passed" % ran if ran else "skipped: no pipeline could run on these devices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
