$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location -LiteralPath $root
$env:REMIX_CONFIG_DIR = Join-Path $root 'runtime-config'
$env:REMIX_BAKE_CACHE_DIR = Join-Path $root 'bake-cache'
$env:OPENREMIX_PROXY_CONTENT_ROOT = $root
$env:OPENREMIX_PROXY_BACKEND = 'forwardplus'
$env:OPENREMIX_PROXY_MODS = Join-Path $root 'rtx-remix/mods'
# Use the packaged dependencies even when launched from a developer shell.
$cleanPath = $env:PATH -split ';' | Where-Object {
  $_ -and $_ -notmatch 'oidn-|usd-windows|forwardplus-validation|subprojects[\\/]SDL3'
}
$env:PATH = (@((Join-Path $root 'bin/win64/openremix'),
  (Join-Path $root 'bin/win64'), $root) + $cleanPath) -join ';'
# Diagnostics are opt-in. A developer shell must not force expensive captures.
Get-ChildItem Env: | Where-Object {
  $_.Name -like 'REMIX_DEBUG_*' -or $_.Name -in @(
    'REMIX_VULKAN_VALIDATION', 'VK_LAYER_VALIDATE_SYNC', 'VK_LAYER_SETTINGS_PATH')
} | ForEach-Object { Remove-Item -LiteralPath ("Env:" + $_.Name) }
& (Join-Path $root 'gmod_win64.exe') -game garrysmod -insecure -novid -condebug @args
