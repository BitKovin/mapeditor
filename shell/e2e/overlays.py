"""End-to-end for the lighting overlays against the real Core (View > Lightvol samples, View > Lightmap density, Texture panel density).
    shell/e2e/run_lit.sh overlays
What it proves in a real WebGL2 context:
  * both overlay shaders compile and draw (no console errors); switching a View entry on starts the lighting by itself, also in the Unlit view;
  * the density overlay paints the ramp colours (blue .. red) over the lit mesh, and the legend names the range;
  * the lightvol overlay paints smooth-shaded probe spheres;
  * a face's lightmapScale set through the Texture panel makes the Core build a denser lightmap for it (the density range grows) and the overlay follows."""
import json, os, sys, time
from PIL import Image
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
URL = "http://localhost:5173/?debug"
MAP_REL = "maps/overlays_e2e.map.json"
MAP = os.path.join(HERE, "..", "..", "core", "assets", MAP_REL)

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

def make_map():
    d = json.load(open(os.path.join(os.path.dirname(MAP), "demo.map.json")))
    d["entities"].append({"id": "e_sun", "classname": "light_point", "origin": [0, 3, 0], "angles": [0, 0, 0], "brushes": [],
                          "keyvalues": {"brightness": "150", "radius": "40"}})
    json.dump(d, open(MAP, "w"))

def toggle_view(page, label):
    page.get_by_text("View ▾").first.click()
    page.locator(".vp-menu-row", has_text=label).first.locator("input").click()
    page.get_by_text("View ▾").first.click()  # close

def viewport_pixels(page, name):
    box = page.locator("canvas").first.bounding_box()
    path = f"{OUT}/{name}.png"
    page.screenshot(path=path)
    im = Image.open(path).convert("RGB").crop((int(box["x"]) + 10, int(box["y"]) + 160, int(box["x"] + box["width"]) - 10, int(box["y"] + box["height"]) - 5))
    return list(im.get_flattened_data())

def hue_counts(px):
    blue = sum(1 for r, g, b in px if b > 180 and r < 80 and g < 110)
    cyan = sum(1 for r, g, b in px if g > 160 and b > 160 and r < 60)
    green = sum(1 for r, g, b in px if g > 170 and r < 90 and b < 110)
    yellow = sum(1 for r, g, b in px if r > 200 and g > 170 and b < 80)
    red = sum(1 for r, g, b in px if r > 200 and g < 80 and b < 80)
    return blue, cyan, green, yellow, red

