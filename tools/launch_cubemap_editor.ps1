param(
  [Parameter(Position=0)][string]$Map = 'gm_construct',
  [string]$Settings = '',
  [string]$ProbeReport = '',
  [string]$IconDirectory = '',
  [string[]]$ContentRoots = @(),
  [switch]$Browser,
  [int]$Port = 8765
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
if (!$Settings) { $Settings = Join-Path $repository '_work/testing-install.json' }
$install = Get-Content -LiteralPath $Settings -Raw | ConvertFrom-Json
$bsp = if (Test-Path -LiteralPath $Map -PathType Leaf) { (Resolve-Path -LiteralPath $Map).Path }
       else { Join-Path $install.source "garrysmod/maps/$Map.bsp" }
if (!(Test-Path -LiteralPath $bsp -PathType Leaf)) { throw "BSP not found: $bsp" }
$name = [IO.Path]::GetFileNameWithoutExtension($bsp)
$document = Join-Path $repository "_work/cubemaps/$name.json"
$runtime = Join-Path $install.target "garrysmod/data/openremix/cubemaps/$name.cubemaps.bin"
$editorArguments = @('-m', 'probe_editor', $bsp, '--output', $document, '--runtime-output', $runtime)
if ($Browser) { $editorArguments += @('--browser', '--port', $Port) }
else {
  $dependencies = Join-Path $repository '_work/deps/probe-editor'
  $requirements = Join-Path $PSScriptRoot 'probe_editor/requirements-desktop.txt'
  $stamp = Join-Path $dependencies 'requirements.sha256'
  $expected = (Get-FileHash -LiteralPath $requirements -Algorithm SHA256).Hash
  if (!(Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $expected) {
    & python -m pip install --target $dependencies --upgrade -r $requirements
    if ($LASTEXITCODE -ne 0) { throw 'Could not install local desktop editor dependencies' }
    Set-Content -LiteralPath $stamp -Value $expected
  }
  $editorArguments += @('--game', $install.source)
  foreach ($root in $ContentRoots) { $editorArguments += @('--content-root', $root) }
  if (!$IconDirectory) {
    $candidate = Join-Path (Split-Path $install.source -Parent) 'Half-Life Alyx/game/core/tools/images/hammer'
    if (Test-Path -LiteralPath $candidate -PathType Container) { $IconDirectory = $candidate }
  }
  if ($IconDirectory) { $editorArguments += @('--icon-directory', $IconDirectory) }
}
if ($ProbeReport) { $editorArguments += @('--probe-report', (Resolve-Path -LiteralPath $ProbeReport).Path) }
Push-Location $PSScriptRoot
try { & python @editorArguments; exit $LASTEXITCODE }
finally { Pop-Location }
