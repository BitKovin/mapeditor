"""Generates the demo map + prefab fixtures (schema 0.2).

Units: meters. Axes: Y is up, the ground plane is XZ (DESIGN.md §4).
Not part of the build -- run it to regenerate the JSON if you tweak numbers:
    python3 gen_demo.py
"""
import json
import os

OUT = os.path.dirname(os.path.abspath(__file__))
TEXTURE_METERS = 2.0  # one texture repeat per 2 m (simple box-mapped UVs)


def r(x):
    return round(x, 6)


def box_brush(brush_id, mins, maxs, texture, op="add"):
    """Axis-aligned box as an indexed polygon mesh. Every loop is CCW seen from outside."""
    (x0, y0, z0), (x1, y1, z1) = mins, maxs
    verts = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    # (loop, uv axes) for -Z, +Z, -X, +X, -Y, +Y
    faces_def = [((0, 3, 2, 1), (0, 1)), ((4, 5, 6, 7), (0, 1)), ((0, 4, 7, 3), (2, 1)),
                 ((1, 2, 6, 5), (2, 1)), ((0, 1, 5, 4), (0, 2)), ((3, 7, 6, 2), (0, 2))]
    faces = []
    for fid, (loop, (ua, va)) in enumerate(faces_def):
        faces.append({
            "id": fid,
            "loop": list(loop),
            "uvs": [[r(verts[i][ua] / TEXTURE_METERS), r(verts[i][va] / TEXTURE_METERS)] for i in loop],
            "texture": texture,
        })
    return {"id": brush_id, "op": op, "vertices": [[r(c) for c in v] for v in verts], "faces": faces}


def entity(eid, classname, brushes, origin=(0, 0, 0), keyvalues=None):
    return {"id": eid, "classname": classname, "keyvalues": keyvalues or {},
            "origin": list(origin), "angles": [0, 0, 0], "brushes": brushes}


def document(entities, prefab_instances):
    return {"schemaVersion": "0.2", "entities": entities, "prefabInstances": prefab_instances}


def instance(iid, file, origin):
    return {"id": iid, "file": file, "origin": list(origin), "angles": [0, 0, 0], "overrides": {}}


def write(rel, doc):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")


# A hinge: a tiny dynamic (non-structural) entity, so it shows a custom-origin
# gizmo even when it arrives through two levels of prefab nesting.
write("prefabs/hinge.map.json", document([
    entity("e_hinge", "prop_hinge",
           [box_brush("b_hinge", (-0.03, 0, -0.03), (0.03, 0.2, 0.03), "dev/metal")],
           origin=(0, 0.1, 0), keyvalues={"model": "models/hinge.mdl"}),
], []))

# A door frame: additive frame + SUBTRACTIVE doorway (the reason booleans
# exist), and the hinge prefab instanced twice (a "diamond": fine, not a cycle).
# Local space: wall plane is Z = 0, frame is 0.3 m thick, opening 1.2 x 2.2 m.
write("prefabs/door_frame.map.json", document([
    entity("e_frame", "func_group", [
        box_brush("b_frame", (-0.9, 0, -0.15), (0.9, 2.5, 0.15), "dev/wood", op="add"),
        box_brush("b_opening", (-0.6, 0, -0.3), (0.6, 2.2, 0.3), "dev/grey", op="subtract"),
    ]),
], [
    instance("pi_hinge_top", "prefabs/hinge.map.json", (-0.63, 1.9, 0)),
    instance("pi_hinge_bottom", "prefabs/hinge.map.json", (-0.63, 0.2, 0)),
]))

# The demo room: 12 x 12 m, 3 m walls. The door frame is instanced into the
# north (+Z) wall; its subtractive brush will carve the opening once booleans exist.
write("maps/demo.map.json", document([
    entity("e_worldspawn", "worldspawn", [
        box_brush("b_floor", (-6, -0.2, -6.2), (6, 0, 6.2), "dev/floor"),
        box_brush("b_wall_n", (-6, 0, 6), (6, 3, 6.2), "dev/wall"),
        box_brush("b_wall_s", (-6, 0, -6.2), (6, 3, -6), "dev/wall"),
        box_brush("b_wall_e", (6, 0, -6.2), (6.2, 3, 6.2), "dev/wall"),
        box_brush("b_wall_w", (-6.2, 0, -6.2), (-6, 3, 6.2), "dev/wall"),
    ]),
    entity("e_pillar", "func_detail", [box_brush("b_pillar", (3, 0, 3), (3.4, 2.4, 3.4), "dev/concrete")]),
    entity("e_button", "func_button", [box_brush("b_button", (-4.6, 1.1, -4.6), (-4.4, 1.3, -4.4), "dev/metal")],
           origin=(-4.5, 1.2, -4.5), keyvalues={"target": "door01"}),
], [
    instance("pi_door01", "prefabs/door_frame.map.json", (0, 0, 6.1)),
]))

print("wrote maps/demo.map.json, prefabs/door_frame.map.json, prefabs/hinge.map.json")
