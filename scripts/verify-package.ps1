# End-to-end acceptance checks for the packaged artifacts.
#
# Requires the artifacts produced by scripts/package.ps1 in -ArtifactsDir.
# This script installs/uninstalls the app (per-user, no admin) and starts the
# client/server, so run it in a session where that is acceptable.
#
# NOTE: this file starts with a UTF-8 BOM on purpose. It contains the Chinese
# log strings we assert on, and Windows PowerShell 5.1 reads .ps1 files without
# a BOM as ANSI, which corrupts those literals and breaks the parser.
#
# Checks:
#   1. portable zip:  extract to a clean dir, launch, assert media engine ready
#      (24/24 exports from real probing), stop;
#   2. Setup.exe:     silent install, launch, assert, silent uninstall, assert no
#      residue (install dir / shortcuts / uninstall registry key);
#   3. MSI:           silent per-user install, launch, assert, uninstall, assert,
#      plus MSI database check that the welcome/license/conclusion/location pages exist;
#   4. server zip:    start partyup.exe, assert INVITE_CODE/READY, issue a second
#      invite, assert `partyup version` matches VERSION.
[CmdletBinding()]
param(
    [string]$ArtifactsDir = '',
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ArtifactsDir)) { $ArtifactsDir = Join-Path $RepoRoot 'artifacts' }
$ArtifactsDir = [System.IO.Path]::GetFullPath($ArtifactsDir)

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -Path (Join-Path $RepoRoot 'VERSION') -Raw).Trim()
}

$script:Results = New-Object 'System.Collections.Generic.List[object]'

# Chinese log needle built from code points: this file must stay ASCII-only because
# Windows PowerShell 5.1 reads a UTF-8 .ps1 without a BOM as ANSI.
# U+5A92 U+4F53 U+5F15 U+64CE U+FF1A U+5DF2 U+5C31 U+7EEA = "media engine: ready"
$mediaReadyNeedle = -join ([char[]]@(0x5A92, 0x4F53, 0x5F15, 0x64CE, 0xFF1A, 0x5DF2, 0x5C31, 0x7EEA))

function Add-Result {
    param([string]$Name, [bool]$Pass, [string]$Detail = '')
    $script:Results.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
    if ($Pass) {
        Write-Host ("[PASS] {0}" -f $Name) -ForegroundColor Green
    } else {
        Write-Host ("[FAIL] {0}" -f $Name) -ForegroundColor Red
    }
    if ($Detail) { Write-Host ("       {0}" -f $Detail) }
}

function Wait-Until {
    param([scriptblock]$Condition, [int]$TimeoutSec = 120, [int]$PollMs = 500)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $result = & $Condition
        if ($result) { return $true }
        Start-Sleep -Milliseconds $PollMs
    }
    return $false
}

function Stop-AppProcesses {
    param([int]$ProcessId = 0)
    if ($ProcessId -gt 0) {
        try { Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue } catch { }
    }
    # taskkill exits non-zero (and writes to stderr) when no process matches; that is
    # fine here, but with $ErrorActionPreference='Stop' the stderr record would throw.
    try { & taskkill.exe /IM 'PartyUp.Client.exe' /F /T *> $null } catch { }
    Start-Sleep -Seconds 1
}

