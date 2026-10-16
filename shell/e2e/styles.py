"""End-to-end for light styles against the real Core and a real WebGL2 context (core/light_styles.h; the engine model from the reference):
    shell/e2e/run_lit.sh styles
  * a light with style 9 (slow strobe "aaaaaaaazzzzzzzz": 0.8 s dark, 0.8 s at 25/12 of normal) is its OWN lightmap layer: the Shell receives the layer, the
    pattern table and the style list;
  * the lit view really animates with the pattern: sampled over time its brightness alternates between a dark and a bright level, in Lit and Lighting-only;
  * a brush entity that is not lightmapped (func_button) is lit from the styled layer's probe and strobes too;
  * the base layer is independent: the lamp of style 0 keeps its light while the styled lamp is dark."""
import json, os, sys, time
from PIL import Image
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
URL = "http://localhost:5173/?debug"
MAP_REL = "maps/styles_e2e.map.json"
MAP = os.path.join(HERE, "..", "..", "core", "assets", MAP_REL)

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

def make_map():
    d = json.load(open(os.path.join(os.path.dirname(MAP), "demo.map.json")))
    d["entities"].append({"id": "e_base", "classname": "light_point", "origin": [0, 3, 0], "angles": [0, 0, 0], "brushes": [], "keyvalues": {"brightness": "6", "radius": "40"}})
    d["entities"].append({"id": "e_strobe", "classname": "light_point", "origin": [-3, 2.5, 3], "angles": [0, 0, 0], "brushes": [], "keyvalues": {"brightness": "60", "radius": "40", "style": "9"}})
    json.dump(d, open(MAP, "w"))

def mean_lum(page, box, path):
    page.screenshot(path=path)
    im = Image.open(path).convert("RGB").crop((int(box["x"]), int(box["y"]) + 160, int(box["x"] + box["width"]), int(box["y"] + box["height"])))
    px = list(im.resize((120, 60)).get_flattened_data())
    return sum(sum(p) for p in px) / (3 * 255 * len(px))

def sample(page, box, seconds, tag):
    t0 = time.time(); xs = []
    while time.time() - t0 < seconds:
        xs.append((time.time() - t0, mean_lum(page, box, f"{OUT}/styles_{tag}.png")))
    return xs

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
    t0 = time.time(); st = {}
    while time.time() - t0 < 300:
        st = E("(()=>{const s=window.__editor.store;const l=s.lightmap;return {conv:!!l.status?.converged, styles:l.styles, st:l.status?.styles, pat:l.patterns[9]||'', vol:s.lightvol.ready, ent:(s.lightvol.entity('', 'e_button')||[]).map(x=>x.s)}})()")
        if st["conv"] and st["vol"] and st["ent"]: break
        page.wait_for_timeout(500)
    print("state after %.0fs: %s" % (time.time() - t0, st))
    check(st["conv"], "all layers converged")
    check(st["styles"] == [0, 9] and st["st"] == [0, 9], "the Shell has the base layer and the style 9 layer")
    check(st["pat"] == "aaaaaaaazzzzzzzz", "the pattern table comes from the Core")
    check(st["ent"] == [0, 9], "the button has one probe per style layer")

    box = page.locator("canvas").first.bounding_box()
    V = "window.__editor.viewport()"
    # ---- real time: with a style that animates, the viewport keeps drawing by itself and the time it passes to the renderer advances
    E(f"(()=>{{const v={V}; const r=v.renderer; window.__frames=[]; const d=r.draw.bind(r); r.draw=(f)=>{{window.__frames.push(f.time); return d(f)}}}})()")
    page.wait_for_timeout(6000)
    fr = E("window.__frames")
    print("real time playback: %d frames in 6 s, clock %.2f .. %.2f" % (len(fr), fr[0] if fr else 0, fr[-1] if fr else 0))
    check(len(fr) >= 3 and all(b > a for a, b in zip(fr, fr[1:])), f"the viewport redraws by itself while a style animates ({len(fr)} frames, time advancing)")

    # ---- frozen at the pattern's dark half (0..0.8 s) and bright half (0.8..1.6 s): deterministic frames
    def frame(t, tag):
        E(f"{V}.setStyleTime({t})"); page.wait_for_timeout(1500)
        return mean_lum(page, box, f"{OUT}/styles_{tag}.png")
    dark, bright = frame(0.30, "lit_dark"), frame(1.10, "lit_bright")
    dark2, bright2 = frame(1.70, "lit_dark2"), frame(0.95, "lit_bright2")  # 1.7 s = 1.6 + 0.1: dark again after the wrap
    print("Lit: dark %.3f / %.3f, bright %.3f / %.3f" % (dark, dark2, bright, bright2))
    check(bright > 1.4 * dark and bright - dark > 0.02, f"Lit view follows the pattern: dark half {dark:.3f}, bright half {bright:.3f}")
    check(abs(dark - dark2) < 0.003 and abs(bright - bright2) < 0.003, "the pattern repeats (period 1.6 s)")
    check(dark > 0.01, f"the base layer's lamp keeps lighting while the strobe is dark ({dark:.3f})")

    page.select_option(sel, "lightingOnly")
    page.wait_for_timeout(500)
    d3, b3 = frame(0.30, "lo_dark"), frame(1.10, "lo_bright")
    print("Lighting-only: dark %.3f, bright %.3f" % (d3, b3))
    check(b3 > 1.4 * d3, f"Lighting-only follows the pattern ({d3:.3f} -> {b3:.3f})")

    # the func_button is lit from the styled layer's probe: compare the button's own pixels dark vs bright
    page.select_option(sel, "lit")
    pos = E("(()=>{const e=window.__editor.store.state.doc.entities.find(e=>e.id==='e_button');const b=e.brushes[0];const v=b.vertices;let m=[0,0,0];for(const p of v){m[0]+=p[0];m[1]+=p[1];m[2]+=p[2]}return m.map(x=>x/v.length)})()")
    scr = E(f"(()=>{{const v={V};const c=v.canvas.getBoundingClientRect();const s=v.worldToScreen({pos});return {{x:c.left+s.x,y:c.top+s.y}}}})()")
    print("button at", pos, "on screen", scr)
    if scr:
        def around(path):
            im = Image.open(path).convert("RGB"); x, y = int(scr["x"]), int(scr["y"])
            px = [im.getpixel((x + dx, y + dy)) for dx in range(-6, 7, 3) for dy in range(-6, 7, 3)]
            return sum(sum(p) for p in px) / (3 * 255 * len(px))
        frame(0.30, "button_dark"); bd = around(f"{OUT}/styles_button_dark.png")
        frame(1.10, "button_bright"); bb = around(f"{OUT}/styles_button_bright.png")
        print("button pixels: dark %.3f, bright %.3f" % (bd, bb))
        check(bb > 1.2 * bd or bb - bd > 0.03, f"the func_button (lit from the light volume) strobes with the style ({bd:.3f} -> {bb:.3f})")
    else:
        check(False, "could not project the button onto the screen")
    check(not errors, f"no console errors ({errors[:3]})")

with sync_playwright() as p:
    b = p.chromium.launch(args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
    pg = b.new_page(viewport={"width": 1600, "height": 900})
    make_map()
    try: run(pg)
    finally:
        b.close()
        try: os.remove(MAP)
        except OSError: pass
print("\nSTYLES E2E FAILED" if check.failed else "\nSTYLES E2E PASSED"); sys.exit(1 if check.failed else 0)