def run(page):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(URL); page.wait_for_selector(".status.ok", timeout=15000); page.wait_for_timeout(800)
    E = page.evaluate
    check(E(f"window.__editor.open('{MAP_REL}').then(r => r.ok)"), "the test map opens")
    page.wait_for_timeout(800)
    check(E("window.__editor.store.lightmap.ready") is False, "no lighting before an overlay asks for it (Unlit view)")
    base = viewport_pixels(page, "ov_0_unlit")

    # ---- density overlay, in the Unlit view: it starts the lighting
    toggle_view(page, "Lightmap density")
    t0 = time.time()
    while time.time() - t0 < 240:
        st = E("(()=>{const s=window.__editor.store;return {conv:!!s.lightmap.status?.converged, vol:s.lightvol.ready, mesh:s.lightmap.ready}})()")
        if st["conv"] and st["vol"]: break
        page.wait_for_timeout(500)
    print("lighting state after %.0fs: %s" % (time.time() - t0, st))
    check(st["mesh"], "switching Lightmap density on started the lighting (a lit mesh arrived in the Unlit view)")
    page.wait_for_timeout(800)
    px = viewport_pixels(page, "ov_1_density")
    counts = hue_counts(px)
    print("density ramp pixels (blue, cyan, green, yellow, red): %s of %d" % (counts, len(px)))
    check(sum(counts) > 1500, f"the density overlay paints ramp colours over the scene ({sum(counts)} pixels)")
    differs = sum(1 for a, b in zip(base, px) if a != b)
    check(differs > len(px) * 0.1, f"the overlay changes the picture ({differs} pixels differ from Unlit)")
    legend = page.locator(".vp-label", has_text="Lightmap texels/m").first.inner_text()
    print("legend:", legend)
    check("Lightmap texels/m" in legend, "the legend names the density range")
    # the demo level's density: the editor default is 8 texels per metre = green
    check(counts[2] > counts[0] and counts[2] > counts[4], "the default density (8 texels/m) is the green part of the ramp, not blue or red")

    # ---- a denser face: select a face of the floor, set the lightmap scale to 4 in the Texture panel, the Core rebuilds, red shows up
    before_max = E("(()=>{const m=window.__editor.store.lightmap.mesh;return m?m.length:0})()")
    faces = E("(()=>{const d=window.__editor.store.state.doc;const e=d.entities[0];return e.brushes.map(b=>[b.id,b.faces.length])})()")
    print("worldspawn brushes:", faces[:4])
    E("(()=>{const ed=window.__editor;const b=ed.store.state.doc.entities[0].brushes[0];ed.store.select({entityId:ed.store.state.doc.entities[0].id,brushId:b.id})})()")
    page.wait_for_timeout(300)
    rev0 = E("window.__editor.store.lightmap.rev")
    page.locator(".dv-tab", has_text="Texture").first.click(); page.wait_for_timeout(400)
    field = page.locator(".section", has_text="Lightmap texel density").locator("xpath=following-sibling::div[1]//input").first
    field.fill("4"); field.dispatch_event("change")
    t0 = time.time()
    while time.time() - t0 < 120:
        scales = E("window.__editor.store.state.doc.entities[0].brushes[0].faces.map(f=>f.lightmapScale)")
        ok = all(s == 4 for s in scales)
        conv = E("!!window.__editor.store.lightmap.status?.converged")
        if ok and conv: break
        page.wait_for_timeout(400)
    check(ok, "the Texture panel stored lightmapScale 4 on every face of the brush")
    t0 = time.time()
    while time.time() - t0 < 120:  # the Core rebuilds from the new document: the density range of the mesh grows
        rng = E("(()=>{const l=window.__editor.store.lightmap;return l.mesh?{rev:l.rev,conv:!!l.status?.converged}:null})()")
        if rng and rng["rev"] > rev0: break
        page.wait_for_timeout(500)
    print("after the edit: %s (%.0fs)" % (rng, time.time() - t0))
    page.wait_for_timeout(2500)
    px2 = viewport_pixels(page, "ov_2_density_dense")
    c2 = hue_counts(px2)
    print("after scale 4 (blue, cyan, green, yellow, red): %s" % (c2,))
    warm = lambda p: sum(1 for r, g, b in p if r > 180 and b < 120 and g < 215)
    print("warm (yellow / orange / red) pixels before / after: %d / %d" % (warm(px), warm(px2)))
    check(warm(px2) > warm(px) + 40, "the denser brush shows warm colours (yellow .. red: 32 texels/m) that were not there before")
    legend2 = page.locator(".vp-label", has_text="Lightmap texels/m").first.inner_text()
    print("legend:", legend2)
    check(legend2 != legend, "the legend range follows")

    # ---- lightvol overlay
    toggle_view(page, "Lightmap density")  # density off
    toggle_view(page, "Lightvol samples")
    page.wait_for_timeout(1500)
    px3 = viewport_pixels(page, "ov_3_lightvol")
    diff3 = sum(1 for a, b in zip(base, px3) if a != b)
    check(diff3 > 600, f"the lightvol overlay draws probes ({diff3} pixels differ from the plain Unlit view)")
    check(not [e for e in errors if "favicon" not in e.lower()], f"no console errors ({errors[:3]})")

with sync_playwright() as p:
    b = p.chromium.launch(args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
    pg = b.new_page(viewport={"width": 1600, "height": 900})
    make_map()
    try: run(pg)
    finally:
        b.close()
        try: os.remove(MAP)
        except OSError: pass
sys.exit(1 if check.failed else 0)
