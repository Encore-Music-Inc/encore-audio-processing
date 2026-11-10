#!/usr/bin/env bash
# Compiles the browser engine to WebAssembly (wasm/master.wasm): the ENCORE.
# mastering chain, the loudness meter and the tuner's pitch detector.
# Needs Emscripten's em++ on PATH (source emsdk_env.sh first). CI pins the
# version in .github/workflows/ci.yml; use the same one to reproduce a release.
# Usage: wasm/build.sh [output file]   (default: wasm/master.wasm)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:-$here/master.wasm}"
command -v em++ >/dev/null || { echo "em++ not found: install Emscripten and source emsdk_env.sh" >&2; exit 1; }
exports="_mc_create,_mc_destroy,_mc_left,_mc_right,_mc_latency,_mc_settings,_mc_process,_mc_learn,_mc_finish_learning,_mc_get,_mc_learned,_mc_lock_learned,_mc_override_style,_mc_user_eq,_lm_create,_lm_destroy,_lm_left,_lm_right,_lm_reset,_lm_process,_lm_integrated,_lm_record,_lm_blocks,_lm_block_length,_lm_block_energy,_tp_estimate,_tp_reading,_malloc,_free"
em++ "$here/master_api.cpp" -O3 -msimd128 -std=c++20 -fno-exceptions -fno-rtti \
    -DVS_NO_JUCE=1 -I "$root" \
    -sSTANDALONE_WASM=1 --no-entry "-sEXPORTED_FUNCTIONS=$exports" \
    -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=0 -sFILESYSTEM=0 \
    -o "$out"
echo "built $out ($(( $(stat -c %s "$out") / 1024 )) KB)"
