#!/usr/bin/env python3
"""Regenerates the checked-in SPIR-V headers (core/src/prebuilt/core_shaders/*.spv.h) from core/shaders/manifest.txt.

Normal Windows / Linux builds do NOT need this: CMake compiles the shaders itself when glslangValidator is found.
The checked-in headers are the fallback for machines without glslang; CMake refuses to use them when a shader source
changed after they were generated (the source hash below must match). Run after editing a shader:
    python3 tools/compile_shaders.py          (needs glslangValidator on PATH)
"""
import hashlib, pathlib, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parent.parent
out_dir = root / "src" / "prebuilt" / "core_shaders"
out_dir.mkdir(parents=True, exist_ok=True)
for line in (root / "shaders" / "manifest.txt").read_text().splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    name, source, env, defines = (line.split("|") + [""])[:4]
    src = root / "shaders" / source
    # hash = normalised source (no CR) + "\n" + manifest line: identical to the CMake computation
    text = src.read_bytes().replace(b"\r", b"").decode("utf-8")
    digest = hashlib.sha256((text + "\n" + line).encode("utf-8")).hexdigest()
    with tempfile.TemporaryDirectory() as tmp:
        spv = pathlib.Path(tmp) / (name + ".spv")
        cmd = ["glslangValidator", "-V", "--target-env", env]
        cmd += ["-D" + d for d in defines.split(",") if d]
        cmd += [str(src), "-o", str(spv)]
        subprocess.check_call(cmd)
        try:
            subprocess.check_call(["spirv-val", "--target-env", env, str(spv)])
        except FileNotFoundError:
            print("spirv-val not found; skipping validation", file=sys.stderr)
        subprocess.check_call(["cmake", "-DSPV=" + str(spv), "-DOUT=" + str(out_dir / (name + ".spv.h")), "-DNAME=" + name,
                               "-DHASH=" + digest, "-P", str(root / "tools" / "spv_to_header.cmake")])
    print("wrote", out_dir / (name + ".spv.h"))
