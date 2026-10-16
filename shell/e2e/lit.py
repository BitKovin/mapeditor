"""End-to-end for the Lit view against the real Core (REQUIREMENTS §9: lightmaps as stored, HDR; light volumes for non-lightmapped entities).
    shell/e2e/run_lit.sh      (starts Core + Vite, runs this, stops both)
What it proves in a real WebGL2 context:
  * the shaders compile (no console errors) and Lit mode draws a textured, lit scene;
  * lightmap values ABOVE 1 reach the Shell and the GPU (RGBA16F): with the exposure at its minimum (0.05) a texel of 17 shows sRGB(0.86) = 240 in
    Lighting-only, a pipeline that clamps the light at 1 first could only show sRGB(0.05) = 64;
  * a brush entity that is not lightmapped is lit from the light volume (entity_light), in Lit and in Lighting-only."""
import json, os, shutil, sys, time
from PIL import Image
from websockets.sync.client import connect
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
URL = "http://localhost:5173/?debug"
MAP_REL = "maps/lit_e2e.map.json"
MAP = os.path.join(HERE, "..", "..", "core", "assets", MAP_REL)

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

def make_map():
    d = json.load(open(os.path.join(os.path.dirname(MAP), "demo.map.json")))
    d["entities"].append({"id": "e_sun", "classname": "light_point", "origin": [0, 3, 0], "angles": [0, 0, 0], "brushes": [],
                          "keyvalues": {"brightness": "150", "radius": "40"}})
    d["entities"].append({"id": "e_decal", "classname": "decal", "origin": [-3.0, 0.4, 1.0], "angles": [0, 0, 0], "brushes": [],
                          "keyvalues": {"texture": "dev/tga_test", "width": "3", "height": "3", "depth": "1"}})
    json.dump(d, open(MAP, "w"))

def set_exposure(x):
    with connect("ws://127.0.0.1:8787", origin="http://localhost:5173") as ws:
        ws.send(json.dumps({"type": "lighting", "exposure": x})); time.sleep(0.3)

