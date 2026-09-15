param([switch]$CleanupOnly, [string]$Generation)
$ErrorActionPreference = 'Stop'

$lifecycleMutex = [Threading.Mutex]::new($false, 'Local\OledTaskbar2.InstallLifecycle')
$lockHeld = $false
try {
    try { $lockHeld = $lifecycleMutex.WaitOne([TimeSpan]::FromSeconds(30)) }
    catch [Threading.AbandonedMutexException] { $lockHeld = $true }
    if (-not $lockHeld) { throw 'Another OLED Taskbar install or uninstall operation is in progress.' }

    $root = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\OledTaskbar2')).TrimEnd('\')
    $scriptPath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
    if (-not $scriptPath.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'This uninstaller must run from the installed OLED Taskbar directory.' }
    $rootItem = Get-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
    if (-not $rootItem) { return }
    if ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "The installation root is a reparse point; refusing cleanup: $root" }

    $appKey = 'HKCU:\Software\OledTaskbar2'
    $properties = Get-ItemProperty -LiteralPath $appKey -ErrorAction SilentlyContinue
    $currentGeneration = if ($properties) { [string]$properties.InstallGeneration } else { $null }
    if ($CleanupOnly) {
        if ([string]::IsNullOrWhiteSpace($Generation) -or -not [string]::Equals($Generation, $currentGeneration, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Output 'Skipping stale OLED Taskbar cleanup because a newer installation generation exists.'
            return
        }
    } else {
        if ([string]::IsNullOrWhiteSpace($currentGeneration)) { throw 'Installation generation is missing; refusing to uninstall.' }
        $Generation = $currentGeneration
        $state = [string]$properties.State
        $knownStates = @('Disabled', 'Enabled', 'Enabling', 'Restoring', 'RecoveryNeeded')
        if ([string]::IsNullOrWhiteSpace($state) -or $knownStates -notcontains $state) { throw "OLED Taskbar state is missing or unknown ('$state'); refusing to uninstall." }
        # Always obtain a fresh verified result, even if persisted state says Disabled.
        & {
            $currentExe = [string]$properties.CurrentExe
            if ([string]::IsNullOrWhiteSpace($currentExe)) { throw 'Current controller path is missing; recovery data was retained.' }
            $currentExe = [IO.Path]::GetFullPath($currentExe)
            if (-not $currentExe.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-Path -LiteralPath $currentExe -PathType Leaf) -or
                ((Get-Item -LiteralPath $currentExe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Current controller path is unsafe or unavailable; recovery data was retained.'
            }
            $request = Start-Process -FilePath $currentExe -ArgumentList '--disable' -WindowStyle Hidden -PassThru
            if (-not $request.WaitForExit(65000)) { throw 'Restoration timed out; uninstall stopped and recovery data retained.' }
            $request.Refresh()
            $state = [string](Get-ItemProperty -LiteralPath $appKey).State
            if ($request.ExitCode -ne 0 -or $state -ne 'Disabled') { throw 'Restoration was not verified. Uninstall stopped; recovery data retained.' }
        }

        Remove-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name OledTaskbar2 -ErrorAction SilentlyContinue
        foreach ($controller in @(Get-Process -Name 'OledTaskbar2' -ErrorAction SilentlyContinue | Where-Object {
            try { ([IO.Path]::GetFullPath($_.Path)).StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) } catch { $false }
        })) { Stop-Process -Id $controller.Id -ErrorAction Stop }

        $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'OLED Taskbar'
        foreach ($name in @('OLED Taskbar', 'Enable OLED Taskbar', 'Disable OLED Taskbar', 'Uninstall OLED Taskbar')) { Remove-Item -LiteralPath (Join-Path $menu ($name + '.lnk')) -Force -ErrorAction SilentlyContinue }
        if ((Test-Path -LiteralPath $menu) -and -not (Get-ChildItem -LiteralPath $menu -Force -ErrorAction SilentlyContinue)) { Remove-Item -LiteralPath $menu -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\OledTaskbar2' -ErrorAction SilentlyContinue
    }

    $latest = Get-ItemProperty -LiteralPath $appKey -ErrorAction SilentlyContinue
    if (-not $latest -or -not [string]::Equals([string]$latest.InstallGeneration, $Generation, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Output 'Skipping stale OLED Taskbar cleanup because the installation generation changed.'
        return
    }

    $pending = $false
    foreach ($dir in @(Get-ChildItem -LiteralPath $root -Directory -Force -ErrorAction SilentlyContinue)) {
        # Twelve-character folders were produced by the pre-generation v2 installer;
        # sixteen-character folders are the current full-payload versions.
        if ($dir.Name -notmatch '^(?:[A-F0-9]{12}|[A-F0-9]{16})$') { continue }
        if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            try { Remove-Item -LiteralPath $dir.FullName -Force -ErrorAction Stop } catch { $pending = $true }
            continue
        }
        foreach ($name in @('OledTaskbar2.exe', 'NativeControl.exe', 'ShellBridge.dll', 'bridge.ini', 'uninstall-v2.ps1', 'LICENSE-GPL-3.0.txt', 'LICENSE-MinHook.txt')) {
            $path = Join-Path $dir.FullName $name
            if (Test-Path -LiteralPath $path) { try { Remove-Item -LiteralPath $path -Force -ErrorAction Stop } catch { $pending = $true } }
        }
        foreach ($log in @(Get-ChildItem -LiteralPath $dir.FullName -File -Filter 'native-*.log' -Force -ErrorAction SilentlyContinue)) { try { Remove-Item -LiteralPath $log.FullName -Force -ErrorAction Stop } catch { $pending = $true } }
        try {
            if (-not (Get-ChildItem -LiteralPath $dir.FullName -Force -ErrorAction Stop)) { Remove-Item -LiteralPath $dir.FullName -Force -ErrorAction Stop }
            else { $pending = $true }
        } catch { $pending = $true }
    }

    $rootUninstaller = Join-Path $root 'uninstall-v2.ps1'
    $uninstallerBytes = if (Test-Path -LiteralPath $rootUninstaller -PathType Leaf) { [IO.File]::ReadAllBytes($rootUninstaller) } else { $null }
    if (-not $pending) {
        $unexpected = @(Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop | Where-Object {
            -not [string]::Equals([IO.Path]::GetFullPath($_.FullName), [IO.Path]::GetFullPath($rootUninstaller), [StringComparison]::OrdinalIgnoreCase)
        })
        if ($unexpected.Count -ne 0) {
            throw 'Unexpected files remain in the installation root. They were not deleted; automatic cleanup was not scheduled.'
        }
        try { Remove-Item -LiteralPath $rootUninstaller -Force -ErrorAction Stop } catch { $pending = $true }
        if (-not $pending) {
            try {
                if (-not (Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop)) { Remove-Item -LiteralPath $root -Force -ErrorAction Stop }
                else { $pending = $true }
            } catch { $pending = $true }
        }
    }
    if ($pending) {
        if (-not (Test-Path -LiteralPath $rootUninstaller -PathType Leaf)) {
            if ($null -eq $uninstallerBytes) { throw 'Cleanup must be retried, but the installed cleanup script is unavailable.' }
            [IO.File]::WriteAllBytes($rootUninstaller, $uninstallerBytes)
        }
        $once = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'; New-Item -Path $once -Force | Out-Null
        $cleanupCommand = 'powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $rootUninstaller + '" -CleanupOnly -Generation "' + $Generation + '"'
        New-ItemProperty -Path $once -Name OledTaskbar2Cleanup -PropertyType String -Value $cleanupCommand -Force | Out-Null
        Write-Output 'OLED Taskbar is disabled and removed from Search/startup. Sign out to clean remaining files.'
    } else {
        Remove-Item -LiteralPath $appKey -Recurse -Force -ErrorAction SilentlyContinue
        Write-Output 'OLED Taskbar removed. Diagnostic logs are retained in LocalAppData\OledTaskbar2.'
    }
}
finally {
    if ($lockHeld) { $lifecycleMutex.ReleaseMutex() }
    $lifecycleMutex.Dispose()
}
