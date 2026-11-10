#!/usr/bin/env bash
# Compiles the browser engine to WebAssembly (wasm/master.wasm): every
# wasm/api_*.cpp file (one per area: the mastering chain, the loudness meter,
# the tuner, ...) and the headers they include, without JUCE.
# Needs Emscripten's em++ on PATH (source emsdk_env.sh first). CI pins the
# version in .github/workflows/ci.yml; use the same one to reproduce a release.
# Usage: wasm/build.sh [output file]   (default: wasm/master.wasm)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:-$here/master.wasm}"
command -v em++ >/dev/null || { echo "em++ not found: install Emscripten and source emsdk_env.sh" >&2; exit 1; }
sources=("$here"/api_*.cpp)
# Every function marked EMSCRIPTEN_KEEPALIVE in those files is exported, plus malloc and free
# for the buffers the browser passes in.
names=$(sed -nE 's/.*EMSCRIPTEN_KEEPALIVE[^(]*[ *]([a-z][a-z0-9_]*) ?\(.*/_\1/p' "${sources[@]}" | sort -u)
[[ -n "$names" ]] || { echo "no EMSCRIPTEN_KEEPALIVE functions found in wasm/api_*.cpp" >&2; exit 1; }
exports="$(echo "$names" | paste -sd, -),_malloc,_free"
em++ "${sources[@]}" -O3 -msimd128 -std=c++20 -fno-exceptions -fno-rtti \
    -DVS_NO_JUCE=1 -I "$root" \
    -sSTANDALONE_WASM=1 --no-entry "-sEXPORTED_FUNCTIONS=$exports" \
    -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=0 -sFILESYSTEM=0 \
    -o "$out"
echo "built $out ($(( $(stat -c %s "$out") / 1024 )) KB, $(echo "$names" | wc -l) functions)"
