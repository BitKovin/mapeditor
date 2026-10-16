#!/bin/bash
# Starts Core + the Vite dev server, runs the browser test, stops both. Use this instead of starting servers by hand:
# in a sandbox the background servers die as soon as the shell call that started them ends.
# Needs: built core (core/build/mapeditor_core), npm install done, python3 with playwright + pillow + websockets.
# Usage (from anywhere):  shell/e2e/run_e2e.sh      -> prints "exit 0" and "E2E PASSED" on success; log in /tmp/e2e.log
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
rm -f "$ROOT/core/build/settings.json"
(cd "$ROOT/core" && exec ./build/mapeditor_core 8787 assets > /tmp/core.log 2>&1) &
CORE=$!
(cd "$ROOT/shell" && exec npx vite --port 5173 > /tmp/vite.log 2>&1) &
VITE=$!
sleep 5
(cd "$ROOT/shell" && timeout 280 python3 e2e/e2e.py > /tmp/e2e.log 2>&1)
echo "exit $?"
grep -E "FAIL|PASSED|FAILED|Traceback" /tmp/e2e.log | head -20
kill $CORE $VITE 2>/dev/null
