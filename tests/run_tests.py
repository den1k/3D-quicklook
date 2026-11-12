#!/usr/bin/env python3
"""Loader tests. Generates small fixtures, loads them with build/ql3d-bench and checks the result.

Run ./build.sh first, then: python3 tests/run_tests.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "build", "ql3d-bench")

# A tetrahedron: 4 vertices, 4 triangles.
VERTS = [(0, 0, 0), (10, 0, 0), (0, 10, 0), (0, 0, 10)]
TRIS = [(0, 2, 1), (0, 1, 3), (1, 2, 3), (0, 3, 2)]


def load(path):
    """Returns (ok, triangle_count, size_xyz, {'#RRGGBB': triangle_count}, raw_output)."""
    out = subprocess.run([BENCH, path], capture_output=True, text=True).stdout
    head = re.search(r"^ok\s+[\d.]+ ms\s+(\d+) tris\s+size ([\d.]+) x ([\d.]+) x ([\d.]+)", out, re.M)
    if not head:
        return False, 0, None, {}, out
    colors = {m.group(1): int(m.group(2)) for m in re.finditer(r"^\s+(#[0-9A-F]{6})\s+(\d+) tris", out, re.M)}
    return True, int(head.group(1)), tuple(float(head.group(i)) for i in (2, 3, 4)), colors, out


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def model_xml(resources, build, extra_ns=""):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"'
            f'{extra_ns}>\n<resources>{resources}</resources>\n<build>{build}</build>\n</model>\n')


def mesh_xml(tri_attrs=None):
    tri_attrs = tri_attrs or [""] * len(TRIS)
    v = "".join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in VERTS)
    t = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"{extra}/>' for (a, b, c), extra in zip(TRIS, tri_attrs))
    return f"<mesh><vertices>{v}</vertices><triangles>{t}</triangles></mesh>"


def write_3mf(path, files):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)


# ------------------------------------------------------------------ tests

def test_stl_binary(d):
    p = os.path.join(d, "tetra_bin.stl")
    with open(p, "wb") as f:
        f.write(b"\0" * 80 + struct.pack("<I", len(TRIS)))
        for a, b, c in TRIS:
            f.write(struct.pack("<12fH", 0, 0, 0, *VERTS[a], *VERTS[b], *VERTS[c], 0))
    ok, n, size, _, out = load(p)
    assert ok and n == 4 and size == (10, 10, 10), out


def test_stl_ascii(d):
    p = os.path.join(d, "tetra_ascii.stl")
    body = "".join(
        "facet normal 0 0 0\n outer loop\n" + "".join(f"  vertex {x:e} {y:e} {z:e}\n" for x, y, z in (VERTS[i] for i in t))
        + " endloop\nendfacet\n" for t in TRIS)
    write(p, f"solid tetra\n{body}endsolid tetra\n")
    ok, n, size, _, out = load(p)
    assert ok and n == 4 and size == (10, 10, 10), out


def test_obj_materials(d):
    p = os.path.join(d, "tetra.obj")
    write(os.path.join(d, "tetra.mtl"), "newmtl red\nKd 1 0 0\nnewmtl blue paint\nKd 0 0 1\n")
    v = "".join(f"v {x} {y} {z}\n" for x, y, z in VERTS)
    # One quad (two triangles after fan triangulation) in red, one triangle in blue using negative indices.
    write(p, f"mtllib tetra.mtl\n{v}usemtl red\nf 1 2 3 4\nusemtl blue paint\nf -4 -3 -1\n")
    ok, n, _, colors, out = load(p)
    assert ok and n == 3 and colors == {"#FF0000": 2, "#0000FF": 1}, out


def test_obj_vertex_colors(d):
    p = os.path.join(d, "vcolor.obj")
    write(p, "v 0 0 0 0 1 0\nv 1 0 0 0 1 0\nv 0 1 0 0 1 0\nf 1 2 3\n")
    ok, n, _, colors, out = load(p)
    assert ok and n == 1 and colors == {"#00FF00": 1}, out


def test_3mf_color_groups(d):
    p = os.path.join(d, "groups.3mf")
    ns = ' xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02"'
    res = ('<basematerials id="1"><base name="a" displaycolor="#FF0000"/><base name="b" displaycolor="#00FF00FF"/></basematerials>'
           '<m:colorgroup id="2"><m:color color="#0000FF"/></m:colorgroup>'
           # object default = base material 1; triangles override via pid/p1
           '<object id="3" type="model" pid="1" pindex="1">'
           + mesh_xml(['', ' pid="1" p1="0"', ' pid="2" p1="0"', ' p1="0"']) + '</object>')
    write_3mf(p, {"3D/3dmodel.model": model_xml(res, '<item objectid="3"/>', ns)})
    ok, n, _, colors, out = load(p)
    assert ok and n == 4 and colors == {"#00FF00": 1, "#FF0000": 2, "#0000FF": 1}, out


def test_3mf_components_and_transforms(d):
    p = os.path.join(d, "components.3mf")
    res = (f'<object id="1" type="model">{mesh_xml()}</object>'
           '<object id="2" type="model"><components>'
           '<component objectid="1"/>'
           '<component objectid="1" transform="1 0 0 0 1 0 0 0 1 100 0 0"/>'
           '</components></object>')
    # Item scales by 2: the far instance ends at (100 + 10) * 2 = 220.
    write_3mf(p, {"3D/3dmodel.model": model_xml(res, '<item objectid="2" transform="2 0 0 0 2 0 0 0 2 0 0 0"/>')})
    ok, n, size, _, out = load(p)
    assert ok and n == 8 and size == (220, 20, 20), out


BAMBU_SETTINGS = """<?xml version="1.0" encoding="UTF-8"?>
<config><object id="5"><metadata key="extruder" value="1"/>
<part id="1" subtype="normal_part"><metadata key="extruder" value="2"/></part>
<part id="2" subtype="normal_part"><metadata key="extruder" value="1"/></part>
<part id="3" subtype="modifier_part"><metadata key="extruder" value="3"/></part>
</object></config>"""
BAMBU_PROJECT = '{"filament_colour": ["#111111", "#22AA22", "#3333FF"], "other": 1}'


def test_3mf_bambu_parts_and_paint(d):
    """Bambu/Orca layout: production-extension sub-model, per-part filaments, painted triangles, a modifier."""
    p = os.path.join(d, "bambu.3mf")
    ns = ' xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06"'
    # Painted triangle in part 2: split on 3 sides into 4 children. Nibbles in stream order:
    # 3 (split, 3 sides), then children in reverse creation order: 4 (state 1), 8 (state 2), 0 (unpainted), 4 (state 1).
    # The attribute stores them back to front.
    painted = [' paint_color="40843"', ' paint_color="8"', ' paint_color="0C"', '']  # "0C": nibble C (leaf, extended state) then 0 -> state 3
    sub = model_xml(f'<object id="1" type="model">{mesh_xml()}</object>'
                    f'<object id="2" type="model">{mesh_xml(painted)}</object>'
                    f'<object id="3" type="model">{mesh_xml()}</object>', "")
    comps = "".join(f'<component p:path="/3D/Objects/object_1.model" objectid="{i}"/>' for i in (1, 2, 3))
    root = model_xml(f'<object id="5" type="model"><components>{comps}</components></object>',
                     '<item objectid="5" transform="1 0 0 0 1 0 0 0 1 50 50 0"/>', ns)
    write_3mf(p, {"3D/3dmodel.model": root, "3D/Objects/object_1.model": sub,
                  "Metadata/model_settings.config": BAMBU_SETTINGS, "Metadata/project_settings.config": BAMBU_PROJECT})
    ok, n, _, colors, out = load(p)
    # part 1: 4 tris in filament 2. part 3 (modifier): skipped.
    # part 2 (filament 1): split tri -> 2x filament 1, 1x filament 2, 1x unpainted (filament 1);
    #                      one tri filament 2; one tri filament 3; one unpainted (filament 1).
    assert ok and n == 11 and colors == {"#22AA22": 6, "#111111": 4, "#3333FF": 1}, out


def test_3mf_prusa_volumes_and_paint(d):
    """PrusaSlicer layout: per-volume extruders by triangle range, mmu_segmentation painting, a modifier volume."""
    p = os.path.join(d, "prusa.3mf")
    ns = ' xmlns:slic3rpe="http://schemas.slic3r.org/3mf/2017/06"'
    res = f'<object id="1" type="model">{mesh_xml(["", " slic3rpe:mmu_segmentation=\"8\"", "", ""])}</object>'
    cfg = ('<config><object id="1"><metadata type="object" key="extruder" value="1"/>'
           '<volume firstid="0" lastid="1"><metadata type="volume" key="extruder" value="0"/>'
           '<metadata type="volume" key="volume_type" value="ModelPart"/></volume>'
           '<volume firstid="2" lastid="2"><metadata type="volume" key="extruder" value="3"/></volume>'
           '<volume firstid="3" lastid="3"><metadata type="volume" key="volume_type" value="ParameterModifier"/></volume>'
           '</object></config>')
    ini = "; some = thing\n; extruder_colour = #AA0000;\"\";#0000AA\n; filament_colour = #010101;#00BB00;#020202\n"
    write_3mf(p, {"3D/3dmodel.model": model_xml(res, '<item objectid="1"/>', ns),
                  "Metadata/Slic3r_PE_model.config": cfg, "Metadata/Slic3r_PE.config": ini})
    ok, n, _, colors, out = load(p)
    # tri 0: object extruder 1; tri 1: painted with extruder 2 (falls back to filament colour); tri 2: extruder 3; tri 3: modifier, skipped.
    assert ok and n == 3 and colors == {"#AA0000": 1, "#00BB00": 1, "#0000AA": 1}, out


def test_3mf_stored_entries(d):
    p = os.path.join(d, "stored.3mf")
    with zipfile.ZipFile(p, "w", zipfile.ZIP_STORED) as z:
        z.writestr("3D/3dmodel.model", model_xml(f'<object id="1" type="model">{mesh_xml()}</object>', '<item objectid="1"/>'))
    ok, n, _, _, out = load(p)
    assert ok and n == 4, out


def test_step_colors(d):
    ok, n, size, colors, out = load(os.path.join(ROOT, "tests", "fixtures", "two_boxes.step"))
    if "STEP support was not built" in out:
        return "skipped (built without OpenCASCADE)"
    assert ok and n == 24 and size == (30, 20, 30) and colors == {"#FF0000": 12, "#0000FF": 12}, out


def test_step_cache(d):
    """A cached STEP load returns the same mesh, and editing the file invalidates the cache."""
    import shutil
    cache = os.path.join(d, "cache")
    os.mkdir(cache)
    p = os.path.join(d, "cached.step")
    shutil.copy(os.path.join(ROOT, "tests", "fixtures", "two_boxes.step"), p)
    env = dict(os.environ, QL3D_CACHE_DIR=cache)
    run = lambda: subprocess.run([BENCH, p], capture_output=True, text=True, env=env).stdout
    first = run()
    if "STEP support was not built" in first:
        return "skipped (built without OpenCASCADE)"
    strip = lambda out: re.sub(r"[\d.]+ ms", "", out)
    assert len(os.listdir(cache)) == 1, os.listdir(cache)
    assert strip(run()) == strip(first) and "24 tris" in first, first
    # Corrupt the source but keep the cache: a changed file must not be served from the old entry.
    with open(p, "w") as f:
        f.write("not a step file")
    assert "FAIL" in run()


def test_invalid_files_fail_cleanly(d):
    for name, data in (("garbage.3mf", b"not a zip"), ("empty.obj", b"# nothing\n"), ("garbage.step", b"hello"),
                       ("short.stl", b"abc")):
        p = os.path.join(d, name)
        with open(p, "wb") as f:
            f.write(data)
        r = subprocess.run([BENCH, p], capture_output=True, text=True)
        assert r.returncode == 0 and "FAIL" in r.stdout, f"{name}: {r.stdout}{r.stderr}"


def main():
    if not os.path.exists(BENCH):
        sys.exit("build/ql3d-bench not found: run ./build.sh first")
    tests = [(k, v) for k, v in globals().items() if k.startswith("test_")]
    failed = 0
    with tempfile.TemporaryDirectory() as d:
        for name, fn in tests:
            try:
                note = fn(d)
                print(f"PASS  {name}" + (f"  [{note}]" if note else ""))
            except AssertionError as e:
                failed += 1
                print(f"FAIL  {name}\n{e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