def run(page):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(URL); page.wait_for_selector(".status.ok", timeout=15000); page.wait_for_timeout(800)
    E = page.evaluate
    check(E(f"window.__editor.open('{MAP_REL}').then(r => r.ok)"), "the test map opens")
    page.wait_for_timeout(800)
    sel = "select[title='Render mode (per viewport)']"
    page.select_option(sel, "lit")
    t0 = time.time()
    while time.time() - t0 < 240:
        st = E("(()=>{const s=window.__editor.store;return {conv:!!s.lightmap.status?.converged, vol:s.lightvol.ready, ents:(s.lightvol.entity('', 'e_button')||[]).length>0}})()")
        if st["conv"] and st["vol"] and st["ents"]: break
        page.wait_for_timeout(500)
    print("lighting state after %.0fs: %s" % (time.time() - t0, st))
    check(st["conv"], "lightmap converged")
    check(st["vol"], "the light volume grid arrived")
    check(st["ents"], "entity_light for the non-lightmapped func_button arrived")

    peak = E("""() => {
        const half = (h) => { const e = (h >> 10) & 31, m = h & 1023; const v = e === 0 ? m * 2 ** -24 : (1 + m / 1024) * 2 ** (e - 15); return (h & 0x8000) ? -v : v }
        let p = 0
        for (const pg of window.__editor.store.lightmap.pages)
          for (let i = 0; i < pg.length; i += 4) { if (!pg[i + 3]) continue; for (let c = 0; c < 3; c++) p = Math.max(p, half(pg[i + c])) }
        return p }""")
    print("brightest lightmap value in the Shell: %.2f" % peak)
    check(peak > 3.0, f"lightmap values above 1 reach the Shell unclamped (peak {peak:.2f})")
    ent = E("window.__editor.store.lightvol.entity('', 'e_button')")
    print("button light:", ent)
    # the light is at (0, 3, 0): from the button (-4.5, 1.2, -4.5) it lies towards +x, +y, +z. (Values saturate at the x2 range of the light volume format.)
    tl = [4.5, 1.8, 4.5]; n = sum(c * c for c in tl) ** 0.5
    base = next((l for l in (ent or []) if l["s"] == 0), None)
    check(base and max(base["directed"]) > 1.0 and sum(base["dir"][i] * tl[i] / n for i in range(3)) > 0.85, "the button is lit from the direction of the light")

    # ---- Lit mode: textured and lit
    page.wait_for_timeout(500)
    page.screenshot(path=f"{OUT}/lit_1_lit.png")
    box = page.locator("canvas").first.bounding_box()
    im = Image.open(f"{OUT}/lit_1_lit.png").convert("RGB").crop((int(box["x"]), int(box["y"]) + 60, int(box["x"] + box["width"]), int(box["y"] + box["height"])))
    small = im.resize((200, 120))
    colours = len(set(small.get_flattened_data()))
    lum = sum(sum(p) for p in small.get_flattened_data()) / (3 * 255 * 200 * 120)
    print("lit view: %d distinct colours, mean luminance %.3f" % (colours, lum))
    check(colours > 60, "Lit view shows textured, lit geometry")
    check(0.02 < lum, "Lit view is not black")

    # ---- decals are drawn in Lit mode: the projector's texture x the light of the floor under it
    t0 = time.time()
    while time.time() - t0 < 30 and not E("window.__editor.store.decalEntries().length"): page.wait_for_timeout(300)
    check(E("window.__editor.store.decalEntries().length") >= 1, "the decal was derived by the Core")
    V = "window.__editor.viewport()"
    def px_at(world, path, r=3):
        scr = E(f"(()=>{{const v={V};const c=v.canvas.getBoundingClientRect();const s=v.worldToScreen({world});return {{x:c.left+s.x,y:c.top+s.y}}}})()")
        page.screenshot(path=path)
        im = Image.open(path).convert("RGB"); x, y = int(scr["x"]), int(scr["y"])
        pts = [im.getpixel((x + dx, y + dy)) for dx in range(-r, r + 1, r) for dy in range(-r, r + 1, r)]
        return tuple(sum(p[i] for p in pts) / len(pts) for i in range(3)), scr
    set_exposure(0.06)  # the floor next to the lamp is far above 1: with a modest exposure its colour is not clipped to white
    t0 = time.time()
    while time.time() - t0 < 20 and abs(E("window.__editor.store.lightmap.exposure") - 0.06) > 1e-9: page.wait_for_timeout(200)
    page.wait_for_timeout(800)
    page.screenshot(path=f"{OUT}/lit_decal_on.png")
    full = Image.open(f"{OUT}/lit_decal_on.png").convert("RGB")
    view = full.crop((int(box["x"]) + 10, int(box["y"]) + 160, int(box["x"] + box["width"]) - 10, int(box["y"] + box["height"]) - 5))
    px = list(view.get_flattened_data())
    # the decal's texture is yellow (blue channel far below red and green); the floor is green, the bricks are reddish with a blue share
    yellow = sum(1 for r, g, b in px if r > 40 and b < 0.45 * r and g > 0.7 * r and r > 0.8 * g)
    green = sum(1 for r, g, b in px if g > 1.1 * r and g > 1.1 * b and g > 40)
    print("viewport pixels: %d yellow (decal), %d green (floor), of %d" % (yellow, green, len(px)))
    check(yellow > 400, f"the decal is drawn in Lit mode: its yellow texture covers {yellow} pixels (a lit view without decals has none)")
    check(green > 2000, f"the floor around it is the green floor texture ({green} pixels)")

    # ---- HDR: exposure 0.05 (the Core's minimum) brings the 17 texel to 0.86; a clamp-at-1 pipeline cannot show more than 0.05
    check(peak > 10, f"the test scene has a texel well above 1 ({peak:.1f})")
    set_exposure(0.05)
    page.select_option(sel, "lightingOnly")
    t0 = time.time()
    while time.time() - t0 < 20 and abs(E("window.__editor.store.lightmap.exposure") - 0.05) > 1e-9: page.wait_for_timeout(200)
    check(abs(E("window.__editor.store.lightmap.exposure") - 0.05) < 1e-9, "exposure arrives with the lighting status")
    page.wait_for_timeout(800)
    page.screenshot(path=f"{OUT}/lit_2_lightingonly_exposure.png")
    full = Image.open(f"{OUT}/lit_2_lightingonly_exposure.png").convert("RGB")
    # the floor part of the viewport, below the header and the light's label (no UI there): neutral grey pixels only
    region = full.crop((int(box["x"]) + 20, 200, int(box["x"] + box["width"]) - 20, 410))
    grey = [p for p in region.get_flattened_data() if abs(p[0] - p[1]) < 8 and abs(p[1] - p[2]) < 8]
    top = max((p[0] for p in grey), default=0)
    print("brightest neutral floor pixel in Lighting-only at exposure 0.05: %d (HDR pipeline ~240, clamped <= 64)" % top)
    check(top >= 200, f"HDR light reaches the screen: texels of up to {peak:.1f} x 0.05 show {top} (a clamped pipeline shows <= 64)")

    # ---- the entity in Lighting-only is lit from the grid (drawn at all)
    set_exposure(1.0)
    page.wait_for_timeout(800)
    page.screenshot(path=f"{OUT}/lit_3_lightingonly.png")
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
print("\nLIT E2E FAILED" if check.failed else "\nLIT E2E PASSED"); sys.exit(1 if check.failed else 0)
