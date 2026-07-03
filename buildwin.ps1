# Builds s411_pi.dll for the locally installed 32-bit OpenCPN (Windows).
#
# Requirements: Visual Studio 2022 (C++ workload), OpenCPN installed.
# The script downloads the matching wxWidgets 3.2.9 vc14x SDK on first run
# and generates the OpenCPN import library from the installed opencpn.exe.
#
#   powershell -ExecutionPolicy Bypass -File buildwin.ps1 [-Install]

param([switch]$Install)
$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$sdk = "$repo\sdk"
$ocpnDir = "C:\Program Files (x86)\OpenCPN"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if (-not (Test-Path $vc)) { throw "vcvars32.bat not found - install VS2022 with C++ workload" }
if (-not (Test-Path "$ocpnDir\opencpn.exe")) { throw "OpenCPN not found in $ocpnDir" }

# 1. wxWidgets 3.2.9 SDK (headers + Win32 vc14x import libs, ~35 MB once)
if (-not (Test-Path "$sdk\wx\lib\vc14x_dll")) {
    New-Item -ItemType Directory -Force "$sdk\wx" | Out-Null
    $base = "https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.9"
    Invoke-WebRequest "$base/wxWidgets-3.2.9-headers.7z" -OutFile "$sdk\headers.7z"
    Invoke-WebRequest "$base/wxMSW-3.2.9_vc14x_Dev.7z" -OutFile "$sdk\dev.7z"
    tar -xf "$sdk\headers.7z" -C "$sdk\wx"   # Windows bsdtar reads 7z
    tar -xf "$sdk\dev.7z" -C "$sdk\wx"
}

# 2. OpenCPN import library from the installed exe's export table
if (-not (Test-Path "$sdk\opencpn.lib")) {
    cmd /c "`"$vc`" >nul 2>&1 && dumpbin /exports `"$ocpnDir\opencpn.exe`" > `"$sdk\exports.txt`""
    $names = Get-Content "$sdk\exports.txt" |
        Where-Object { $_ -match "^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)" } |
        ForEach-Object { $Matches[1] }
    if ($names.Count -lt 100) { throw "unexpectedly few exports - wrong opencpn.exe?" }
    Set-Content "$sdk\opencpn.def" (@("LIBRARY opencpn.exe", "EXPORTS") + $names) -Encoding ascii
    cmd /c "`"$vc`" >nul 2>&1 && lib /nologo /def:`"$sdk\opencpn.def`" /machine:x86 /out:`"$sdk\opencpn.lib`""
}

# 3. compile + link (Win32, /MD, wx DLL build - matches OpenCPN's vc14x DLLs)
New-Item -ItemType Directory -Force "$repo\winbuild" | Out-Null
cmd /c "`"$vc`" >nul 2>&1 && cd /d `"$repo`" && cl /nologo /c /std:c++14 /EHsc /MD /O2 /DWIN32 /D_WINDOWS /D__WXMSW__ /DWXUSINGDLL /D_UNICODE /DUNICODE /Isdk\wx\include /Isdk\wx\lib\vc14x_dll\mswu /Iapi /Isrc /Fowinbuild\ src\s411_pi.cpp src\S411Data.cpp src\S411Tess.cpp && link /nologo /DLL /MACHINE:X86 /OUT:winbuild\s411_pi.dll winbuild\s411_pi.obj winbuild\S411Data.obj winbuild\S411Tess.obj /LIBPATH:sdk\wx\lib\vc14x_dll sdk\opencpn.lib wxmsw32u_core.lib wxbase32u.lib wxbase32u_xml.lib wxmsw32u_gl.lib wxmsw32u_aui.lib wxmsw32u_adv.lib glu32.lib opengl32.lib comctl32.lib rpcrt4.lib"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "OK: winbuild\s411_pi.dll"

# 4. optional: install (needs elevation)
if ($Install) {
    $dst = "$ocpnDir\plugins\s411_pi.dll"
    try { Copy-Item "$repo\winbuild\s411_pi.dll" $dst -Force }
    catch {
        Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile","-Command",
            "Copy-Item '$repo\winbuild\s411_pi.dll' '$dst' -Force"
    }
    Write-Host "installed: $dst"
}
