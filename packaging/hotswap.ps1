# Replaces the installed s411_pi.dll with the freshly built one WITHOUT going
# through the plugin manager. The imported plugin lives in the user's local
# appdata (writable without admin), so a restart of OpenCPN is all that's
# needed. CLOSE OpenCPN first (it locks the DLL while running).
#
#   powershell -ExecutionPolicy Bypass -File packaging\hotswap.ps1

$repo = Split-Path $PSScriptRoot -Parent
$src = "$repo\winbuild\s411_pi.dll"
$dst = "$env:LOCALAPPDATA\opencpn\plugins\s411_pi.dll"

if (-not (Test-Path $src)) { throw "build first: buildwin.ps1 (no $src)" }
if (-not (Test-Path $dst)) {
    Write-Host "Plugin not installed via import yet - use Options -> Plugins -> Import plugin with dist\*.tar.gz instead."
    return
}
try {
    Copy-Item $src $dst -Force -ErrorAction Stop
    Write-Host "Replaced installed DLL. Restart OpenCPN."
} catch {
    Write-Host "Could not replace the DLL - is OpenCPN still open? Close it and retry."
}
