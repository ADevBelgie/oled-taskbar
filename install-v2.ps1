$ErrorActionPreference = 'Stop'

$lifecycleMutex = [Threading.Mutex]::new($false, 'Local\OledTaskbar2.InstallLifecycle')
$lockHeld = $false
try {
    try { $lockHeld = $lifecycleMutex.WaitOne([TimeSpan]::FromSeconds(30)) }
    catch [Threading.AbandonedMutexException] { $lockHeld = $true }
    if (-not $lockHeld) { throw 'Another OLED Taskbar install or uninstall operation is in progress.' }

    $root = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\OledTaskbar2')).TrimEnd('\')
    $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'OLED Taskbar'
    $dist = Join-Path $PSScriptRoot 'dist'
    $appKey = 'HKCU:\Software\OledTaskbar2'
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $runtimeFiles = @('OledTaskbar2.exe', 'NativeControl.exe', 'ShellBridge.dll', 'bridge.ini', 'uninstall-v2.ps1')

    $rootItem = Get-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
    if ($rootItem -and ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "The installation root is a reparse point; refusing to install: $root"
    }
    $controllers = @()
    foreach ($candidate in @(Get-Process -Name 'OledTaskbar2' -ErrorAction SilentlyContinue)) {
        try { $candidatePath = [IO.Path]::GetFullPath($candidate.Path) }
        catch { throw "Could not safely identify a running OLED Taskbar controller. Install/update was stopped: $($_.Exception.Message)" }
        if ($candidatePath.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { $controllers += $candidate }
    }
    if ($controllers.Count -ne 0) { throw 'OLED Taskbar is running from the installed directory. Disable it, exit it, and retry.' }
    foreach ($explorer in @(Get-Process -Name 'explorer','ShellHost','StartMenuExperienceHost' -ErrorAction SilentlyContinue | Where-Object SessionId -eq (Get-Process -Id $PID).SessionId)) {
        try {
            foreach ($module in $explorer.Modules) {
                if ($module.ModuleName -ieq 'ShellBridge.dll' -and
                    ([IO.Path]::GetFullPath($module.FileName)).StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw 'ShellBridge.dll is still mapped in Explorer. Sign out and back in before installing or updating; pinned detours must not be stacked.'
                }
            }
        } catch {
            if ($_.Exception.Message -like 'ShellBridge.dll is still mapped*') { throw }
            throw "Could not safely inspect Explorer modules. Install/update was stopped: $($_.Exception.Message)"
        }
    }

    $existingKey = Test-Path -LiteralPath $appKey
    $existing = if ($existingKey) { Get-ItemProperty -LiteralPath $appKey } else { $null }
    $state = if ($existing) { [string]$existing.State } else { $null }
    if ($existingKey -and [string]::IsNullOrWhiteSpace($state)) { throw 'Existing OLED Taskbar state is missing; refusing to install.' }
    if ($state -and $state -ne 'Disabled') { throw "OLED Taskbar state is '$state'. Complete Disable / restore before installing or updating." }
    $startupWasEnabled = $null -ne (Get-ItemProperty -LiteralPath $runKey -Name OledTaskbar2 -ErrorAction SilentlyContinue).OledTaskbar2

    $sources = @{}
    foreach ($name in $runtimeFiles) {
        $source = if ($name -eq 'uninstall-v2.ps1') { Join-Path $PSScriptRoot $name } else { Join-Path $dist $name }
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required payload is missing: $source" }
        if ((Get-Item -LiteralPath $source -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Payload is a reparse point: $source" }
        $sources[$name] = $source
    }
    $gpl = Join-Path $PSScriptRoot 'LICENSE-GPL-3.0.txt'
    $minHookLicense = Join-Path $PSScriptRoot 'native\vendor\minhook-1.3.4\LICENSE.txt'
    foreach ($source in @($gpl, $minHookLicense)) { if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required license is missing: $source" } }

    $hashMaterial = foreach ($name in $runtimeFiles) { "$name=$((Get-FileHash -LiteralPath $sources[$name] -Algorithm SHA256).Hash)" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($hashMaterial -join "`n"))
        $version = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').Substring(0, 16)
    } finally { $sha.Dispose() }
    $versionDir = Join-Path $root $version

    New-Item -ItemType Directory -Force -Path $root, $startMenu | Out-Null
    if (Test-Path -LiteralPath $versionDir) {
        if ((Get-Item -LiteralPath $versionDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Version directory is a reparse point: $versionDir" }
        foreach ($name in $runtimeFiles) {
            $installed = Join-Path $versionDir $name
            # The app rewrites bridge.ini in place after Windows updates replace taskbar.dll;
            # the bridge validates it against the live image, so only require it to exist.
            if (-not (Test-Path -LiteralPath $installed -PathType Leaf) -or
                ($name -ne 'bridge.ini' -and
                 (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $sources[$name] -Algorithm SHA256).Hash)) {
                throw "Immutable version directory is incomplete or modified: $versionDir"
            }
        }
    } else {
        $staging = Join-Path $root ('.staging-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $staging | Out-Null
        try {
            foreach ($name in $runtimeFiles) { Copy-Item -LiteralPath $sources[$name] -Destination (Join-Path $staging $name) }
            Copy-Item -LiteralPath $gpl -Destination (Join-Path $staging 'LICENSE-GPL-3.0.txt')
            Copy-Item -LiteralPath $minHookLicense -Destination (Join-Path $staging 'LICENSE-MinHook.txt')
            Move-Item -LiteralPath $staging -Destination $versionDir
        } finally { if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue } }
    }

    Copy-Item -LiteralPath $sources['uninstall-v2.ps1'] -Destination (Join-Path $root 'uninstall-v2.ps1') -Force
    $exe = Join-Path $versionDir 'OledTaskbar2.exe'
    $shell = New-Object -ComObject WScript.Shell
    foreach ($entry in @(@('OLED Taskbar', ''), @('Enable OLED Taskbar', '--enable'), @('Disable OLED Taskbar', '--disable'))) {
        $shortcut = $shell.CreateShortcut((Join-Path $startMenu ($entry[0] + '.lnk')))
        $shortcut.TargetPath = $exe; $shortcut.Arguments = $entry[1]; $shortcut.WorkingDirectory = $versionDir
        $shortcut.Description = 'Full Windows taskbar on the secondary display; primary display stays clear'; $shortcut.Save()
    }
    $shortcut = $shell.CreateShortcut((Join-Path $startMenu 'Uninstall OLED Taskbar.lnk'))
    $shortcut.TargetPath = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $shortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $root 'uninstall-v2.ps1') + '"'; $shortcut.Save()

    $generation = [Guid]::NewGuid().ToString('N')
    # New-Item -Force recreates an existing registry key and drops its values, which
    # would lose State/WantEnabled/originals on upgrade. Create it only when absent.
    if (-not $existingKey) { New-Item -Path $appKey -Force | Out-Null }
    if (-not $existingKey) { New-ItemProperty -Path $appKey -Name State -Value 'Disabled' -PropertyType String -Force | Out-Null }
    New-ItemProperty -Path $appKey -Name CurrentExe -Value $exe -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $appKey -Name InstallGeneration -Value $generation -PropertyType String -Force | Out-Null
    if ($startupWasEnabled) { New-ItemProperty -Path $runKey -Name OledTaskbar2 -Value ('"' + $exe + '" --background') -PropertyType String -Force | Out-Null }

    $uninstallCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $root 'uninstall-v2.ps1') + '"'
    $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\OledTaskbar2'
    New-Item -Path $uninstallKey -Force | Out-Null
    (@{DisplayName='OLED Taskbar';DisplayVersion='2.0';Publisher='Personal utility';InstallLocation=$root;DisplayIcon=$exe;UninstallString=$uninstallCommand}).GetEnumerator() |
        ForEach-Object { New-ItemProperty -Path $uninstallKey -Name $_.Key -Value $_.Value -PropertyType String -Force | Out-Null }
    Write-Output $exe
}
finally {
    if ($lockHeld) { $lifecycleMutex.ReleaseMutex() }
    $lifecycleMutex.Dispose()
}
