$ErrorActionPreference='Stop'
& "$PSScriptRoot\native\build-native.ps1"
& "$PSScriptRoot\assets\build-icon.ps1"
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /platform:x64 /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /win32manifest:"$PSScriptRoot\app.manifest" /win32icon:"$PSScriptRoot\assets\oled-taskbar.ico" /out:"$PSScriptRoot\dist\OledTaskbar2.exe" "$PSScriptRoot\ProgramV2.cs"
if($LASTEXITCODE){throw 'Control app build failed'}
Write-Output 'Built. Run prepare-symbols.ps1 before installing on this computer.'