function Start-AppAndWaitReady {
    param([string]$Exe, [string]$WorkDir, [string]$LogPath, [int]$TimeoutSec = 120)
    $errPath = $LogPath + '.err'
    foreach ($file in @($LogPath, $errPath)) { if (Test-Path $file) { Remove-Item $file -Force } }

    $process = Start-Process -FilePath $Exe -WorkingDirectory $WorkDir -PassThru `
        -RedirectStandardOutput $LogPath -RedirectStandardError $errPath

    $ready = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) { break }
        if (Test-Path $LogPath) {
            try {
                $text = Get-Content -Path $LogPath -Raw -Encoding UTF8
                if ($text -match $mediaReadyNeedle) { $ready = $true; break }
            } catch { }
        }
        Start-Sleep -Milliseconds 500
    }

    $text = ''
    if (Test-Path $LogPath) { try { $text = Get-Content -Path $LogPath -Raw -Encoding UTF8 } catch { } }
    return [pscustomobject]@{ Process = $process; Ready = $ready; Log = $text; LogPath = $LogPath }
}

function Assert-MediaReady {
    param([string]$Prefix, [pscustomobject]$Run, [string]$ExpectedDll = '')
    Add-Result -Name ("{0}: client process started and stayed alive" -f $Prefix) `
        -Pass ($Run.Ready -and -not $Run.Process.HasExited)
    Add-Result -Name ("{0}: log shows media engine ready" -f $Prefix) `
        -Pass ($Run.Log -match $mediaReadyNeedle)
    Add-Result -Name ("{0}: media self-check reports 24/24 exports (real probe)" -f $Prefix) `
        -Pass ($Run.Log -match '24/24') -Detail ("log: {0}" -f $Run.LogPath)

    # Packaging accident guard: the app must load partyup_media.dll from its own
    # directory, never from a developer path (this machine has a media-build/ dir
    # outside the package that a user machine would not have). Two independent
    # checks: the loaded module path reported by the OS, and the client's own log.
    $modulePath = $null
    if ($Run.Ready -and -not $Run.Process.HasExited) {
        try {
            $modulePath = ($Run.Process.Modules |
                Where-Object { $_.ModuleName -eq 'partyup_media.dll' } |
                Select-Object -First 1).FileName
        } catch { }
    }
    $loadedFromPackage = $false
    if ($modulePath -and $ExpectedDll) {
        $loadedFromPackage =
            ([System.IO.Path]::GetFullPath($modulePath)).ToLowerInvariant() -eq
            ([System.IO.Path]::GetFullPath($ExpectedDll)).ToLowerInvariant()
    }
    Add-Result -Name ("{0}: media DLL loaded from the package dir (OS module path)" -f $Prefix) `
        -Pass $loadedFromPackage -Detail $modulePath
    if ($ExpectedDll) {
        $expectedLower = ([System.IO.Path]::GetFullPath($ExpectedDll)).ToLowerInvariant()
        Add-Result -Name ("{0}: client log reports the packaged DLL path" -f $Prefix) `
            -Pass ($Run.Log.ToLowerInvariant().Contains($expectedLower))
    }
}

function Invoke-WithDevMediaDirHidden {
    # Temporarily rename %LOCALAPPDATA%\PartyUp\media-build (the dev-bridge copy of
    # the media DLL) so that a packaged client cannot silently load it. Restored in
    # finally, even when a check throws.
    param([scriptblock]$Block)
    $devDir = Join-Path $env:LOCALAPPDATA 'PartyUp\media-build'
    $backup = $devDir + '.verify-disabled'
    $hidden = $false
    if (Test-Path $devDir) {
        if (Test-Path $backup) { Remove-Item $backup -Recurse -Force }
        Rename-Item -Path $devDir -NewName (Split-Path -Leaf $backup)
        $hidden = $true
    }
    Write-Host ("       dev media-build dir hidden during client tests: {0}" -f $hidden)
    try {
        & $Block
    } finally {
        if ($hidden -and (Test-Path $backup)) {
            Rename-Item -Path $backup -NewName 'media-build'
        }
    }
}

function Invoke-Silent {
    param([string]$Exe, [string[]]$Arguments, [int]$TimeoutSec = 300)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        return 999
    }
    return $proc.ExitCode
}

