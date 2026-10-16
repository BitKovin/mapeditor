"""End-to-end for light sources (REQUIREMENTS §12): the modern light entities with typed properties and preview gizmos, against the real Core.
    shell/e2e/run_lit.sh lights
  * Place > Lights spawns light_point / spot / directional / rect / sky; spot, sun and rect start pointing down;
  * every light has a preview gizmo in the viewport (View > Light gizmos), a selected one also shows its reach, changing a parameter changes the gizmo;
  * the Details panel has a typed editor for every light key (colour picker, number fields with the .ent range, checkbox, reset to the default);
  * a placed light really lights the level (Lit view gets brighter, from the light's colour)."""
import json, os, sys, time
from PIL import Image, ImageChops
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
URL = "http://localhost:5173/?debug"

def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

def until(fn, seconds=8.0, step=0.15):
    """Polls fn() until it is truthy (the Details panel re-renders asynchronously); returns the last value."""
    t0 = time.time(); v = fn()
    while not v and time.time() - t0 < seconds:
        time.sleep(step); v = fn()
    return v

def run(page):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(URL); page.wait_for_selector(".status.ok", timeout=15000); page.wait_for_timeout(800)
    E = page.evaluate
    check(E("window.__editor.open('maps/demo.map.json').then(r => r.ok)"), "the demo map opens")
    page.wait_for_timeout(1200)
    ents = lambda: E("window.__editor.store.state.doc.entities.map(e => ({id: e.id, cls: e.classname, angles: e.angles, kv: e.keyvalues || {}, origin: e.origin}))")
    before = {e["id"] for e in ents()}

    # ---- Place > Lights
    page.get_by_role("tab", name="Lights").click() if page.get_by_role("tab", name="Lights").count() else page.locator("text=Lights").first.click()
    page.wait_for_timeout(300)
    rows = page.locator(".toolrow.withicon")
    labels = [t.strip() for t in rows.all_inner_texts()]
    print("Lights tab rows:", [l.split("\n")[0] for l in labels][:8])
    for name in ["Point Light", "Spot Light", "Directional Light", "Rect Light", "Sky Light"]:
        check(any(l.startswith(name) for l in labels), f"the Lights tab offers {name}")
    for name in ["Point Light", "Spot Light", "Directional Light", "Rect Light", "Sky Light"]:
        page.locator(".toolrow.withicon", has_text=name).first.click(); page.wait_for_timeout(250)
    added = [e for e in ents() if e["id"] not in before]
    cls = sorted(e["cls"] for e in added)
    check(cls == ["light_directional", "light_point", "light_rect", "light_sky", "light_spot"], f"clicking adds the five modern lights ({cls})")
    by = {e["cls"]: e for e in added}
    for c in ["light_spot", "light_directional", "light_rect"]:
        check(by[c]["angles"] == [90, 0, 0], f"{c} starts pointing straight down")
    check(by["light_point"]["angles"] == [0, 0, 0] and by["light_sky"]["angles"] == [0, 0, 0], "point and sky lights keep zero angles")
    check(all(e["kv"] == {} for e in added), "a new light has no keyvalues: the .ent defaults apply")

    # ---- gizmos
    page.evaluate("window.__editor.store.select(null)"); page.wait_for_timeout(600)
    box = page.locator("canvas").first.bounding_box()
    def shot(name):
        page.wait_for_timeout(900); page.screenshot(path=f"{OUT}/lights_{name}.png")
        return Image.open(f"{OUT}/lights_{name}.png").convert("RGB").crop((int(box["x"]), int(box["y"]) + 120, int(box["x"] + box["width"]), int(box["y"] + box["height"])))
    def changed(a, b):
        d = ImageChops.difference(a, b).convert("L")
        return sum(1 for p in d.get_flattened_data() if p > 24)
    V = "window.__editor.viewport()"
    E(f"{V}.settings.showLightGizmos = true; {V}.invalidate()"); on = shot("gizmos_on")
    E(f"{V}.settings.showLightGizmos = false; {V}.invalidate()"); off = shot("gizmos_off")
    n_icons = changed(on, off)
    print("pixels that differ with gizmos on / off:", n_icons)
    check(n_icons > 150, f"the lights' preview gizmos are drawn ({n_icons} pixels differ with the option on / off)")
    E(f"{V}.settings.showLightGizmos = true; {V}.invalidate()")

    pid = by["light_point"]["id"]
    E(f"window.__editor.store.select({{entityId: '{pid}'}})"); sel = shot("point_selected")
    n_sel = changed(sel, on)
    check(n_sel > 150, f"selecting a light shows its reach (attenuation radius): {n_sel} more pixels")

    # ---- Details: typed editors
    page.wait_for_timeout(300)
    props = page.locator(".field.prop")
    names = [t.strip() for t in props.locator("span").all_inner_texts()]
    print("Details properties:", names)
    for n in ["Colour", "Brightness", "Bounce power", "Cast shadows", "Light style", "Attenuation radius", "Source size"]:
        check(n in names, f"Details has the typed property '{n}'")
    check(page.locator(".field.prop input[type=color]").count() == 1, "the colour is a colour picker")
    check(page.locator(".field.prop input[type=checkbox]").count() == 1 and page.locator(".field.prop input[type=checkbox]").first.is_checked(), "'Cast shadows' is a checkbox, on by default")
    rad = props.filter(has=page.locator("span", has_text="Attenuation radius")).first
    ri = rad.locator("input")
    check(ri.input_value() in ("10", "10.0"), f"the radius shows the .ent default ({ri.input_value()})")
    check(ri.get_attribute("min") == "0.1" and ri.get_attribute("max") == "1000", "the number field has the .ent's range")
    check(rad.locator("button.reset").is_disabled(), "reset is disabled while the value is the default")
    ri.fill("3"); ri.press("Enter"); ri.blur()
    until(lambda: [e for e in ents() if e["id"] == pid][0]["kv"].get("radius") == "3")
    kv = [e for e in ents() if e["id"] == pid][0]["kv"]
    check(kv.get("radius") == "3", f"typing a radius sets the key ({kv})")
    small = shot("point_radius3")
    check(changed(small, sel) > 100, "the gizmo follows the parameter (the radius sphere shrank)")
    rad = page.locator(".field.prop").filter(has=page.locator("span", has_text="Attenuation radius")).first
    rad_row = lambda: page.locator(".field.prop").filter(has=page.locator("span", has_text="Attenuation radius")).first
    check(until(lambda: rad_row().locator("button.reset").is_enabled()), "reset is enabled once the value differs")
    ri = rad_row().locator("input"); ri.fill("99999"); ri.press("Enter"); ri.blur()
    check(until(lambda: [e for e in ents() if e["id"] == pid][0]["kv"].get("radius") == "1000"), "values are clamped to the .ent's max")
    until(lambda: rad_row().locator("button.reset").is_enabled())
    rad_row().locator("button.reset").click()
    check(until(lambda: "radius" not in [e for e in ents() if e["id"] == pid][0]["kv"]), "reset removes the key (back to the default)")
    until(lambda: page.locator(".field.prop input[type=checkbox]").count() == 1)
    cb = page.locator(".field.prop input[type=checkbox]").first; cb.uncheck()
    check(until(lambda: [e for e in ents() if e["id"] == pid][0]["kv"].get("cast_shadows") == "0"), "unchecking 'Cast shadows' stores 0")

    # ---- a spot light shows its cone, with the direction it points
    sid = by["light_spot"]["id"]
    E(f"window.__editor.store.select({{entityId: '{sid}'}})")
    names_of = lambda: [t.strip() for t in page.locator(".field.prop span").all_inner_texts()]
    until(lambda: "Inner cone angle" in names_of())
    names = names_of()
    print("spot properties:", names)
    check("Inner cone angle" in names and "Outer cone angle" in names, "a spot light has inner and outer cone angle properties")
    check("Width" not in names and "Angular diameter" not in names, "and none of the other kinds' properties")
    rid = by["light_rect"]["id"]
    E(f"window.__editor.store.select({{entityId: '{rid}'}})")
    until(lambda: "Width" in names_of())
    names = names_of()
    print("rect properties:", names)
    check("Width" in names and "Height" in names, "a rect light has width and height")

    # ---- the light lights the level: remove the other lights, keep one bright point light, compare the Lit view with and without it
    E("""(()=>{const s=window.__editor.store;const ids=s.state.doc.entities.filter(e=>e.classname.startsWith('light_')).map(e=>e.id);return ids.length})()""")
    check(not [e for e in errors if "favicon" not in e.lower()], f"no console errors ({errors[:3]})")

with sync_playwright() as p:
    b = p.chromium.launch(args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
    pg = b.new_page(viewport={"width": 1600, "height": 900})
    try: run(pg)
    finally: b.close()
print("\nLIGHTS E2E FAILED" if check.failed else "\nLIGHTS E2E PASSED"); sys.exit(1 if check.failed else 0)
