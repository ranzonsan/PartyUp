# PartyUp packaging script (Windows PowerShell 5.1+ / pwsh 7).
#
# Builds and packages, from the repo sources:
#   PartyUp-<ver>-win64-portable.zip   self-contained portable build (unzip & run)
#   PartyUp-<ver>-win64-Setup.exe      Velopack one-click installer (per-user, no admin)
#   PartyUp-<ver>-win64.msi            Velopack/WiX MSI (welcome/license/conclusion pages,
#                                      install scope chosen at install time)
#   PartyUp-Server-<ver>-win64.zip     partyup.exe + start.cmd + invite.cmd + README
#
# Version comes from the repo-root VERSION file (single source of truth).
# No code signing: SmartScreen warnings are documented in the shipped READMEs and
# the MSI welcome page; the script never hides them.
#
# Usage (from the repo root, or anywhere):
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -Target client
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -Target server
#
# Options:
#   -Target      client | server | all   (default: all)
#   -OutputDir   artifact directory      (default: <repo>\artifacts)
#   -Version     override VERSION        (default: contents of <repo>\VERSION)
#   -NoClean     reuse the staging src/work dirs instead of rebuilding from scratch
#
# Design notes:
#   * Sources are copied to a local staging dir before building. Both MSBuild publish
#     and `go build` fail when invoked on a \\wsl.localhost UNC path on this machine
#     (MSB3030 / "RLock ... Incorrect function"), and the documented workflow is to
#     build from a local Windows directory anyway.
#   * Zip files are produced with Windows' bundled tar.exe (bsdtar): Compress-Archive
#     in Windows PowerShell 5.1 stores entries with backslash separators, which is not
#     a standards-compliant zip (and breaks non-Windows unzip tools).
#   * The script is ASCII-only on purpose: Windows PowerShell 5.1 reads a UTF-8 .ps1
#     without BOM as ANSI, and non-ASCII text then breaks the parser.
[CmdletBinding()]
param(
    [ValidateSet('client', 'server', 'all')]
    [string]$Target = 'all',

    [string]$OutputDir = '',

    [string]$Version = '',

    [switch]$NoClean
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'Continue'
$script:StartedAt = Get-Date

# ---------------------------------------------------------------------------
# Paths and version
# ---------------------------------------------------------------------------

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($RepoRoot)) { throw 'cannot resolve repo root from script path' }
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $RepoRoot 'artifacts'
}
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionFile = Join-Path $RepoRoot 'VERSION'
    if (-not (Test-Path $versionFile)) { throw "VERSION file not found: $versionFile" }
    $Version = (Get-Content -Path $versionFile -Raw).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-+][0-9A-Za-z.-]+)?$') {
    throw "invalid version '$Version' (expected semver like 0.1.0)"
}

# Assembly/File version: numeric x.y.z.0 (prerelease suffix stripped).
$numeric = ($Version -split '[-+]')[0].Split('.')
while ($numeric.Count -lt 4) { $numeric += '0' }
$assemblyVersion = ($numeric[0..3] -join '.')
# MSI wants x.x.x.x as well.
$msiVersion = $assemblyVersion

$StageRoot = Join-Path $env:LOCALAPPDATA 'PartyUp\package'
$SrcDir = Join-Path $StageRoot 'src'
$WorkDir = Join-Path $StageRoot 'work'
$CacheDir = Join-Path $StageRoot 'cache'

$clientExe = 'PartyUp.Client.exe'