function Get-ShortcutCandidates {
    return @(
        (Join-Path ([Environment]::GetFolderPath('Desktop')) 'PartyUp.lnk'),
        (Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\PartyUp.lnk'),
        (Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\PartyUp\PartyUp.lnk')
    )
}

# ---------------------------------------------------------------------------
# Setup / paths
# ---------------------------------------------------------------------------

$runRoot = Join-Path $env:TEMP ('partyup-verify-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$portableZip = Join-Path $ArtifactsDir ("PartyUp-{0}-win64-portable.zip" -f $Version)
$setupExe = Join-Path $ArtifactsDir ("PartyUp-{0}-win64-Setup.exe" -f $Version)
$msiPath = Join-Path $ArtifactsDir ("PartyUp-{0}-win64.msi" -f $Version)
$serverZip = Join-Path $ArtifactsDir ("PartyUp-Server-{0}-win64.zip" -f $Version)

foreach ($artifact in @($portableZip, $setupExe, $msiPath, $serverZip)) {
    if (-not (Test-Path $artifact)) { throw ("missing artifact: {0}" -f $artifact) }
}

$installDir = Join-Path $env:LOCALAPPDATA 'io.partyup.client'
$sourceMediaDll = Join-Path $RepoRoot 'src\media-core\out\partyup_media.dll'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\io.partyup.client'

Write-Host '================================================================='
Write-Host (" PartyUp package verification   version={0}" -f $Version)
Write-Host (" artifacts: {0}" -f $ArtifactsDir)
Write-Host (" logs     : {0}" -f $runRoot)
Write-Host '================================================================='

# ---------------------------------------------------------------------------
# 1. Portable zip
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== 1/4 portable zip' -ForegroundColor Cyan
Invoke-WithDevMediaDirHidden {
try {
    $portableDir = Join-Path $runRoot 'portable'
    New-Item -ItemType Directory -Force -Path $portableDir | Out-Null
    Expand-Archive -Path $portableZip -DestinationPath $portableDir -Force
    $leaf = Join-Path $portableDir ("PartyUp-{0}-win64" -f $Version)
    $exe = Join-Path $leaf 'PartyUp.Client.exe'
    Add-Result -Name 'portable: extracted PartyUp.Client.exe' -Pass (Test-Path $exe) -Detail $exe

    $dll = Join-Path $leaf 'partyup_media.dll'
    $dllOk = Test-Path $dll
    Add-Result -Name 'portable: partyup_media.dll ships next to the exe' -Pass $dllOk -Detail $dll
    if ($dllOk -and (Test-Path $sourceMediaDll)) {
        $same = (Get-FileHash $dll).Hash -eq (Get-FileHash $sourceMediaDll).Hash
        Add-Result -Name 'portable: media DLL bytes match src/media-core/out' -Pass $same
    }
    Add-Result -Name 'portable: README.txt (SmartScreen guidance) present' -Pass (Test-Path (Join-Path $leaf 'README.txt'))

    $run = Start-AppAndWaitReady -Exe $exe -WorkDir $leaf -LogPath (Join-Path $runRoot 'portable-client.log')
    Assert-MediaReady -Prefix 'portable' -Run $run -ExpectedDll $dll
    Stop-AppProcesses -ProcessId $run.Process.Id
} catch {
    Add-Result -Name 'portable: check crashed' -Pass $false -Detail $_.Exception.Message
}

# ---------------------------------------------------------------------------
# 2. Setup.exe (Velopack one-click, per-user)
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== 2/4 Setup.exe (silent install / run / uninstall)' -ForegroundColor Cyan
try {
    if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    Stop-AppProcesses

    $setupLog = Join-Path $runRoot 'setup-install.log'
    $code = Invoke-Silent -Exe $setupExe -Arguments @('-s', '-l', $setupLog) -TimeoutSec 300
    Add-Result -Name 'setup: silent install exited 0' -Pass ($code -eq 0) -Detail ("exit={0} log={1}" -f $code, $setupLog)

    $installed = Wait-Until -TimeoutSec 180 -Condition { Test-Path (Join-Path $installDir 'current\PartyUp.Client.exe') }
    Add-Result -Name 'setup: installed to %LOCALAPPDATA%\io.partyup.client' -Pass $installed -Detail $installDir

    if ($installed) {
        $installedExe = Join-Path $installDir 'current\PartyUp.Client.exe'
        $run = Start-AppAndWaitReady -Exe $installedExe -WorkDir (Join-Path $installDir 'current') `
            -LogPath (Join-Path $runRoot 'setup-client.log')
        Assert-MediaReady -Prefix 'setup' -Run $run -ExpectedDll (Join-Path $installDir 'current\partyup_media.dll')
        Stop-AppProcesses -ProcessId $run.Process.Id

        $updateExe = Join-Path $installDir 'Update.exe'
        if (Test-Path $updateExe) {
            $uninstallCode = Invoke-Silent -Exe $updateExe -Arguments @('uninstall', '-s') -TimeoutSec 300
            Add-Result -Name 'setup: Update.exe uninstall exited 0' -Pass ($uninstallCode -eq 0) -Detail ("exit={0}" -f $uninstallCode)
        } else {
            Add-Result -Name 'setup: Update.exe present for uninstall' -Pass $false -Detail $updateExe
        }
        $gone = Wait-Until -TimeoutSec 120 -Condition { -not (Test-Path $installDir) }
        Add-Result -Name 'setup: install dir removed after uninstall' -Pass $gone -Detail $installDir

        $leftShortcuts = @(Get-ShortcutCandidates | Where-Object { Test-Path $_ })
        Add-Result -Name 'setup: no shortcut left behind' -Pass ($leftShortcuts.Count -eq 0) -Detail ($leftShortcuts -join '; ')
        Add-Result -Name 'setup: uninstall registry key removed' -Pass (-not (Test-Path $uninstallKey)) -Detail $uninstallKey
    }
} catch {
    Add-Result -Name 'setup: check crashed' -Pass $false -Detail $_.Exception.Message
}

# ---------------------------------------------------------------------------
# 3. MSI (Velopack/WiX; install scope selected at install time = Either)
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== 3/4 MSI (per-user silent install / run / uninstall + pages)' -ForegroundColor Cyan
try {
    # 3a. Static check: expected dialogs exist in the MSI database.
    # WindowsInstaller COM cannot open an MSI over the \\wsl.localhost UNC path
    # (DISP_E_TYPEMISMATCH), so work on a local copy first.
    $dialogs = @()
    $msiLocal = Join-Path $runRoot 'inspect.msi'
    Copy-Item -Path $msiPath -Destination $msiLocal -Force
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $db = $installer.OpenDatabase($msiLocal, 0)
        $view = $db.OpenView('SELECT `Dialog` FROM `Dialog`')
        $view.Execute()
        while ($true) {
            $record = $view.Fetch()
            if ($null -eq $record) { break }
            $dialogs += [string]$record.StringData(1)
        }
        $view.Close()
    } catch {
        Add-Result -Name 'msi: read Dialog table' -Pass $false -Detail $_.Exception.Message
    }
    Write-Host ("       dialogs: {0}" -f ($dialogs -join ', '))
    Add-Result -Name 'msi: welcome page dialog present' -Pass (@($dialogs | Where-Object { $_ -match 'Welcome' }).Count -gt 0)
    Add-Result -Name 'msi: license page dialog present' -Pass (@($dialogs | Where-Object { $_ -match 'License' }).Count -gt 0)
    # Velopack's conclusion page is WiX UI's ExitDialog (the "finish" page).
    Add-Result -Name 'msi: conclusion (finish) page dialog present' `
        -Pass (@($dialogs | Where-Object { $_ -match 'Finish|ExitDialog|Conclusion' }).Count -gt 0)
    Add-Result -Name 'msi: install-location/scope page dialog present' `
        -Pass (@($dialogs | Where-Object { $_ -match 'InstallDir|InstallScope|Location|BrowseDlg' }).Count -gt 0)

    # 3a-2. The MSI stores installer text as UTF-8; check that our actual page content
    # made it in (not just that WiX dialogs exist).
    try {
        $msiText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($msiPath))
        Add-Result -Name 'msi: welcome page text present (unsigned/SmartScreen notice)' `
            -Pass $msiText.Contains('SmartScreen')
        Add-Result -Name 'msi: license page text present (GPL-3.0)' `
            -Pass $msiText.Contains('GNU GENERAL PUBLIC LICENSE')
        Add-Result -Name 'msi: conclusion page text present (first-use and INVITE_CODE)' `
            -Pass ($msiText.Contains('INVITE_CODE') -and $msiText.Contains('PartyUp'))
    } catch {
        Add-Result -Name 'msi: read embedded text' -Pass $false -Detail $_.Exception.Message
    }

    # 3b. Silent per-user install.
    #   * msiexec runs the install in the SYSTEM Windows Installer service, which cannot
    #     read \\wsl.localhost share paths (error 2203/1620) - so install from the local
    #     copy made above, never from the UNC artifact path.
    #   * The MSI is built with --instLocation Either; per-user is forced via the
    #     baked-in ALLUSERS=2/MSIINSTALLPERUSER=1 and the destination is chosen with
    #     VELOPACK_INSTALLDIR (the documented override; the UI exposes InstallScopeDlg
    #     + directory page). Without VELOPACK_INSTALLDIR the per-user default of this
    #     Velopack 1.2.0 template resolves to C:\PartyUp.
    if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    Stop-AppProcesses
    $msiLog = Join-Path $runRoot 'msi-install.log'
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $msiCode = Invoke-Silent -Exe $msiexec -Arguments @(
        '/i', $msiLocal, '/qn', '/norestart',
        'ALLUSERS=2', 'MSIINSTALLPERUSER=1', ('VELOPACK_INSTALLDIR=' + $installDir),
        '/l*v', $msiLog
    ) -TimeoutSec 600
    Add-Result -Name 'msi: silent per-user install exited 0' -Pass ($msiCode -eq 0) -Detail ("exit={0} log={1}" -f $msiCode, $msiLog)

    $msiInstalled = Wait-Until -TimeoutSec 180 -Condition { Test-Path (Join-Path $installDir 'current\PartyUp.Client.exe') }
    Add-Result -Name 'msi: installed to %LOCALAPPDATA%\io.partyup.client' -Pass $msiInstalled -Detail $installDir

    if ($msiInstalled) {
        $msiExe = Join-Path $installDir 'current\PartyUp.Client.exe'
        $run = Start-AppAndWaitReady -Exe $msiExe -WorkDir (Join-Path $installDir 'current') `
            -LogPath (Join-Path $runRoot 'msi-client.log')
        Assert-MediaReady -Prefix 'msi' -Run $run -ExpectedDll (Join-Path $installDir 'current\partyup_media.dll')
        Stop-AppProcesses -ProcessId $run.Process.Id

        $uninstallLog = Join-Path $runRoot 'msi-uninstall.log'
        $msiUninstallCode = Invoke-Silent -Exe $msiexec -Arguments @(
            '/x', $msiLocal, '/qn', '/norestart', '/l*v', $uninstallLog
        ) -TimeoutSec 600
        Add-Result -Name 'msi: uninstall exited 0' -Pass ($msiUninstallCode -eq 0) -Detail ("exit={0} log={1}" -f $msiUninstallCode, $uninstallLog)
        $msiGone = Wait-Until -TimeoutSec 180 -Condition { -not (Test-Path $installDir) }
        Add-Result -Name 'msi: install dir removed after uninstall' -Pass $msiGone -Detail $installDir
        $leftShortcuts = @(Get-ShortcutCandidates | Where-Object { Test-Path $_ })
        Add-Result -Name 'msi: no shortcut left behind' -Pass ($leftShortcuts.Count -eq 0) -Detail ($leftShortcuts -join '; ')
    }
} catch {
    Add-Result -Name 'msi: check crashed' -Pass $false -Detail $_.Exception.Message
}
}   # end Invoke-WithDevMediaDirHidden (client tests)

# ---------------------------------------------------------------------------
# 4. Server zip
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== 4/4 server zip' -ForegroundColor Cyan
try {
    $serverDir = Join-Path $runRoot 'server'
    New-Item -ItemType Directory -Force -Path $serverDir | Out-Null
    Expand-Archive -Path $serverZip -DestinationPath $serverDir -Force
    $serverRoot = Join-Path $serverDir ("PartyUp-Server-{0}-win64" -f $Version)
    $serverExe = Join-Path $serverRoot 'partyup.exe'
    Add-Result -Name 'server: partyup.exe present' -Pass (Test-Path $serverExe) -Detail $serverExe
    foreach ($name in @('start.cmd', 'invite.cmd', 'README.txt', 'LICENSE.txt')) {
        Add-Result -Name ("server: {0} present" -f $name) -Pass (Test-Path (Join-Path $serverRoot $name))
    }

    $versionOut = (& $serverExe version 2>&1 | Out-String).Trim()
    Add-Result -Name 'server: version matches VERSION' -Pass ($versionOut -match [regex]::Escape($Version)) `
        -Detail ("partyup version -> {0}" -f $versionOut)

    $dataDir = Join-Path $runRoot 'server-data'
    $stdout = Join-Path $runRoot 'server-stdout.log'
    $stderr = Join-Path $runRoot 'server-stderr.log'
    $proc = Start-Process -FilePath $serverExe -WorkingDirectory $serverRoot -PassThru `
        -ArgumentList @('serve', '--data', $dataDir, '--http', '127.0.0.1:18080', '--stun-listen', 'off') `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    $ready = Wait-Until -TimeoutSec 60 -Condition {
        if (-not (Test-Path $stdout)) { return $false }
        try { $text = Get-Content $stdout -Raw -Encoding UTF8 } catch { return $false }
        return ($text -match 'READY')
    }
    $serverLog = ''
    if (Test-Path $stdout) { $serverLog = Get-Content $stdout -Raw -Encoding UTF8 }
    Add-Result -Name 'server: startup reached READY' -Pass $ready -Detail ("log: {0}" -f $stdout)
    $invite = ($serverLog -split "`r?`n" | Where-Object { $_ -match '^INVITE_CODE=' } | Select-Object -First 1)
    Add-Result -Name 'server: stdout printed INVITE_CODE on first start' -Pass ($null -ne $invite) -Detail $invite

    try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
    Start-Sleep -Seconds 1

    $inviteOut = (& $serverExe invite --data $dataDir 2>&1 | Out-String)
    $invite2 = ($inviteOut -split "`r?`n" | Where-Object { $_ -match '^INVITE_CODE=' } | Select-Object -First 1)
    Add-Result -Name 'server: invite subcommand issues another code' -Pass ($null -ne $invite2) -Detail $invite2
} catch {
    Add-Result -Name 'server: check crashed' -Pass $false -Detail $_.Exception.Message
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

$failed = @($script:Results | Where-Object { -not $_.Pass })
Write-Host ''
Write-Host '================================================================='
if ($failed.Count -eq 0) {
    Write-Host (" ALL {0} CHECKS PASSED" -f $script:Results.Count) -ForegroundColor Green
} else {
    Write-Host (" {0}/{1} CHECKS FAILED" -f $failed.Count, $script:Results.Count) -ForegroundColor Red
    foreach ($item in $failed) { Write-Host ("   - {0}: {1}" -f $item.Name, $item.Detail) -ForegroundColor Red }
}
Write-Host (" logs: {0}" -f $runRoot)
Write-Host '================================================================='

if ($failed.Count -gt 0) { exit 1 }
exit 0
