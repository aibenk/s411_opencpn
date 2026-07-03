# Cleanly removes the s411_pi plugin from a Windows OpenCPN installation.
#
# Normally you uninstall from within OpenCPN:
#   Options -> Plugins -> S-411 Ice -> Uninstall
# Use this script only to force-remove leftovers (e.g. a DLL locked while
# OpenCPN was running). CLOSE OpenCPN before running it.
#
#   powershell -ExecutionPolicy Bypass -File packaging\uninstall.ps1

$ErrorActionPreference = "SilentlyContinue"
$removed = @()

# 1. the plugin DLL (imported plugins land in the user's local appdata)
$targets = @(
  "$env:LOCALAPPDATA\opencpn\plugins\s411_pi.dll",
  "C:\Program Files (x86)\OpenCPN\plugins\s411_pi.dll",   # legacy manual copy
  "C:\Program Files\OpenCPN\plugins\s411_pi.dll"
)

# 2. OpenCPN's install bookkeeping (so the manager forgets the plugin)
$installData = "$env:PROGRAMDATA\opencpn\plugins\install_data"
$targets += @(
  "$installData\s411_pi.dirs",
  "$installData\s411_pi.files",
  "$installData\s411_pi.version",
  "$installData\imports\s411_pi.xml"
)

foreach ($t in $targets) {
  if (Test-Path $t) {
    try {
      Remove-Item $t -Force -ErrorAction Stop
      $removed += $t
    } catch {
      # needs elevation - retry the whole set once as admin
      Start-Process powershell -Verb RunAs -Wait -ArgumentList `
        "-NoProfile","-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`""
      Write-Host "Re-ran as administrator; check the result above."
      return
    }
  }
}

if ($removed.Count) {
  Write-Host "Removed:"
  $removed | ForEach-Object { Write-Host "  $_" }
  Write-Host "`nRestart OpenCPN - S-411 Ice is gone."
} else {
  Write-Host "Nothing to remove (plugin not installed, or already gone)."
}