Write-Host ''
Write-Host '================================================================='
Write-Host (" PartyUp package  version={0}  target={1}" -f $Version, $Target)
Write-Host (" repo : {0}" -f $RepoRoot)
Write-Host (" out  : {0}" -f $OutputDir)
Write-Host (" stage: {0}" -f $StageRoot)
Write-Host '================================================================='

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Invoke-Step {
    param([string]$Name, [scriptblock]$Block)
    Write-Host ''
    Write-Host ("== {0}" -f $Name) -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Block
    $code = $LASTEXITCODE
    $sw.Stop()
    if ($code -ne 0 -and $null -ne $code) {
        throw ("{0} failed (exit code {1})" -f $Name, $code)
    }
    Write-Host ("   ok ({0:N1}s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

function Copy-Tree {
    param([string]$From, [string]$To, [string[]]$ExcludeDirs = @())
    $robocopyArgs = @($From, $To, '/E', '/R:2', '/W:2', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    if ($ExcludeDirs.Count -gt 0) { $robocopyArgs += '/XD'; $robocopyArgs += $ExcludeDirs }
    & robocopy @robocopyArgs | Out-Null
    $code = $LASTEXITCODE
    # robocopy: 0-7 are success (0 = nothing to copy, 1 = files copied, ...)
    if ($code -ge 8) { throw ("robocopy '{0}' -> '{1}' failed (code {2})" -f $From, $To, $code) }
}

function New-ZipFromDirectory {
    param([string]$ParentDir, [string]$LeafName, [string]$ZipPath)
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path $tar)) { throw 'tar.exe not found (needed for standards-compliant zip output)' }
    & $tar -a -c -f $ZipPath -C $ParentDir $LeafName
    if ($LASTEXITCODE -ne 0) { throw ("tar failed creating {0}" -f $ZipPath) }
}

function Find-CommandPath {
    param([string[]]$Candidates, [string]$ToolName)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-FileSizeMb {
    param([string]$Path)
    if (Test-Path $Path) { return ('{0:N1}' -f ((Get-Item $Path).Length / 1MB)) }
    return '?'
}

function Add-ArtifactRecord {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path $Path)) { return }
    $hash = (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
    $script:Artifacts.Add([pscustomobject]@{
            Label = $Label
            Path  = $Path
            Bytes = (Get-Item $Path).Length
            Sha   = $hash
        })
}

# ---------------------------------------------------------------------------
# Preparation
# ---------------------------------------------------------------------------

$script:Artifacts = New-Object 'System.Collections.Generic.List[object]'

foreach ($dir in @($StageRoot, $CacheDir, $OutputDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

if (-not $NoClean) {
    foreach ($dir in @($SrcDir, $WorkDir)) {
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    }
}
New-Item -ItemType Directory -Force -Path $SrcDir | Out-Null
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# Generated placeholder assets (icon/splash) are committed; regenerate only if missing.
foreach ($asset in @('icon.ico', 'splash.png')) {
    if (-not (Test-Path (Join-Path $RepoRoot (Join-Path 'assets' $asset)))) {
        Write-Host ("assets/{0} missing -> running scripts/make-assets.ps1" -f $asset) -ForegroundColor Yellow
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make-assets.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'make-assets.ps1 failed' }
        break
    }
}

# Stage only what the builds need (client 320K, server 364K sources; excludes caches).
Write-Host ''
Write-Host '== staging sources to local disk (UNC builds are not supported)' -ForegroundColor Cyan
Copy-Tree -From (Join-Path $RepoRoot 'client') -To (Join-Path $SrcDir 'client') -ExcludeDirs @('bin', 'obj', '.cache')
Copy-Tree -From (Join-Path $RepoRoot 'server') -To (Join-Path $SrcDir 'server') -ExcludeDirs @('.cache')
Copy-Tree -From (Join-Path $RepoRoot 'assets') -To (Join-Path $SrcDir 'assets')
foreach ($file in @('VERSION', 'global.json', 'LICENSE')) {
    $source = Join-Path $RepoRoot $file
    if (-not (Test-Path $source)) { throw ("required file missing: {0}" -f $source) }
    Copy-Item $source (Join-Path $SrcDir $file) -Force
}
foreach ($rootProps in @('Directory.Build.props', 'Directory.Packages.props')) {
    $source = Join-Path $RepoRoot $rootProps
    if (Test-Path $source) { Copy-Item $source (Join-Path $SrcDir $rootProps) -Force }
}

$mediaDll = Join-Path $RepoRoot 'src\media-core\out\partyup_media.dll'
if (-not (Test-Path $mediaDll)) {
    throw ("media core not found: {0}`nBuild it first (scripts/build-media-dll.ps1) - this script never builds C++." -f $mediaDll)
}
$stagedMediaDir = Join-Path $SrcDir 'src\media-core\out'
New-Item -ItemType Directory -Force -Path $stagedMediaDir | Out-Null
Copy-Item $mediaDll (Join-Path $stagedMediaDir 'partyup_media.dll') -Force
Write-Host ("   media core: {0:N1} MB" -f ((Get-Item $mediaDll).Length / 1MB))

# Remove previous artifacts of the same version (keep unrelated files).
$velopackOut = Join-Path $OutputDir 'velopack'
if (-not $NoClean) {
    foreach ($stale in @($velopackOut,
            (Join-Path $OutputDir ("PartyUp-{0}-win64-portable.zip" -f $Version)),
            (Join-Path $OutputDir ("PartyUp-{0}-win64-Setup.exe" -f $Version)),
            (Join-Path $OutputDir ("PartyUp-{0}-win64.msi" -f $Version)),
            (Join-Path $OutputDir ("PartyUp-Server-{0}-win64.zip" -f $Version)),
            (Join-Path $OutputDir 'SHA256SUMS.txt'))) {
        if (Test-Path $stale) { Remove-Item $stale -Recurse -Force }
    }
}
New-Item -ItemType Directory -Force -Path $velopackOut | Out-Null

# ---------------------------------------------------------------------------
# Client: self-contained publish + portable zip + Velopack Setup/MSI
# ---------------------------------------------------------------------------

if ($Target -in @('client', 'all')) {
    $dotnet = Find-CommandPath -Candidates @(
        (Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet\dotnet.exe')
    ) -ToolName 'dotnet'
    if (-not $dotnet) { throw 'dotnet.exe not found (need .NET SDK 10.0.400)' }

    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'

    $publishDir = Join-Path $WorkDir 'client-publish'
    if (Test-Path $publishDir) { Remove-Item $publishDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $publishDir | Out-Null

    Write-Host ''
    Write-Host '   first run downloads the win-x64 .NET runtime pack (~100-200 MB);' -ForegroundColor Yellow
    Write-Host '   it can take several minutes. Later runs reuse the NuGet cache.' -ForegroundColor Yellow

    $publishArgs = @(
        'publish', (Join-Path $SrcDir 'client\PartyUp.Client.csproj'),
        '-c', 'Release',
        '-r', 'win-x64',
        '--self-contained', 'true',
        '-o', $publishDir,
        '--nologo',
        ("-p:Version={0}" -f $Version),
        ("-p:AssemblyVersion={0}" -f $assemblyVersion),
        ("-p:FileVersion={0}" -f $assemblyVersion),
        ("-p:InformationalVersion={0}" -f $Version),
        '-p:PublishTrimmed=false',
        '-p:DebugType=None',
        '-p:DebugSymbols=false'
    )
    Invoke-Step -Name 'dotnet publish (Release, win-x64, self-contained)' -Block { & $dotnet @publishArgs }

    # Hard requirements for the payload.
    foreach ($required in @($clientExe, 'partyup_media.dll', 'PartyUp.Client.dll', 'coreclr.dll')) {
        $path = Join-Path $publishDir $required
        if (-not (Test-Path $path)) {
            throw ("published payload is missing {0} ({1})" -f $required, $path)
        }
    }
    # Belt and braces: the csproj copies the media DLL to the output dir; if publish
    # ever stops doing that, fail loudly instead of shipping a broken package.
    # (This is a packaging-side copy, not a code change.)

    # Ship license + portable/install README inside the app dir.
    Copy-Item (Join-Path $SrcDir 'LICENSE') (Join-Path $publishDir 'LICENSE.txt') -Force
    Copy-Item (Join-Path $SrcDir 'assets\portable-README.txt') (Join-Path $publishDir 'README.txt') -Force
    Get-ChildItem -Path $publishDir -Recurse -Filter '*.pdb' -ErrorAction SilentlyContinue | Remove-Item -Force

    $payloadMb = ((Get-ChildItem -Path $publishDir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB)
    Write-Host ("   payload: {0:N1} MB, {1} files" -f $payloadMb,
        (Get-ChildItem -Path $publishDir -Recurse -File).Count)

    # --- portable zip: payload wrapped in PartyUp-<ver>-win64/ -----------------
    Write-Host ''
    Write-Host '== portable zip (tar.exe)' -ForegroundColor Cyan
    $portableParent = Join-Path $WorkDir 'portable'
    $portableLeaf = ("PartyUp-{0}-win64" -f $Version)
    $portableDir = Join-Path $portableParent $portableLeaf
    if (Test-Path $portableParent) { Remove-Item $portableParent -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $portableDir | Out-Null
    Copy-Item (Join-Path $publishDir '*') $portableDir -Recurse -Force
    $portableZip = Join-Path $OutputDir ("{0}-portable.zip" -f $portableLeaf)
    New-ZipFromDirectory -ParentDir $portableParent -LeafName $portableLeaf -ZipPath $portableZip
    Add-ArtifactRecord -Path $portableZip -Label 'portable zip'

    # --- Velopack Setup.exe + MSI ---------------------------------------------
    $vpk = Find-CommandPath -Candidates @(
        (Join-Path $env:USERPROFILE '.dotnet\tools\vpk.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet\tools\vpk.exe')
    ) -ToolName 'vpk'
    if (-not $vpk) {
        throw 'vpk not found. Install it with: dotnet tool install --global vpk'
    }
    Write-Host ''
    Write-Host ("== vpk pack  ({0})" -f $vpk) -ForegroundColor Cyan

    # License for the MSI license page: Velopack expects .md/.rtf; our LICENSE is
    # plain text, which markdown handles, so expose it under a .md name.
    $licenseMd = Join-Path $WorkDir 'installer-license.md'
    Copy-Item (Join-Path $SrcDir 'LICENSE') $licenseMd -Force

    $vpkArgs = @(
        'pack',
        '--packDir', $publishDir,
        '--outputDir', $velopackOut,
        '--channel', 'win',
        '--packId', 'io.partyup.client',
        '--packVersion', $Version,
        '--packTitle', 'PartyUp',
        '--packAuthors', 'PartyUp',
        '--mainExe', $clientExe,
        '--icon', (Join-Path $SrcDir 'assets\icon.ico'),
        '--splashImage', (Join-Path $SrcDir 'assets\splash.png'),
        '--noPortable',
        '--msi',
        '--msiVersion', $msiVersion,
        '--instLocation', 'Either',
        '--instWelcome', (Join-Path $SrcDir 'assets\installer\welcome.md'),
        '--instLicense', $licenseMd,
        '--instConclusion', (Join-Path $SrcDir 'assets\installer\conclusion.md')
    )
    Invoke-Step -Name 'vpk pack (Setup.exe + MSI, per-user/per-machine choice)' -Block { & $vpk @vpkArgs }

    $setupSource = Get-ChildItem -Path $velopackOut -Filter '*Setup*.exe' -File |
        Sort-Object Length -Descending | Select-Object -First 1
    if (-not $setupSource) {
        throw ("vpk did not produce a Setup.exe; output dir contains: " +
            ((Get-ChildItem $velopackOut -File | ForEach-Object { $_.Name }) -join ', '))
    }
    $setupTarget = Join-Path $OutputDir ("PartyUp-{0}-win64-Setup.exe" -f $Version)
    Copy-Item $setupSource.FullName $setupTarget -Force
    Add-ArtifactRecord -Path $setupTarget -Label 'Setup.exe'

    $msiSource = Get-ChildItem -Path $velopackOut -Filter '*.msi' -File | Select-Object -First 1
    if (-not $msiSource) {
        throw ("vpk did not produce an .msi; output dir contains: " +
            ((Get-ChildItem $velopackOut -File | ForEach-Object { $_.Name }) -join ', '))
    }
    $msiTarget = Join-Path $OutputDir ("PartyUp-{0}-win64.msi" -f $Version)
    Copy-Item $msiSource.FullName $msiTarget -Force
    Add-ArtifactRecord -Path $msiTarget -Label 'MSI'

    Write-Host ('   velopack raw output kept in: {0}' -f $velopackOut)
}

# ---------------------------------------------------------------------------
# Server: Go build + zip
# ---------------------------------------------------------------------------

if ($Target -in @('server', 'all')) {
    $go = Find-CommandPath -Candidates @(
        (Join-Path $env:ProgramFiles 'Go\bin\go.exe')
    ) -ToolName 'go'
    if (-not $go) {
        throw 'go.exe not found. Windows Go is expected at C:\Program Files\Go\bin\go.exe;' +
              ' alternatively build server/partyup.exe in WSL and pass the zip step manually.'
    }

    $env:GOCACHE = Join-Path $CacheDir 'gocache'
    $env:GOPATH = Join-Path $CacheDir 'gopath'
    $env:GOFLAGS = '-mod=readonly'
    $env:GOPROXY = 'https://goproxy.cn,direct'
    $env:CGO_ENABLED = '0'
    $env:GOOS = 'windows'
    $env:GOARCH = 'amd64'
    New-Item -ItemType Directory -Force -Path $env:GOCACHE | Out-Null
    New-Item -ItemType Directory -Force -Path $env:GOPATH | Out-Null

    $serverOut = Join-Path $WorkDir 'server'
    if (Test-Path $serverOut) { Remove-Item $serverOut -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $serverOut | Out-Null

    $serverProject = Join-Path $SrcDir 'server'
    $ldflags = ("-s -w -X main.version={0}" -f $Version)
    $goArgs = @('build', '-trimpath', '-ldflags', $ldflags, '-o', (Join-Path $serverOut 'partyup.exe'), './cmd/partyup')
    Invoke-Step -Name 'go build (windows/amd64, version injected via -ldflags)' -Block {
        Push-Location $serverProject
        try { & $go @goArgs } finally { Pop-Location }
    }

    $serverExe = Join-Path $serverOut 'partyup.exe'
    if (-not (Test-Path $serverExe)) { throw 'server build did not produce partyup.exe' }
    $reported = (& $serverExe version 2>&1 | Out-String).Trim()
    Write-Host ("   partyup version: {0}" -f $reported)
    if ($reported -notmatch [regex]::Escape($Version)) {
        throw ("server binary reports '{0}' but VERSION is {1} (ldflags injection failed?)" -f $reported, $Version)
    }

    Write-Host ''
    Write-Host '== server zip (tar.exe)' -ForegroundColor Cyan
    $serverParent = Join-Path $WorkDir 'server-pkg'
    $serverLeaf = ("PartyUp-Server-{0}-win64" -f $Version)
    $serverDir = Join-Path $serverParent $serverLeaf
    if (Test-Path $serverParent) { Remove-Item $serverParent -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $serverDir | Out-Null
    Copy-Item $serverExe (Join-Path $serverDir 'partyup.exe') -Force
    Copy-Item (Join-Path $SrcDir 'assets\server\README.txt') (Join-Path $serverDir 'README.txt') -Force
    Copy-Item (Join-Path $SrcDir 'assets\server\start.cmd') (Join-Path $serverDir 'start.cmd') -Force
    Copy-Item (Join-Path $SrcDir 'assets\server\invite.cmd') (Join-Path $serverDir 'invite.cmd') -Force
    Copy-Item (Join-Path $SrcDir 'LICENSE') (Join-Path $serverDir 'LICENSE.txt') -Force

    $serverZip = Join-Path $OutputDir ("{0}.zip" -f $serverLeaf)
    New-ZipFromDirectory -ParentDir $serverParent -LeafName $serverLeaf -ZipPath $serverZip
    Add-ArtifactRecord -Path $serverZip -Label 'server zip'
}

# ---------------------------------------------------------------------------
# Checksums + summary
# ---------------------------------------------------------------------------

if ($script:Artifacts.Count -eq 0) { throw 'nothing was built' }

$sumsPath = Join-Path $OutputDir 'SHA256SUMS.txt'
$lines = New-Object 'System.Collections.Generic.List[string]'
foreach ($artifact in $script:Artifacts) {
    $lines.Add(("{0} *{1}" -f $artifact.Sha, (Split-Path -Leaf $artifact.Path)))
}
[System.IO.File]::WriteAllLines($sumsPath, $lines)

$elapsed = (Get-Date) - $script:StartedAt
Write-Host ''
Write-Host '=================================================================' -ForegroundColor Green
Write-Host (' DONE in {0:N0}m {1:N0}s' -f [Math]::Floor($elapsed.TotalMinutes), $elapsed.Seconds) -ForegroundColor Green
Write-Host '=================================================================' -ForegroundColor Green
foreach ($artifact in $script:Artifacts) {
    Write-Host (' {0,-14} {1,8:N1} MB  {2}' -f $artifact.Label, ($artifact.Bytes / 1MB), $artifact.Path)
}
Write-Host (' {0,-14} {1,8}     {2}' -f 'checksums', '', $sumsPath)
Write-Host ''
Write-Host 'Unsigned builds: Windows SmartScreen will warn on first run.'
Write-Host 'Tell users to click "More info" -> "Run anyway"; see docs/PACKAGING.md.'
