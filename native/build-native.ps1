$ErrorActionPreference='Stop'
$nativeDir=$PSScriptRoot
$dist=Join-Path (Split-Path $nativeDir) 'dist'
# Discover already-installed build tools. This script installs nothing.
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(-not(Test-Path -LiteralPath $vswhere)){throw 'An existing Visual Studio C++ build toolchain is required to build from source.'}
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vsRoot){throw 'No installed x64 C++ build toolchain was found.'}
$vcVersion=(Get-Content -LiteralPath (Join-Path $vsRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$vcRoot=Join-Path $vsRoot ('VC\Tools\MSVC\'+$vcVersion)
$compiler=Join-Path $vcRoot 'bin\Hostx64\x64\cl.exe'
$sdkRoot=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdkVersion=Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Where-Object {Test-Path (Join-Path $_.FullName 'um\Windows.h')} | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if(-not $sdkVersion){throw 'An installed Windows SDK is required.'}
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB
try {
    $env:INCLUDE="$vcRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um"
    $env:LIB="$vcRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Push-Location $nativeDir
    try {
        & $compiler /nologo /std:c++17 /EHsc /MT /O2 /W4 NativeControl.cpp /link /OUT:"$dist\NativeControl.exe" user32.lib shell32.lib advapi32.lib dbghelp.lib psapi.lib dwmapi.lib
        if($LASTEXITCODE){throw 'Native helper build failed'}
        $minhook=Join-Path $nativeDir 'vendor\minhook-1.3.4'
        & $compiler /nologo /MT /O2 /W3 /I"$minhook\include" /I"$minhook\src" /c "$minhook\src\buffer.c" "$minhook\src\hook.c" "$minhook\src\trampoline.c" "$minhook\src\hde\hde64.c"
        if($LASTEXITCODE){throw 'Bundled hook library build failed'}
        & $compiler /nologo /std:c++17 /EHsc /MT /O2 /W4 /LD /I"$minhook\include" ShellBridge.cpp buffer.obj hook.obj trampoline.obj hde64.obj /link /OUT:"$dist\ShellBridge.dll" user32.lib shell32.lib advapi32.lib psapi.lib wtsapi32.lib
        if($LASTEXITCODE){throw 'Native bridge build failed'}
    } finally {Pop-Location}
} finally {$env:INCLUDE=$oldInclude;$env:LIB=$oldLib}
