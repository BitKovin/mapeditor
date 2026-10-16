"""End-to-end smoke test against a *running* mapeditor_core (see README in DESIGN.md).

    ./build/mapeditor_core 8787 assets &   # from core/
    python3 smoke_test.py

Exercises the real WebSocket server with two concurrent sessions: initial sync,
ack-vs-broadcast on document_update, rejection, unknown-field preservation,
open/save, and the Origin check.
"""
import asyncio
import copy
import json
import sys

import websockets

URI = "ws://127.0.0.1:8787"


async def recv(ws, label, timeout=3):
    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout))
    print(f"  [{label}] <- {msg['type']}" + (f" rev={msg['rev']}" if "rev" in msg else ""))
    return msg


async def expect_silence(ws, label, seconds=0.4):
    try:
        msg = await asyncio.wait_for(ws.recv(), timeout=seconds)
    except asyncio.TimeoutError:
        return
    raise AssertionError(f"[{label}] expected nothing, got {json.loads(msg)['type']}")


async def main():
    async with websockets.connect(URI) as a, websockets.connect(URI) as b:
        print("1. both sessions get document_sync + prefab_library + profile on connect")
        a_doc, a_lib, a_prof = await recv(a, "A"), await recv(a, "A"), await recv(a, "A")
        b_doc, b_lib, b_prof = await recv(b, "B"), await recv(b, "B"), await recv(b, "B")
        assert a_doc["type"] == "document_sync" and a_lib["type"] == "prefab_library" and a_prof["type"] == "profile"
        assert a_prof["profile"]["textureRoot"] == "textures"
        assert b_doc["doc"] == a_doc["doc"]
        libs = set(a_lib["prefabs"])
        assert libs == {"prefabs/door_frame.map.json", "prefabs/hinge.map.json"}, libs
        assert a_doc["doc"]["entities"][0]["id"] == "e_worldspawn"
        rev0 = a_doc["rev"]

        print("2. accepted update: sender gets an ack (no echo), the other session gets the document")
        doc = copy.deepcopy(a_doc["doc"])
        doc["entities"][2]["origin"] = [-4.5, 2.0, -4.5]           # move the button
        doc["editor"] = {"visgroups": [{"name": "props", "hidden": False}]}  # unknown to Core
        await a.send(json.dumps({"type": "document_update", "doc": doc}))
        ack = await recv(a, "A")
        assert ack == {"type": "update_ack", "rev": rev0 + 1}, ack
        sync = await recv(b, "B")
        assert sync["type"] == "document_sync" and sync["doc"] == doc, "B must see exactly what A sent"
        assert sync["doc"]["editor"] == doc["editor"], "unknown fields must survive Core"
        await expect_silence(a, "A")   # no echo, no redundant library push
        await expect_silence(b, "B")

        print("3. rejected update: only the sender hears, nothing changes")
        bad = copy.deepcopy(doc)
        bad["prefabInstances"].append({"id": "pi_bad", "file": "prefabs/nope.map.json"})
        await a.send(json.dumps({"type": "document_update", "doc": bad}))
        err = await recv(a, "A")
        assert err["type"] == "error" and "not found" in err["message"], err
        await expect_silence(b, "B")

        print("4. a non-convex worldspawn brush is rejected (vis-portal rule); non-convex func_detail is not")
        # L-shaped prism: y in [0,1], profile in XZ.
        n = 6
        prof = [(0, 0), (2, 0), (2, 1), (1, 1), (1, 2), (0, 2)]
        verts = [[x, 0, z] for x, z in prof] + [[x, 1, z] for x, z in prof]
        faces = [{"id": 0, "loop": list(reversed(range(n)))}, {"id": 1, "loop": [n + i for i in range(n)]}]
        for i in range(n):
            j = (i + 1) % n
            faces.append({"id": 2 + i, "loop": [i, j, n + j, n + i]})
        # Fix winding so faces point outward (signed volume via divergence theorem).
        def vol(fs):
            v = 0.0
            for f in fs:
                a0 = verts[f["loop"][0]]
                for k in range(1, len(f["loop"]) - 1):
                    b0, c0 = verts[f["loop"][k]], verts[f["loop"][k + 1]]
                    cx = (b0[1] * c0[2] - b0[2] * c0[1], b0[2] * c0[0] - b0[0] * c0[2], b0[0] * c0[1] - b0[1] * c0[0])
                    v += (a0[0] * cx[0] + a0[1] * cx[1] + a0[2] * cx[2]) / 6.0
            return v
        if vol(faces) < 0:
            for f in faces:
                f["loop"].reverse()
        lbrush = {"id": "b_L", "op": "add", "vertices": verts, "faces": faces}
        concave_world = copy.deepcopy(doc)
        concave_world["entities"][0]["brushes"].append(lbrush)
        await a.send(json.dumps({"type": "document_update", "doc": concave_world}))
        err = await recv(a, "A")
        assert err["type"] == "error" and "must be convex" in err["message"], err
        concave_detail = copy.deepcopy(doc)
        concave_detail["entities"][1]["brushes"].append(lbrush)
        await a.send(json.dumps({"type": "document_update", "doc": concave_detail}))
        assert (await recv(a, "A"))["type"] == "update_ack"
        assert (await recv(b, "B"))["type"] == "document_sync"
        doc = concave_detail

        print("5. save + open round trip through disk (and sandbox refusal)")
        await a.send(json.dumps({"type": "save_document", "requestId": 1, "path": "maps/smoke_out.map.json"}))
        res = await recv(a, "A")
        assert res["ok"] and res["path"] == "maps/smoke_out.map.json", res
        assert (await recv(b, "B"))["type"] == "document_sync"  # save-as changed the path
        await a.send(json.dumps({"type": "save_document", "requestId": 2, "path": "../escape.json"}))
        res = await recv(a, "A")
        assert res["ok"] is False and "escapes" in res["error"], res
        await a.send(json.dumps({"type": "open_document", "requestId": 3, "path": "maps/demo.map.json"}))
        for who in (a, b):  # opening replaces the document for every window
            assert (await recv(who, "*"))["type"] == "document_sync"
            assert (await recv(who, "*"))["type"] == "prefab_library"
        assert (await recv(a, "A"))["ok"] is True
        await a.send(json.dumps({"type": "open_document", "requestId": 4, "path": "maps/smoke_out.map.json"}))
        for who in (a, b):
            sync = await recv(who, "*")
            await recv(who, "*")
        assert sync["doc"] == doc, "what we saved is what we get back"
        await recv(a, "A")

    print("6. connections from foreign origins are refused")
    try:
        async with websockets.connect(URI, origin="http://evil.example") as _:
            raise AssertionError("evil origin was accepted")
    except websockets.exceptions.InvalidStatus as e:
        assert e.response.status_code == 403, e
        print("  evil origin -> HTTP 403")
    async with websockets.connect(URI, origin="http://localhost:5173") as ok:
        assert (await recv(ok, "local-origin"))["type"] == "document_sync"

    print("\nALL SMOKE TESTS PASSED")


try:
    asyncio.run(main())
except AssertionError as e:
    print("FAILED:", e)
    sys.exit(1)
