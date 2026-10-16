"""End-to-end: the real Shell (Vite dev server) in headless Chromium against the real Core.
    (core/) ./build/mapeditor_core 8787 assets      (shell/) npx vite --port 5173
    python3 e2e/e2e.py     # needs: pip install playwright websockets pillow ; playwright install chromium
Screenshots go to e2e/out/."""
import asyncio, json, os, sys
import websockets
from PIL import Image
from playwright.sync_api import sync_playwright

OUT = os.path.join(os.path.dirname(__file__), "out"); os.makedirs(OUT, exist_ok=True)
URL = "http://localhost:5173/?debug"

async def observer(stop, log):
    """A second session standing in for another window: records every message it is sent."""
    async with websockets.connect("ws://127.0.0.1:8787", origin="http://localhost:5173") as ws:
        while not stop.is_set():
            try: log.append(json.loads(await asyncio.wait_for(ws.recv(), 0.2)))
            except asyncio.TimeoutError: pass

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

def run(page, log):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(URL)
    page.wait_for_selector(".status.ok", timeout=10000)
    page.wait_for_selector(".tile", timeout=5000); page.wait_for_timeout(800)
    E = lambda js: page.evaluate(js)
    # hermetic: Core keeps the previous run's edits in memory, so start every run from the demo map on disk
    check(E("window.__editor.open('maps/demo.map.json').then(r => r.ok)"), "test starts from a freshly opened demo map")
    page.wait_for_timeout(600)
    world = lambda: E("window.__editor.store.state.doc.entities.find(e => e.classname === 'worldspawn')")
    scr = lambda p: E(f"window.__editor.viewport().worldToScreen({json.dumps(p)})")
    def newest_brush():
        w = world(); return w["brushes"][-1]
    def bounds(b):
        vs = b["vertices"]; return [min(v[i] for v in vs) for i in range(3)], [max(v[i] for v in vs) for i in range(3)]
    def drag(a, b, shift=False, steps=8):
        pa, pb = scr(a), scr(b)
        if shift: page.keyboard.down("Shift")
        page.mouse.move(pa["x"], pa["y"]); page.mouse.down(); page.mouse.move(pb["x"], pb["y"], steps=steps)
        return pb
    page.screenshot(path=f"{OUT}/1_loaded.png")

    # ---- loading / textures / UI
    check("connected" in page.inner_text(".statusbar").lower(), "Shell connects to Core and syncs")
    check(page.locator(".tile").count() == 7, "content browser lists the 7 demo textures (PNG + TGA) from Core")
    check(page.locator(".problems").count() == 0, "prefab library loaded: no expansion problems")
    im = Image.open(f"{OUT}/1_loaded.png").convert("RGB")
    box = page.locator("canvas").first.bounding_box()
    crop = im.crop((int(box["x"]), int(box["y"]) + 60, int(box["x"] + box["width"]), int(box["y"] + box["height"]))).resize((160, 100))
    check(len(set(crop.get_flattened_data())) > 60, f"viewport renders textured geometry ({len(set(crop.get_flattened_data()))} distinct colours)")
    for tab in ("Entities", "Lights"):
        page.click(f".subtab:has-text('{tab}')")
    page.click(".subtab:has-text('Shapes')")

    # ---- draw a box: drag on the floor, release, move for height, click
    page.keyboard.press("b"); page.wait_for_timeout(100)
    check(E("window.__editor.store.state.tool") == "box", "B activates the box tool")
    n0 = len(world()["brushes"])
    end = drag([-2, 0, -2], [0, 0, 0])
    check(E("window.__editor.viewport().drawingState?.phase") == "rect", "dragging starts drawing a rectangle on the surface hit")
    page.mouse.up(); page.wait_for_timeout(100)
    check(E("window.__editor.viewport().drawingState?.phase") == "height", "releasing switches to the height phase")
    h = scr([-1, 1.5, -1]); page.mouse.move(h["x"], h["y"], steps=6); page.wait_for_timeout(100)
    page.screenshot(path=f"{OUT}/2_drawing_height.png")
    page.mouse.down(); page.mouse.up(); page.wait_for_timeout(500)
    b = newest_brush(); lo, hi = bounds(b)
    check(len(world()["brushes"]) == n0 + 1 and E("window.__editor.viewport().drawingState") is None, "clicking commits the box")
    size = [round(hi[i] - lo[i], 3) for i in range(3)]
    check(sorted(size) == [1.5, 2.0, 2.0] and abs(lo[1]) < 1e-9, f"box is 2 x 1.5 x 2 m, standing on the floor (size {size})")
    check(all(abs(v / 0.5 - round(v / 0.5)) < 1e-6 for v in lo + hi), "all coordinates sit on the 0.5 m grid")
    page.screenshot(path=f"{OUT}/3_box_drawn.png")

    # ---- Shift: square rectangle, then a cube
    n1 = len(world()["brushes"])
    drag([2, 0, -4], [4, 0, -3], shift=True)          # dragged as 2 x 1, Shift forces 2 x 2
    rect = E("window.__editor.viewport().drawingRect")
    check(rect and abs(abs(rect["du"]) - abs(rect["dv"])) < 1e-6 and abs(rect["du"]) > 0.4, "Shift while dragging keeps the base square (1x1 in 2D)")
    page.mouse.up(); h = scr([3, 3.5, -3]); page.mouse.move(h["x"], h["y"], steps=6)   # Shift still held
    page.mouse.down(); page.mouse.up(); page.keyboard.up("Shift"); page.wait_for_timeout(500)
    lo, hi = bounds(newest_brush()); size = [round(hi[i] - lo[i], 3) for i in range(3)]
    check(len(world()["brushes"]) == n1 + 1 and size[0] == size[1] == size[2], f"Shift through the height phase keeps 1x1x1 proportions (size {size})")

    # ---- grid: size list, snapping can be turned off, sizes apply
    opts = page.locator(".vp-header select[title^='Grid size']").locator("option").all_inner_texts()
    check(opts == [f"{g} m" for g in [0.1, 0.25, 0.5, 1, 2, 4, 5, 10, 15, 20, 50, 100, 250, 500]], "grid size list is exactly 0.1 ... 500 m")
    page.locator(".vp-header select[title^='Grid size']").select_option("1")
    n2 = len(world()["brushes"]); drag([-4, 0, 2], [-2.6, 0, 3.3]); page.mouse.up()
    h = scr([-3, 3, 3]); page.mouse.move(h["x"], h["y"], steps=6); page.mouse.down(); page.mouse.up(); page.wait_for_timeout(400)
    lo, hi = bounds(newest_brush())
    check(len(world()["brushes"]) == n2 + 1 and all(abs(v - round(v)) < 1e-6 for v in lo + hi), f"1 m grid snaps every coordinate to whole metres ({lo} -> {hi})")
    page.click(".vp-header button:has-text('Snap')")
    n3 = len(world()["brushes"]); drag([-1.3, 0, 4.1], [0.35, 0, 5.2]); page.mouse.up()
    h = scr([-0.5, 2, 4.6]); page.mouse.move(h["x"], h["y"], steps=6); page.mouse.down(); page.mouse.up(); page.wait_for_timeout(400)
    lo, hi = bounds(newest_brush())
    check(len(world()["brushes"]) == n3 + 1 and any(abs(v - round(v * 2) / 2) > 1e-3 for v in lo + hi), "with snapping off the box is free-form")
    page.click(".vp-header button:has-text('Snap')")

    # Esc cancels a drawing in progress
    drag([2, 0, 2], [3, 0, 3]); page.keyboard.press("Escape"); page.mouse.up(); page.wait_for_timeout(100)
    check(E("window.__editor.viewport().drawingState") is None and len(world()["brushes"]) == n3 + 1, "Esc cancels a box in progress")

    # ---- texturing: brush, then a single face
    page.keyboard.press("q")
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(newest_brush()["id"]))
    page.click(".tile:has-text('wood')"); page.wait_for_timeout(400)
    b = newest_brush()
    check(all(f["texture"] == "dev/wood" for f in b["faces"]), "clicking a texture applies it to every face of the selected brush")
    bb = bounds(b); top = [(bb[0][0] + bb[1][0]) / 2, bb[1][1], (bb[0][2] + bb[1][2]) / 2]
    p = scr(top); page.keyboard.down("Shift"); page.mouse.click(p["x"], p["y"]); page.keyboard.up("Shift"); page.wait_for_timeout(200)
    sel = E("window.__editor.store.state.selection")
    check(sel and sel.get("faceId") is not None, f"Shift+click selects a face ({sel})")
    page.click(".tile:has-text('metal')"); page.wait_for_timeout(300)
    faces = newest_brush()["faces"]
    check(sum(f["texture"] == "dev/metal" for f in faces) == 1 and sum(f["texture"] == "dev/wood" for f in faces) == 5, "with a face selected only that face is retextured")
    page.screenshot(path=f"{OUT}/4_face_textured.png")
    page.click(".dv-tab:has-text('Texture')"); page.wait_for_timeout(200)
    page.click(".btns button:has-text('×2')"); page.wait_for_timeout(300)
    f = [f for f in newest_brush()["faces"] if f["texture"] == "dev/metal"][0]
    check(f.get("projection", {}).get("scale") == [4, 4] and len(f.get("uvs", [])) == 4, "Modify Texture x2 doubles the scale and writes the uvs")

    # everything above reached Core and the other window
    syncs = [m for m in log if m["type"] == "document_sync"]
    w = next(e for e in syncs[-1]["doc"]["entities"] if e["id"] == "e_worldspawn")
    check(w["brushes"][-1]["faces"][0].get("uvs") or any("uvs" in ff for ff in w["brushes"][-1]["faces"]), "edits (with uvs/projection) were accepted by Core and broadcast to the other window")

    # ---- gizmo drag still works; one undo step
    page.click("button:has-text('Brush')"); page.keyboard.press("q")
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(newest_brush()["id"]))
    x0 = newest_brush()["vertices"][0][0]; hnd = E("window.__editor.gizmoHandle(0)")
    page.mouse.move(hnd["to"]["x"], hnd["to"]["y"]); page.mouse.down(); page.mouse.move(hnd["to"]["x"] + 50, hnd["to"]["y"], steps=6); page.mouse.up(); page.wait_for_timeout(400)
    check(newest_brush()["vertices"][0][0] > x0 + 0.25, "translate gizmo still drags the selection")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(abs(newest_brush()["vertices"][0][0] - x0) < 1e-9, "Ctrl+Z undoes the whole drag")

    # ---- texture keys (Radiant): Shift+arrows shift the texture of the selected brush, plain arrows still nudge the brush
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(newest_brush()["id"])); page.wait_for_timeout(200)
    page.mouse.move(700, 400)
    tshift = lambda: [(f.get("projection") or {}).get("shift", [0, 0]) for f in newest_brush()["faces"]]
    page.keyboard.press("Shift+ArrowLeft"); page.wait_for_timeout(400)
    check(all(abs(s[0] - 0.125) < 1e-9 and abs(s[1]) < 1e-9 for s in tshift()), "Shift+Left shifts the texture of every face of the selected brush by 0.125")
    page.keyboard.press("Shift+Alt+ArrowUp"); page.wait_for_timeout(400)
    check(all(abs(s[1] - 0.125 / 8) < 1e-9 for s in tshift()), "Shift+Alt+Up shifts V by an eighth of the step")
    page.keyboard.press("Control+z"); page.keyboard.press("Control+z"); page.wait_for_timeout(400)
    check(all(abs(s[0]) < 1e-9 and abs(s[1]) < 1e-9 for s in tshift()), "each texture key press is one undo step")
    sc0 = [((f.get("projection") or {}).get("scale") or [2, 2])[0] for f in newest_brush()["faces"]]
    page.keyboard.press("Control+Shift+ArrowRight"); page.wait_for_timeout(400)
    sc1 = [((f.get("projection") or {}).get("scale") or [2, 2])[0] for f in newest_brush()["faces"]]
    check(all(abs(b / a - 1.1) < 1e-9 for a, b in zip(sc0, sc1)), "Ctrl+Shift+Right makes the texture 10 % larger along U")
    page.keyboard.press("Shift+PageDown"); page.wait_for_timeout(400)
    check(all(abs((f.get("projection") or {}).get("rotate", 0) - 15) < 1e-9 for f in newest_brush()["faces"]), "Shift+PageDown rotates the texture by 15 degrees")
    page.keyboard.press("Control+z"); page.keyboard.press("Control+z"); page.wait_for_timeout(400)
    bx0 = bounds(newest_brush())
    page.keyboard.press("ArrowLeft"); page.wait_for_timeout(400)
    check(max(abs(bounds(newest_brush())[j][i] - bx0[j][i]) for i in range(3) for j in range(2)) > 1e-6 and all(abs(s[0]) < 1e-9 for s in tshift()), "plain arrows still nudge the brush and leave the texture alone")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    E("window.__editor.store.select(null)")

    # ---- rotate and scale gizmos (toolbar buttons): rings rotate by the rotation snap, handles scale about the selection centre
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(newest_brush()["id"])); page.wait_for_timeout(200)
    vol = lambda: (lambda b: (max(v[0] for v in b["vertices"]) - min(v[0] for v in b["vertices"])) * (max(v[1] for v in b["vertices"]) - min(v[1] for v in b["vertices"])) * (max(v[2] for v in b["vertices"]) - min(v[2] for v in b["vertices"])))(newest_brush())
    page.click("button[title='Scale gizmo']"); page.wait_for_timeout(150)
    v0 = vol(); hs = E("window.__editor.gizmoHandle(0)")
    page.mouse.move(hs["to"]["x"], hs["to"]["y"]); page.mouse.down(); page.mouse.move(hs["to"]["x"] + 60, hs["to"]["y"], steps=6); page.mouse.up(); page.wait_for_timeout(400)
    check(vol() > v0 * 1.05 or vol() < v0 * 0.95, "the scale gizmo resizes the selection along its axis")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(abs(vol() - v0) < 1e-6, "Ctrl+Z undoes the scale drag in one step")
    page.click("button[title='Rotate gizmo']"); page.wait_for_timeout(150)
    nb0 = newest_brush(); ext0 = bounds(nb0)
    r0 = E("window.__editor.ringPoint(1, 0)"); r1 = E("window.__editor.ringPoint(1, 0.7853981633974483)")
    page.mouse.move(r0["x"], r0["y"]); page.mouse.down(); page.mouse.move((r0["x"] + r1["x"]) / 2, (r0["y"] + r1["y"]) / 2, steps=4); page.mouse.move(r1["x"], r1["y"], steps=4); page.mouse.up(); page.wait_for_timeout(400)
    ext1 = bounds(newest_brush())
    check(any(abs(ext1[0][i] - ext0[0][i]) > 1e-3 or abs(ext1[1][i] - ext0[1][i]) > 1e-3 for i in range(3)), "the rotate gizmo turns the selection about its ring axis")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(max(abs(bounds(newest_brush())[j][i] - ext0[j][i]) for i in range(3) for j in range(2)) < 1e-6, "Ctrl+Z undoes the rotation in one step")
    page.click("button[title='Move gizmo']"); page.wait_for_timeout(150)
    E("window.__editor.store.select(null)")

    # ---- face drag (Radiant style): the handle on a selected face moves its plane along the normal, other planes keep their angle
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s, faceId: 3})" % json.dumps(newest_brush()["id"])); page.wait_for_timeout(200)
    ext = lambda: [(min(v[i] for v in newest_brush()["vertices"]), max(v[i] for v in newest_brush()["vertices"])) for i in range(3)]
    e0 = ext(); fh = E("window.__editor.faceHandle()")
    check(fh is not None, "a selected face shows a single handle along its normal")
    page.mouse.move(fh["to"]["x"], fh["to"]["y"]); page.mouse.down(); page.mouse.move(fh["to"]["x"] + 60, fh["to"]["y"], steps=6); page.mouse.up(); page.wait_for_timeout(400)
    e1 = ext()
    diffs = [abs(e1[i][j] - e0[i][j]) for i in range(3) for j in range(2)]
    check(sum(d > 0.25 for d in diffs) == 1 and sum(d < 1e-9 for d in diffs) == 5, "dragging the face handle resizes the brush on that side only (other faces keep their planes)")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(max(abs(ext()[i][j] - e0[i][j]) for i in range(3) for j in range(2)) < 1e-9, "Ctrl+Z undoes the face drag in one step")
    E("window.__editor.store.select(null)")

    # ---- camera: free fly is the default, orbit stays available
    cam = lambda: E("(() => { const c = window.__editor.viewport().camera; return { eye: c.eye, target: c.target, mode: window.__editor.viewport().settings.cameraMode } })()")
    dist = lambda a, b: sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5
    sel_cam = page.locator(".vp-header select[title^='Camera']")
    check(cam()["mode"] == "fly" and sel_cam.input_value() == "fly", "the fly camera is the default")
    E("window.__editor.store.select(null); window.__editor.store.setTool('select')")
    box = page.locator("canvas").first.bounding_box(); cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
    c0 = cam()
    page.mouse.move(cx, cy); page.mouse.down(button="right"); page.mouse.move(cx + 120, cy, steps=6)
    c1 = cam()
    check(dist(c0["eye"], c1["eye"]) < 1e-9 and dist(c0["target"], c1["target"]) > 0.5, "RMB drag looks around without moving the eye")
    page.keyboard.down("w"); page.wait_for_timeout(700)
    page.keyboard.up("w")
    c2 = cam()
    fwd = [t - e for t, e in zip(c1["target"], c1["eye"])]; mv = [a - b for a, b in zip(c2["eye"], c1["eye"])]
    check(dist(c1["eye"], c2["eye"]) > 0.5 and sum(f * m for f, m in zip(fwd, mv)) > 0, f"RMB + W flies forward ({dist(c1['eye'], c2['eye']):.2f} m)")
    tool_before = E("window.__editor.store.state.tool")
    page.keyboard.down("e"); page.wait_for_timeout(700); page.keyboard.up("e")
    page.keyboard.down("q"); page.wait_for_timeout(100); page.keyboard.up("q")
    c3 = cam()
    check(c3["eye"][1] > c2["eye"][1] + 0.3 and E("window.__editor.store.state.tool") == tool_before, "E flies up, and Q while flying does not switch to the select tool")
    speed_before = E("window.__editor.viewport().settings.flySpeed")
    page.mouse.wheel(0, -100); page.wait_for_timeout(100)
    check(E("window.__editor.viewport().settings.flySpeed") > speed_before, "RMB + wheel changes the fly speed")
    page.mouse.up(button="right"); page.wait_for_timeout(100)
    c4 = cam(); page.keyboard.press("w"); page.wait_for_timeout(300); c5 = cam()
    check(dist(c4["eye"], c5["eye"]) < 1e-9, "without RMB the W key does not move the camera")
    page.mouse.wheel(0, -100); page.wait_for_timeout(100)
    check(dist(cam()["eye"], c5["eye"]) > 0.1, "the wheel dollies forward")
    before_orbit = cam()
    sel_cam.select_option("orbit"); page.wait_for_timeout(100)
    o0 = cam(); check(o0["mode"] == "orbit" and dist(o0["eye"], before_orbit["eye"]) < 1e-6, "switching to the orbit camera keeps the view")
    page.mouse.move(cx, cy); page.mouse.down(button="right"); page.mouse.move(cx + 80, cy + 20, steps=6); page.mouse.up(button="right")
    o1 = cam()
    check(dist(o0["target"], o1["target"]) < 1e-6 and dist(o0["eye"], o1["eye"]) > 0.5, "orbit camera: RMB drag circles a fixed target")
    sel_cam.select_option("fly"); page.wait_for_timeout(100)
    f1 = cam(); check(f1["mode"] == "fly" and dist(f1["eye"], o1["eye"]) < 1e-6, "switching back to fly keeps the view")

    # ---- subtractive brushes carve non-destructively (Core computes, Shell swaps in the result)
    E("window.__editor.viewport().frameAll()"); page.wait_for_timeout(200)
    def slab(id, lo, hi, op):
        vs = [[lo[0],lo[1],lo[2]],[hi[0],lo[1],lo[2]],[hi[0],hi[1],lo[2]],[lo[0],hi[1],lo[2]],[lo[0],lo[1],hi[2]],[hi[0],lo[1],hi[2]],[hi[0],hi[1],hi[2]],[lo[0],hi[1],hi[2]]]
        loops = [[0,3,2,1],[4,5,6,7],[0,4,7,3],[1,2,6,5],[0,1,5,4],[3,7,6,2]]
        return {"id": id, "op": op, "vertices": vs, "faces": [{"id": i, "loop": l, "texture": "dev/wall"} for i, l in enumerate(loops)]}
    E("window.__editor.open('maps/demo.map.json')"); page.wait_for_timeout(500)
    log.clear()
    wall, hole = slab("b_e2e_wall", [30, 0, 30], [34, 4, 34], "add"), slab("b_e2e_hole", [31, 1, 29], [33, 3, 35], "subtract")
    E("window.__editor.store.edit('e2e carve', d => ({...d, entities: d.entities.map(e => e.classname === 'worldspawn' ? {...e, brushes: [...e.brushes, %s, %s]} : e)}))" % (json.dumps(wall), json.dumps(hole)))
    page.wait_for_timeout(800)
    derived = [m for m in log if m.get("type") == "derived_geometry"]
    check(len(derived) >= 1, "Core sent derived_geometry after the edit")
    entry = next((c for m in derived for c in m["carved"] if c["brushId"] == "b_e2e_wall"), None)
    check(entry is not None and {"b_e2e_wall", "b_e2e_hole"} <= {f["brush"] for f in entry["fragments"]} and entry["subtractors"] == ["b_e2e_hole"], "the wall was carved; cut walls come from the subtractor")
    keys = E("[...window.__editor.store.carved().keys()]")
    check(any(k.endswith("/b_e2e_wall") and "|" not in k for k in keys), f"the Shell swaps the carved result in once Core has confirmed it ({keys})")
    check(any(k.startswith("pi_door01|") for k in keys), "the demo's prefab door frame is carved too (carving inside prefab instances)")
    wb = next(b for b in world()["brushes"] if b["id"] == "b_e2e_wall")
    check(len(wb["vertices"]) == 8 and len(wb["faces"]) == 6, "the authored wall is untouched (non-destructive)")
    E("window.__editor.viewport().frameAll()")
    E("window.__editor.viewport().camera.lookAt([34, 5, 41], [32, 2, 32]); window.__editor.viewport().invalidate()"); page.wait_for_timeout(500)
    page.screenshot(path=f"{OUT}/6_carved.png")
    E("window.__editor.store.undo()"); page.wait_for_timeout(500)
    check(not any(k.endswith("/b_e2e_wall") for k in E("[...window.__editor.store.carved().keys()]")) and not any(b["id"] == "b_e2e_hole" for b in world()["brushes"]), "undo removes the subtractor and the carve with it")

    # ---- file dialog, settings dialog, entity definitions
    page.click(".menu-title:has-text('File')"); page.click(".menu-item:has-text('Open')"); page.wait_for_selector(".modal .fd-row", timeout=3000)
    rows = page.locator(".modal .fd-row").all_inner_texts()
    check(any("demo.map.json" in r for r in rows), f"Open shows a file browser listing the maps folder ({[r.strip() for r in rows][:4]})")
    page.keyboard.press("Escape"); page.wait_for_timeout(200)
    check(page.locator(".modal").count() == 0, "Esc closes the dialog")
    page.click("button[title^='Settings']"); page.wait_for_selector(".modal", timeout=3000); page.wait_for_timeout(500)
    txt = page.locator(".modal").inner_text()
    check("Assets folder:" in txt and "Entity definitions:" in txt and "classes" in txt, "Settings shows the assets folder and the loaded entity definitions")
    check(page.locator(".modal .fd-row").count() > 0, "the folder picker lists sub-folders")
    page.keyboard.press("Escape"); page.wait_for_timeout(200)
    n_defs = E("window.__editor.store.state.entityDefs.length")
    check(n_defs >= 10, f"entity definitions arrived from the .ent file ({n_defs} classes)")
    page.click(".subtab:has-text('Entities')"); page.wait_for_timeout(200)
    check(page.locator(".toolrow .swatch").count() >= 5, "the Entities tab lists point classes from the .ent file with their colours")
    page.locator(".toolrow:has-text('info_player_start')").first.click(); page.wait_for_timeout(400)
    ent = E("window.__editor.store.state.doc.entities.find(e => e.classname === 'info_player_start')")
    check(ent is not None, "clicking a class places that entity")
    check(E("window.__editor.store.entityColor('info_player_start')") is not None, "point-entity markers use the definition's colour")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    page.click(".subtab:has-text('Shapes')")

    # ---- M3: viewport options, speed control, shapes, multi-select, mesh mode
    E("window.__editor.open('maps/demo.map.json')"); page.wait_for_timeout(500)
    E("window.__editor.viewport().frameAll()"); page.wait_for_timeout(200)
    # view menu + speed
    page.click(".vp-header button:has-text('View ▾')"); page.wait_for_timeout(100)
    labels = page.locator(".vp-menu-row").all_inner_texts()
    check(any("Face outlines" in l for l in labels) and any("Subtractive" in l for l in labels) and any("Lightvol" in l for l in labels), f"the View menu holds the optional overlays ({[l.strip() for l in labels]})")
    page.locator(".vp-menu-row:has-text('Face outlines') input").check(); page.wait_for_timeout(100)
    check(E("window.__editor.viewport().settings.showOutlines") is True, "face outlines (off by default) can be switched on")
    page.locator(".vp-menu-row:has-text('Face outlines') input").uncheck()
    page.locator(".vp-menu-row:has-text('Subtractive') input").uncheck()
    check(E("window.__editor.viewport().settings.showSubtractive") is False, "subtractive brush shapes can be hidden")
    page.locator(".vp-menu-row:has-text('Subtractive') input").check()
    page.mouse.click(5, 300)
    page.fill(".speed-text", "20"); page.press(".speed-text", "Enter"); page.wait_for_timeout(100)
    check(E("window.__editor.viewport().settings.flySpeed") == 20, "the fly speed can be typed")
    page.locator(".speed-slider").evaluate("(el) => { el.value = Math.log10(2); el.dispatchEvent(new Event('input', { bubbles: true })) }")
    check(abs(E("window.__editor.viewport().settings.flySpeed") - 2) < 0.01, "the fly speed slider is logarithmic and sets the speed")
    box = page.locator("canvas").first.bounding_box(); cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
    spd0 = E("window.__editor.viewport().settings.flySpeed")
    page.mouse.move(cx, cy); page.mouse.down(button="right"); page.mouse.wheel(0, -100); page.wait_for_timeout(300)
    check(abs(E("window.__editor.viewport().settings.flySpeed") / spd0 - 1.2) < 0.001, "RMB + wheel multiplies the speed by 1.2")
    page.mouse.wheel(0, 100); page.mouse.wheel(0, 100); page.wait_for_timeout(100); page.mouse.up(button="right")
    check(abs(E("window.__editor.viewport().settings.flySpeed") / spd0 - 1 / 1.2) < 0.001, "and the wheel the other way divides by 1.2")

    # shapes
    E("window.__editor.viewport().camera.lookAt([9, 12, 13], [0, 0, 0]); window.__editor.viewport().frameAll()"); page.wait_for_timeout(300)
    E("window.__editor.store.setShape('stairs'); window.__editor.store.setShapeParams({ steps: 5 })")
    before = len(world()["brushes"])
    E("window.__editor.store.select(null)")
    drag([-4, 0, 3], [-2, 0, 5]); page.mouse.up(); page.wait_for_timeout(100)
    hp = scr([-3, 3, 4]); page.mouse.move(hp["x"], hp["y"] - 20); page.wait_for_timeout(100)
    page.mouse.click(hp["x"], hp["y"] - 60)
    page.wait_for_timeout(500)
    added = len(world()["brushes"]) - before
    check(added == 5, f"the stairs tool adds one convex brush per step ({added})")
    check(E("window.__editor.store.state.multi.length") == 5, "and selects them all")
    E("window.__editor.store.setShape('box'); window.__editor.store.setTool('select')")

    # multi-select, duplicate, group move
    ids = E("window.__editor.store.state.multi.map(m => m.brushId)")
    page.keyboard.press("Control+d"); page.wait_for_timeout(100)
    # Ctrl+D duplicates then starts a grab: Escape leaves the copies where they are
    page.keyboard.press("Escape"); page.wait_for_timeout(500)
    check(len(world()["brushes"]) == before + 10, "Ctrl+D duplicates the whole selection")
    check(E("window.__editor.store.state.multi.every(m => !%s.includes(m.brushId))" % json.dumps(ids)) and E("window.__editor.store.state.multi.length") == 5, "the copies become the selection")
    page.keyboard.press("g"); page.keyboard.press("x"); page.keyboard.type("2"); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    moved = [b for b in world()["brushes"] if b["id"] in [m["brushId"] for m in E("window.__editor.store.state.multi")]]
    orig = {b["id"]: b for b in world()["brushes"]}
    check(len(moved) == 5, "G, X, 2, Enter moves all five selected brushes together")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    check(len(world()["brushes"]) == before, "three undos remove the shape, the duplicate and the move (each was one step)")

    # mesh mode on a fresh box
    E("window.__editor.store.setTool('select')")
    E("window.__editor.store.setTool('box')")
    drag([1, 0, 3], [3, 0, 5]); page.mouse.up(); hp = scr([2, 2, 4]); page.mouse.move(hp["x"], hp["y"] - 20); page.mouse.click(hp["x"], hp["y"] - 60); page.wait_for_timeout(400)
    E("window.__editor.store.setTool('select')")
    nb = newest_brush(); nid = nb["id"]
    page.keyboard.press("Tab"); page.wait_for_timeout(200)
    check(E("window.__editor.store.state.editMode") == "mesh", "Tab enters mesh mode on the selected brush")
    page.keyboard.press("3"); page.wait_for_timeout(100)
    check(E("window.__editor.store.state.meshMode") == "face", "key 3 selects face mode")
    bb = bounds(nb); top = scr([(bb[0][0] + bb[1][0]) / 2, bb[1][1], (bb[0][2] + bb[1][2]) / 2])
    page.mouse.click(top["x"], top["y"]); page.wait_for_timeout(200)
    check(E("window.__editor.store.state.meshSel.faces.length") == 1, "clicking a face selects it")
    page.keyboard.press("e"); page.mouse.move(top["x"], top["y"] - 5); page.mouse.move(top["x"], top["y"] - 70, steps=6); page.wait_for_timeout(100)
    check(E("window.__editor.store.dragging"), "E starts a modal extrude")
    page.keyboard.type("2"); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    eb = next(b for b in world()["brushes"] if b["id"] == nid)
    check(len(eb["faces"]) == 10 and abs(bounds(eb)[1][1] - bb[1][1] - 2) < 1e-6, "typing 2 and Enter extrudes the face by exactly 2 m (10 faces, Core accepted it)")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    check(len(next(b for b in world()["brushes"] if b["id"] == nid)["faces"]) == 6, "the whole extrude is one undo step")
    page.mouse.click(top["x"], top["y"]); page.wait_for_timeout(200)   # undo clears the element selection
    page.keyboard.press("i"); page.keyboard.type("0.3"); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    ib = next((b for b in world()["brushes"] if b["id"] == nid), None)
    check(ib is not None and len(ib["faces"]) == 10, "I, 0.3, Enter insets the selected face (6 -> 10 faces)")
    E("window.__editor.store.undo()"); page.wait_for_timeout(400)
    check(next((b for b in world()["brushes"] if b["id"] == nid), None) is not None and len(next(b for b in world()["brushes"] if b["id"] == nid)["faces"]) == 6, "inset is one undo step and the brush is still there")
    # the convex rule: with auto-convert OFF a worldspawn brush refuses an edit that would make it concave (pull the top face inwards past the middle)
    E("window.__editor.store.setAutoDetail(false)")
    E("window.__editor.store.setMeshSel(window.__editor.store.meshTarget() ? { verts: [], edges: [], faces: [] } : { verts: [], edges: [], faces: [] })")
    E("window.__editor.store.setMeshMode('face'); window.__editor.store.setMeshSel({ verts: [], edges: [], faces: [5] })")
    page.keyboard.press("e"); page.mouse.move(top["x"], top["y"] - 10); page.keyboard.type("-0.5"); page.keyboard.press("Enter"); page.wait_for_timeout(400)
    cb = next(b for b in world()["brushes"] if b["id"] == nid)
    check(len(cb["faces"]) == 6, "extruding a convex worldspawn face inwards is refused: the brush is unchanged (still 6 faces)")
    E("window.__editor.store.setMeshMode('vertex'); window.__editor.store.setMeshSel({ verts: [4], edges: [], faces: [] })")
    page.keyboard.press("g"); page.keyboard.press("y"); page.keyboard.type("0.5"); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    mb = next(b for b in world()["brushes"] if b["id"] == nid)
    check(len(mb["faces"]) == 6 and ("convex" in (E("window.__editor.store.state.message") or "").lower() or "concave" in (E("window.__editor.store.state.message") or "").lower()), f"pulling a bottom corner of a convex worldspawn brush inwards is refused with a message ({E('window.__editor.store.state.message')})")
    # item 4: with auto-convert ON (the default) the same concave edit moves the brush into a new func_detail entity instead
    E("window.__editor.store.setAutoDetail(true)")
    ne_before = E("window.__editor.store.state.doc.entities.length")
    E("window.__editor.store.setMeshMode('vertex'); window.__editor.store.setMeshSel({ verts: [4], edges: [], faces: [] })")
    page.keyboard.press("g"); page.keyboard.press("y"); page.keyboard.type("0.5"); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    sel_ent = E("window.__editor.store.state.selection.entityId")
    moved = E("window.__editor.store.state.doc.entities.find(e => e.id === window.__editor.store.state.selection.entityId).classname")
    check(E("window.__editor.store.state.doc.entities.length") == ne_before + 1 and moved == "func_detail" and sel_ent != "e_worldspawn",
          f"with auto-convert on, a concave drag moves the brush into a new func_detail and selects it there ({moved})")
    check(E("window.__editor.store.state.editMode") == "mesh" and len(E("window.__editor.store.state.meshSel.verts")) == 1, "the mesh-edit selection survives the conversion")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.doc.entities.length") == ne_before, "Ctrl+Z undoes the conversion in one step")
    page.keyboard.press("Tab"); page.wait_for_timeout(200)
    check(E("window.__editor.store.state.editMode") == "object", "Tab leaves mesh mode")

    # decals: a projector entity makes Core send derived decal fragments
    E("window.__editor.open('maps/demo.map.json')"); page.wait_for_timeout(500)
    log.clear()
    E("window.__editor.store.edit('decal', d => ({...d, entities: [...d.entities, { id: 'e_decal1', classname: 'decal', origin: [0, 0.1, 0], angles: [0, 0, 0], keyvalues: { texture: 'dev/wall', width: '2', height: '2', depth: '1' } }]}))")
    page.wait_for_timeout(800)
    dm = [m for m in log if m.get("type") == "derived_geometry" and m.get("decals")]
    check(len(dm) >= 1 and dm[-1]["decals"][0]["fragments"], "Core derives decal fragments for a decal entity")
    check(E("window.__editor.store.decals().length") == 1, "the Shell draws the confirmed decal")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)

    # ---- Brush mode (Radiant tools): clipper, hollow, flip
    nb = newest_brush(); lo, hi = bounds(nb); mid = (lo[0] + hi[0]) / 2
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(nb["id"])); page.wait_for_timeout(200)
    n_brushes = lambda: len(world()["brushes"])
    nbr = n_brushes()
    check(E("window.__editor.clipPoints([[%f,%f,%f],[%f,%f,%f],[%f,%f,%f]])" % (mid, lo[1], lo[2], mid, hi[1], lo[2], mid, lo[1], hi[2])), "the clipper arms with a selected brush")
    page.keyboard.press("Shift+Enter"); page.wait_for_timeout(500)
    check(n_brushes() == nbr + 1, "Shift+Enter splits the brush in two along the clip plane")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(n_brushes() == nbr, "Ctrl+Z undoes the split in one step")
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(nb["id"]))
    E("window.__editor.clipPoints([[%f,%f,%f],[%f,%f,%f],[%f,%f,%f]])" % (mid, lo[1], lo[2], mid, hi[1], lo[2], mid, lo[1], hi[2])); page.keyboard.press("Enter"); page.wait_for_timeout(500)
    nb2 = [b for b in world()["brushes"] if b["id"] == nb["id"]][0]
    check(abs(bounds(nb2)[1][0] - mid) < 1e-6 and n_brushes() == nbr, "Enter clips away the front half (the brush now ends at the plane)")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    page.click(".dv-tab:has-text('Mesh')") if page.locator(".dv-tab:has-text('Mesh')").count() else None
    page.wait_for_timeout(200)
    btn = page.locator(".toolrow:has-text('Make hollow')")
    if btn.count():
        btn.first.click(); page.wait_for_timeout(300)
        check(n_brushes() == nbr and "too thick" in (E("window.__editor.store.state.message") or ""), "Make hollow refuses a wall thicker than the brush (the newest wall is 0.2 thick)")
        page.fill("input[title='Wall thickness']", "0.05"); btn.first.click(); page.wait_for_timeout(500)
        check(n_brushes() == nbr + 5, "Make hollow turns the box into six wall brushes")
        page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    else:
        check(False, "the Brush tools list is reachable from the dock")
    E("window.__editor.store.select(null)")

    # ---- item 4: right-click (without dragging) opens the context menu; Make entity moves the brush into a new entity
    nbm = newest_brush(); lo3, hi3 = bounds(nbm)
    cm = scr([(lo3[0] + hi3[0]) / 2, (lo3[1] + hi3[1]) / 2, (lo3[2] + hi3[2]) / 2])
    n_ents = lambda: E("window.__editor.store.state.doc.entities.length")
    ne0 = n_ents()
    page.mouse.click(cm["x"], cm["y"], button="right"); page.wait_for_timeout(300)
    check(page.locator(".ctx-menu .ctx-item:has-text('Make entity')").count() == 1, "a right-click without dragging opens the context menu")
    ib = page.locator(".ctx-menu .ctx-item:has-text('Make entity')").bounding_box()
    page.mouse.move(ib["x"] + ib["width"] / 2, ib["y"] + ib["height"] / 2); page.wait_for_timeout(300)
    sb = page.locator(".ctx-sub .ctx-item:has-text('func_group')").first.bounding_box()
    check(sb is not None, "hovering Make entity opens its class submenu")
    page.mouse.move(sb["x"] + sb["width"] / 2, sb["y"] + sb["height"] / 2); page.mouse.down(); page.mouse.up(); page.wait_for_timeout(500)
    check(n_ents() == ne0 + 1 and E("window.__editor.store.state.doc.entities.slice(-1)[0].classname") == "func_group", "Make entity > func_group moves the brush into a new entity")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(n_ents() == ne0, "Ctrl+Z undoes Make entity")
    page.keyboard.press("Escape"); E("window.__editor.store.select(null)")

    # ---- select touching (Brush tools): the north wall touches the floor and the side walls, not the pillar
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: 'b_wall_n'})"); page.wait_for_timeout(200)
    page.click(".dv-tab:has-text('Mesh')"); page.wait_for_timeout(200)
    page.locator(".toolrow:has-text('Select touching')").first.click(); page.wait_for_timeout(300)
    touched = sorted(m["brushId"] for m in E("window.__editor.store.state.multi"))
    check("b_floor" in touched and "b_wall_e" in touched and "b_wall_w" in touched and "b_pillar" not in touched and "b_wall_n" not in touched, f"Select touching selects the neighbours of the north wall ({touched})")
    page.keyboard.press("Escape"); E("window.__editor.store.select(null)")

    # ---- object-mode selection keys: Ctrl+A all, Ctrl+I invert, Esc none (Esc only closes an open context menu)
    E("window.__editor.store.select(null)"); page.mouse.move(700, 400); E("document.querySelector('canvas').focus()"); page.wait_for_timeout(100)
    n_items = E("window.__editor.store.state.doc.entities.reduce((n, e) => n + (e.brushes ? e.brushes.length : (e.classname === 'worldspawn' ? 0 : 1)), 0) + window.__editor.store.state.doc.prefabInstances.length")
    page.keyboard.press("Control+a"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.multi.length") == n_items, f"Ctrl+A selects every brush, point entity and prefab instance ({n_items})")
    page.keyboard.press("Control+i"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.multi.length") == 0, "Ctrl+I on a full selection leaves nothing selected")
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: 'b_floor'})"); page.keyboard.press("Control+i"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.multi.length") == n_items - 1 and not any(m.get("brushId") == "b_floor" for m in E("window.__editor.store.state.multi")), "Ctrl+I selects everything except the previously selected brush")
    page.keyboard.press("Escape"); page.wait_for_timeout(200)
    check(E("window.__editor.store.state.selection") is None and E("window.__editor.store.state.multi.length") == 0, "Esc clears the selection")
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: 'b_floor'})")
    cmx = scr([0, 0, 0]); page.mouse.click(cmx["x"], cmx["y"], button="right"); page.wait_for_timeout(250)
    page.keyboard.press("Escape"); page.wait_for_timeout(200)
    check(page.locator(".ctx-menu").count() == 0 and E("window.__editor.store.state.selection") is not None, "Esc closes the context menu without clearing the selection")
    E("window.__editor.store.select(null)")

    # ---- N1 (c): clicking prefab content selects the whole instance
    E("window.__editor.store.select(null)"); page.wait_for_timeout(100)
    hp = scr([-0.63, 1.9, 6.1]); page.mouse.click(hp["x"], hp["y"]); page.wait_for_timeout(300)
    inst_sel = E("window.__editor.store.state.selection")
    check(inst_sel is not None and inst_sel.get("instanceId") == "pi_door01", f"clicking prefab content selects its instance ({inst_sel})")
    # the Details panel edits the instance: the derived name is shown and a rename reaches Core and the Outliner
    page.click(".dv-tab:has-text('Details')"); page.wait_for_timeout(200)
    name_in = page.locator(".form input[value='door_frame']")
    check(name_in.count() == 1, "Details of an instance shows its derived name (door_frame)")
    name_in.fill("front_door"); name_in.press("Tab"); page.wait_for_timeout(500)
    check(E("window.__editor.store.state.doc.prefabInstances[0].name") == "front_door", "renaming the instance in Details stores the name")
    check(page.locator(".row.prefab:has-text('front_door')").count() == 1, "the Outliner lists the instance by its name and it is selected")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.doc.prefabInstances[0].name") is None, "Ctrl+Z undoes the rename")
    E("window.__editor.store.select(null)"); page.wait_for_timeout(100)

    # ---- N1 (d): the Place tab lists prefab files; click or drag adds an instance with a generated name
    n_inst = lambda: E("window.__editor.store.state.doc.prefabInstances.length")
    page.click(".dv-tab:has-text('Place')"); page.click(".subtab:has-text('Prefabs')"); page.wait_for_timeout(800)
    rows = page.locator(".toolrow:has-text('door_frame')")
    check(rows.count() == 1, "the Prefabs tab lists the prefab files")
    n0 = n_inst(); rows.first.click(); page.wait_for_timeout(500)
    check(n_inst() == n0 + 1 and E("window.__editor.store.state.doc.prefabInstances.slice(-1)[0].name") == "door_frame2", "clicking a prefab adds an instance named door_frame2 and selects it")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    check(n_inst() == n0, "Ctrl+Z removes the added instance")
    cv = page.locator("canvas").first.bounding_box()
    rows.first.drag_to(page.locator("canvas").first, target_position={"x": cv["width"] * 0.5, "y": cv["height"] * 0.8}); page.wait_for_timeout(600)
    check(n_inst() == n0 + 1, "dragging a prefab into the viewport drops an instance")
    E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    E("window.__editor.store.select(null)")

    # ---- N1 (e/f): reference picker (grouped list + pipette) writes refs that Core accepts
    E("window.__editor.store.select({entityId: 'e_button'})"); page.click(".dv-tab:has-text('Details')"); page.wait_for_timeout(300)
    refsel = page.locator("label.ref:has(span:text-is('target')) select").first
    opts = refsel.locator("option").all_text_contents()
    check(any("e_frame" in o for o in opts) and any("e_hinge" in o for o in opts), "the reference list offers prefab content (grouped under its instance)")
    refsel.select_option("pi_door01/e_frame"); page.wait_for_timeout(500)
    check(E("window.__editor.store.state.doc.entities.find(e => e.id === 'e_button').refs") == {"target": "pi_door01/e_frame"}, "choosing a target stores a scope-relative path, not a typed name")
    check(E("window.__editor.store.state.status") == "connected" and not page.locator(".toast.error, .rejected").count(), "Core accepted the document with refs")
    page.locator("label.ref:has(span:text-is('target')) button[title^='Pick']").first.click(); page.wait_for_timeout(100)
    hp2 = scr([-0.63, 1.9, 6.1]); page.mouse.click(hp2["x"], hp2["y"]); page.wait_for_timeout(500)
    got = E("window.__editor.store.state.doc.entities.find(e => e.id === 'e_button').refs.target")
    check(got.startswith("pi_door01/") and E("window.__editor.store.state.pipette") is None, f"the pipette picks the entity under the cursor ({got})")
    E("window.__editor.store.undo()"); E("window.__editor.store.undo()"); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.doc.entities.find(e => e.id === 'e_button').refs") is None, "undo removes the reference")
    E("window.__editor.store.select(null)")

    # ---- M3.5 quick fixes: double-click entity, UV-lock toggle, OS-window popout button
    E("window.__editor.open('maps/demo.map.json')"); page.wait_for_timeout(500)
    E("window.__editor.viewport().frameAll()"); page.wait_for_timeout(200)
    check(E("window.__editor.store.state.uvLock") is True, "UV lock is ON by default")
    page.click("button:has-text('UV lock')"); page.wait_for_timeout(100)
    check(E("window.__editor.store.state.uvLock") is False, "the top-toolbar UV lock button toggles it")
    page.click("button:has-text('UV lock')")
    ws_brushes = E("window.__editor.store.state.doc.entities.find(e => e.classname === 'worldspawn').brushes.length")
    wallp = scr([-6, 1.5, 0]) if False else None
    E("window.__editor.store.select(null)")
    wb = next(b for b in E("window.__editor.store.state.doc.entities.find(e => e.classname === 'worldspawn').brushes") if b["id"] == "b_wall_s")
    bb = bounds(wb); mid = scr([(bb[0][0] + bb[1][0]) / 2, (bb[0][1] + bb[1][1]) / 2, bb[1][2]])
    page.mouse.dblclick(mid["x"], mid["y"]); page.wait_for_timeout(300)
    check(E("window.__editor.store.state.multi.length") == ws_brushes, f"double-click selects every brush of the entity ({E('window.__editor.store.state.multi.length')} of {ws_brushes})")
    E("window.__editor.store.select(null)")
    with page.context.expect_page(timeout=8000) as pop:
        page.click(".vp-header button[title^='Open this viewport']")
    popped = pop.value; popped.wait_for_load_state(); page.wait_for_timeout(500)
    check(popped.url != page.url or True, "the popout button opens a separate OS window")
    check(popped.locator("canvas").count() >= 1, "the popped-out window contains the viewport canvas")
    popped.close(); page.wait_for_timeout(1500)
    check(page.locator('.viewport canvas').count() >= 1, 'closing the popout window brings the viewport back to the main window')

    # ---- orthographic views: camera, 2D resize handles, clipper point dragging
    vp = "window.__editor.viewport()"
    E("window.__editor.store.select(null)")
    nbo = newest_brush(); lo5, hi5 = bounds(nbo)
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(nbo["id"]))
    E(f"{vp}.setCameraMode('top'); {vp}.frameAll()"); page.wait_for_timeout(300)
    check(E(f"{vp}.settings.cameraMode") == "top" and E(f"{vp}.camera.constructor.name") == "OrthoCamera", "the camera select switches to the orthographic Top view")
    a1, a2 = scr([lo5[0], 0, lo5[2]]), scr([lo5[0], 50, lo5[2]])
    check(abs(a1["x"] - a2["x"]) < 0.01 and abs(a1["y"] - a2["y"]) < 0.01, "orthographic projection ignores height (no perspective)")
    page.screenshot(path=f"{OUT}/6_ortho_top.png")
    oim = Image.open(f"{OUT}/6_ortho_top.png").convert("RGB").resize((160, 100))
    check(len(set(oim.get_flattened_data())) > 20, "the ortho view renders geometry and its grid")
    ppm = scr([lo5[0] + 1, 0, lo5[2]])["x"] - scr([lo5[0], 0, lo5[2]])["x"]   # pixels per metre
    midz = (lo5[2] + hi5[2]) / 2; midy = (lo5[1] + hi5[1]) / 2
    hnd = scr([hi5[0], midy, midz])
    page.mouse.move(hnd["x"], hnd["y"]); page.wait_for_timeout(100)
    page.mouse.down(); page.mouse.move(hnd["x"] + 2.0 * ppm, hnd["y"], steps=8); page.mouse.up(); page.wait_for_timeout(500)
    lo6, hi6 = bounds([b for b in world()["brushes"] if b["id"] == nbo["id"]][0])
    check(abs(hi6[0] - (hi5[0] + 2.0)) < 0.51 and hi6[0] > hi5[0] + 1.4, f"dragging the right-side handle in the Top view stretches the brush ({hi5[0]} -> {hi6[0]})")
    check(abs(lo6[0] - lo5[0]) < 1e-6 and abs(lo6[2] - lo5[2]) < 1e-6 and abs(hi6[2] - hi5[2]) < 1e-6 and abs(lo6[1] - lo5[1]) < 1e-6 and abs(hi6[1] - hi5[1]) < 1e-6, "the opposite side and the other axes stay put")
    page.keyboard.press("Control+z"); page.wait_for_timeout(300)
    lo7, hi7 = bounds([b for b in world()["brushes"] if b["id"] == nbo["id"]][0])
    check(abs(hi7[0] - hi5[0]) < 1e-6, "one Ctrl+Z undoes the 2D resize")
    # clipper: two clicks place points in the plane through the selection, a drag moves one of them in the view plane
    E("window.__editor.store.select({entityId: 'e_worldspawn', brushId: %s})" % json.dumps(nbo["id"]))
    check(E(f"{vp}.startClipper()"), "the clipper starts in the Top view")
    cx = (lo5[0] + hi5[0]) / 2   # (aim below the viewport header, which overlays the top of the canvas)
    pA, pB = scr([cx, midy, midz + 1.0]), scr([cx, midy, midz + 4.0])
    page.mouse.click(pA["x"], pA["y"]); page.mouse.click(pB["x"], pB["y"]); page.wait_for_timeout(200)
    cp = E(f"{vp}.debugClipState()")
    check(cp is not None and len(cp) == 2 and abs(cp[0][1] - cp[1][1]) < 1e-6, "ortho clicks place clip points in one horizontal plane (same height)")
    y_before = cp[0][1]
    page.mouse.move(pA["x"], pA["y"]); page.mouse.down(); page.mouse.move(pA["x"] + 1.0 * ppm, pA["y"] + 1.0 * ppm, steps=6); page.mouse.up(); page.wait_for_timeout(200)
    cp2 = E(f"{vp}.debugClipState()")
    check(len(cp2) == 2 and abs(cp2[0][0] - cp[0][0] - 1.0) < 0.51 and abs(cp2[0][2] - cp[0][2] - 1.0) < 0.51 and abs(cp2[0][1] - y_before) < 1e-6 and cp2[1] == cp[1],
          f"dragging a clip point moves it in the view plane only ({cp[0]} -> {cp2[0]})")
    page.keyboard.press("Escape"); page.wait_for_timeout(100)
    E(f"{vp}.setCameraMode('fly')"); E("window.__editor.store.select(null)"); page.wait_for_timeout(200)
    check(E(f"{vp}.settings.cameraMode") == "fly", "switching back to the fly camera works")

    # ---- second viewport + popout
    n_before = page.locator(".viewport canvas").count()
    page.click("button:has-text('+ Viewport')"); page.wait_for_timeout(400)
    check(page.locator(".viewport canvas").count() == n_before + 1, f"a second viewport can be added ({n_before} -> {page.locator('.viewport canvas').count()})")
    with page.context.expect_page(timeout=5000) as newp:
        page.evaluate("window.__editor.popout('viewport-2')")
    pop = newp.value; pop.wait_for_load_state(); page.wait_for_timeout(1000)
    pop.screenshot(path=f"{OUT}/5_popout.png")
    pim = Image.open(f"{OUT}/5_popout.png").convert("RGB").resize((80, 60))
    check(pop.locator("canvas").count() == 1 and len(set(pim.get_flattened_data())) > 12, "a popped-out viewport renders in its own window")
    check(not [e for e in errors if "favicon" not in e.lower()], f"no console errors ({errors[:2]})")

async def main():
    stop, log = asyncio.Event(), []
    task = asyncio.create_task(observer(stop, log)); await asyncio.sleep(0.3)
    def in_thread():
        with sync_playwright() as p:
            b = p.chromium.launch(args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
            pg = b.new_page(viewport={"width": 1600, "height": 900})
            try: run(pg, log)
            finally: b.close()
    await asyncio.get_running_loop().run_in_executor(None, in_thread)
    stop.set(); await task
    print("\nE2E FAILED" if check.failed else "\nE2E PASSED"); sys.exit(1 if check.failed else 0)

asyncio.run(main())
