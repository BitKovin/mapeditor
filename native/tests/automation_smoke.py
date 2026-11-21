"""Smoke test of the editor's automation socket (08_MILESTONES.md T9.4).

Starts mapeditor_native.exe with --automation-port (or connects to one that is already running), opens maps/demo.map.json, takes two shots from
two cameras, reads stats and checks that the two pictures differ. NOTE: this opens the real editor window on the desktop (the viewports need
one), so it is run by hand, not by the automatic test runs.

    python native/tests/automation_smoke.py --exe native/build/mapeditor_native.exe --assets core/assets
    python native/tests/automation_smoke.py --port 5577          # an editor started with --automation-port 5577

Protocol: one JSON object per line each way; {"id": .., "cmd": "open", ...} -> {"id": .., "ok": true, ...}. Send {"cmd": "help"} for the list.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


class Editor:
    def __init__(self, port, timeout=180):
        deadline = time.time() + 30
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.25)
        self.file = self.sock.makefile("rwb")
        self.next_id = 0

    def call(self, cmd, **params):
        self.next_id += 1
        request = dict(params, cmd=cmd, id=self.next_id)
        self.file.write((json.dumps(request) + "\n").encode("utf-8"))
        self.file.flush()
        line = self.file.readline()
        if not line:
            raise RuntimeError("the editor closed the connection")
        reply = json.loads(line)
        assert reply.get("id") == self.next_id, reply
        return reply

    def ok(self, cmd, **params):
        reply = self.call(cmd, **params)
        if not reply.get("ok"):
            raise RuntimeError("%s failed: %s" % (cmd, reply.get("error")))
        return reply


def png_pixels(path):
    """Decoded RGBA bytes of an 8-bit RGBA / RGB PNG (stdlib only)."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, width, height, depth, ctype = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    assert depth == 8 and ctype in (2, 6), "unsupported PNG"
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * bpp
    out, prev = bytearray(), bytearray(stride)
    i = 0
    for _ in range(height):
        ft = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="mapeditor_native.exe to start")
    ap.add_argument("--assets", help="assets folder for --exe", default=None)
    ap.add_argument("--port", type=int, default=5577)
    ap.add_argument("--map", default="maps/demo.map.json")
    args = ap.parse_args()

    proc = None
    if args.exe:
        cmd = [args.exe, "--automation-port", str(args.port)]
        if args.assets:
            cmd += ["--assets", args.assets]
        proc = subprocess.Popen(cmd)
    try:
        ed = Editor(args.port)
        print("commands:", ed.ok("help")["commands"])
        print("open:", ed.ok("open", path=args.map))
        print("settings:", ed.ok("settings", key="renderMode", value="unlit")["settings"]["renderMode"])
        tmp = tempfile.mkdtemp(prefix="automation_smoke_")
        shots = []
        for i, (eye, target) in enumerate((([6, 5, 6], [0, 1, 0]), ([-6, 3, 4], [0, 1, 0]))):
            ed.ok("camera", viewport=0, eye=eye, target=target, fov=60)
            path = os.path.join(tmp, "shot%d.png" % i)
            reply = ed.ok("screenshot", viewport=0, path=path, timeoutMs=60000)
            print("screenshot:", reply)
            shots.append(png_pixels(path))
        assert shots[0][0] > 0 and shots[0][1] > 0
        assert shots[0][2] != shots[1][2], "the two cameras gave the same picture"
        stats = ed.ok("stats")
        print("stats:", json.dumps(stats))
        assert stats["document"]["open"] and stats["document"]["brushes"] > 0
        assert stats["viewports"], "no viewport"
        doc = ed.ok("get_document")["doc"]
        assert doc.get("entities"), "empty document"
        print("OK: shots in", tmp)
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
