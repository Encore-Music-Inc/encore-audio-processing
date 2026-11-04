# Compiles the Encore mastering chain to WebAssembly (master.wasm).
# Needs Emscripten (installed at E:\tools\emsdk).
$ErrorActionPreference = "Stop"
$emcc = "E:\tools\emsdk\upstream\emscripten\em++.exe"
if (-not (Test-Path $emcc)) { $emcc = "E:\tools\emsdk\upstream\emscripten\em++.bat" }
$env:EMSDK = "E:\tools\emsdk"
$env:EM_CONFIG = "E:\tools\emsdk\.emscripten"
$env:PATH = "E:\tools\emsdk;E:\tools\emsdk\upstream\emscripten;" + (Get-ChildItem "E:\tools\emsdk\node" -Directory | Select-Object -First 1 | ForEach-Object { "$($_.FullName)\bin;" }) + $env:PATH
$exports = "_mc_create,_mc_destroy,_mc_left,_mc_right,_mc_latency,_mc_settings,_mc_process,_mc_learn,_mc_finish_learning,_mc_get,_mc_learned,_mc_lock_learned,_mc_override_style,_mc_user_eq,_malloc,_free"
& $emcc "$PSScriptRoot\master_api.cpp" -O3 -msimd128 -std=c++20 -fno-exceptions -fno-rtti `
    -DVS_NO_JUCE=1 -I "$PSScriptRoot\..\..\encore\shared" `
    -sSTANDALONE_WASM=1 --no-entry "-sEXPORTED_FUNCTIONS=$exports" `
    -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=0 -sFILESYSTEM=0 `
    -o "$PSScriptRoot\master.wasm"
if ($LASTEXITCODE -ne 0) { throw "em++ failed" }
Write-Host ("built master.wasm ({0:N0} KB)" -f ((Get-Item "$PSScriptRoot\master.wasm").Length / 1KB))
