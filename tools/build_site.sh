#!/usr/bin/env bash
# Builds the static Life Duel web site (no server needed, e.g. GitHub Pages):
# the C++ battle engine compiled to WebAssembly, the web UI and the bot leagues.
#
#   tools/build_site.sh [OUT_DIR]      (default: dist)
#
# Needs Emscripten (emcmake/emcc on PATH, or an emsdk in $EMSDK or ~/emsdk)
# and python3. Serve the result with e.g. `python3 -m http.server -d dist`.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/dist}"
build="${WASM_BUILD_DIR:-$root/build-wasm}"

if ! command -v emcmake >/dev/null 2>&1; then
  for sdk in "${EMSDK:-}" "$HOME/emsdk"; do
    if [[ -n "$sdk" && -f "$sdk/emsdk_env.sh" ]]; then
      # shellcheck disable=SC1091
      source "$sdk/emsdk_env.sh" >/dev/null 2>&1
      break
    fi
  done
fi
command -v emcmake >/dev/null 2>&1 || { echo "Emscripten not found (install emsdk, see README)." >&2; exit 1; }

emcmake cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DGOL_NATIVE=OFF >/dev/null
cmake --build "$build" -j"$(nproc 2>/dev/null || echo 2)"
# The core tests under node: the engine in the browser must be the native one.
if [[ "${SKIP_WASM_TESTS:-0}" != 1 ]]; then
  ctest --test-dir "$build" --output-on-failure
fi

rm -rf "$out"
mkdir -p "$out/engine" "$out/leagues"
cp -r "$root/battle/web/." "$out/"
cp "$build/life_duel_engine.js" "$build/life_duel_engine.wasm" "$out/engine/"
# The static backend answers app.js's /api/ requests in the browser.
python3 - "$out/index.html" <<'EOF'
import sys
path = sys.argv[1]
html = open(path, encoding="utf-8").read()
tag = '<script src="app.js"></script>'
assert tag in html, "index.html must load app.js"
html = html.replace(tag, '<script src="static-backend.js"></script>\n  ' + tag, 1)
open(path, "w", encoding="utf-8").write(html)
EOF

# Bot leagues, with the ids life_battle would give them.
shopt -s nullglob
python3 - "$out/leagues" "$root"/assets/leagues/*.json <<'EOF'
import json, os, shutil, sys
dest, files = sys.argv[1], sorted(sys.argv[2:])
index = []
for path in files:
    name = os.path.basename(path)
    base = name[:-len(".json")]
    if base.endswith(".txt"):
        base = base[:-len(".txt")]
    bot_id, n = base, 2
    while any(b["id"] == bot_id for b in index):
        bot_id, n = f"{base}-{n}", n + 1
    shutil.copy(path, os.path.join(dest, name))
    index.append({"id": bot_id, "file": name})
json.dump(index, open(os.path.join(dest, "index.json"), "w"))
print(f"{len(index)} bot league(s)")
EOF
touch "$out/.nojekyll"
echo "Static site written to $out"
