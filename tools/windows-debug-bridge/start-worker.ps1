#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 调试桥引导脚本（bootstrap）：本机安装源码 → 本机构建 → 前台运行 worker。

.DESCRIPTION
    由 WSL 侧代理通过 UNC 路径一次性调用。脚本只做四件事：

      1. 校验参数与安装目录安全性（拒绝非本地绝对路径、盘根、UNC，以及非本脚本所有的既有目录）；
      2. 在专用工具目录下新建一次性干净 staging 目录 build-<GUID>，把本脚本所在目录
         （通常为 UNC 源码目录）里的桥源码复制进去；复制时跳过 bin/obj/.git/.vs 与重解析点，
         避免在 UNC 上构建、避免 bin/obj 污染；staging 目录只新建、永不删除；
      3. 在 staging 目录执行 dotnet build（非零退出即停止，绝不启动 worker）；
      4. 前台运行编译产物：<bridge> worker --queue <Queue> --runs-root <RunsRoot>，
         并把 worker 的退出码原样作为本脚本的退出码。

    路径布局（默认）：
      %LOCALAPPDATA%\WslDebugBridge\tool\               工具目录（所有权标记所在层，本脚本只建不删）
      %LOCALAPPDATA%\WslDebugBridge\tool\build-<GUID>\  本次构建用的干净 staging 目录（永不删除）
      %LOCALAPPDATA%\WslDebugBridge\runs\               RunsRoot（本脚本不创建，由 Worker 创建并校验所有权）

    安全约定（硬性）：
      * InstallRoot / RunsRoot 必须是本机盘符绝对路径，不接受 UNC、不接受盘符根目录；
      * InstallRoot 及其每一级已存在的祖先都不得是重解析点（junction/symlink）；
        复制目标路径的每个已存在组件同样逐一校验，防止穿透链接写到别处；
      * 安装目录不存在则创建；已存在时必须是本脚本所有的目录（含所有权标记文件），
        否则拒绝写入；
      * RunsRoot 必须与 InstallRoot、源码目录、Queue 互不重叠（不相等、不互为祖先/后代）；
        本脚本只校验、不创建 RunsRoot——其专用性与所有权由 Worker 负责（建议标记文件
        .wsl-debug-bridge-runs-owner，首行 wsl-debug-bridge/runs-root/v1，语义与工具标记一致）；
      * 本脚本不含任何 Remove-Item，绝不递归删除目录；每次构建都新建 staging 目录，源复制只写新子目录；
      * 参数一律以 PowerShell argv 数组直接传给可执行文件，不做字符串拼接、不经 shell 二次解析；
      * 原生命令（dotnet / worker）执行期间临时把 ErrorActionPreference 降为 Continue，
        try/finally 恢复原值，并即时保存真实 $LASTEXITCODE；
      * dotnet build 失败即终止，不进入 worker 启动步骤。

.PARAMETER Queue
    必填。WSL 侧队列目录在 Windows 上的绝对路径，通常为 UNC，
    例如 \\wsl.localhost\<distro>\<user>\.wdb\queue。

.PARAMETER InstallRoot
    本机专用工具目录，默认 $env:LOCALAPPDATA\WslDebugBridge\tool（该层存放所有权标记）。
    每次运行会在其下新建一个 build-<GUID> 干净 staging 目录，源码只复制进该新子目录；
    桥项目应提供源码目录下的 Bridge.csproj（自带 AssemblyName 与 TargetFramework），
    构建在 staging 目录内进行。

.PARAMETER RunsRoot
    worker 的运行目录，默认取 InstallRoot 同基目录下的 runs，
    即默认安装下为 $env:LOCALAPPDATA\WslDebugBridge\runs。
    本脚本只校验（本地绝对、非重解析点、与 InstallRoot/源码/Queue 不重叠）并原样传给 worker，
    不创建该目录；RunsRoot 的专用性与所有权校验由 Worker 负责。

.EXAMPLE
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        \\wsl.localhost\<distro>\<user>\galleries-manager\tools\windows-debug-bridge\start-worker.ps1 `
        -Queue '\\wsl.localhost\<distro>\<user>\.wdb\queue'

.OUTPUTS
    进程退出码 = worker 的退出码；引导阶段失败时为 1，构建失败时为 dotnet build 的退出码。

.NOTES
    只写本机 InstallRoot（含其下新建的 build-<GUID> staging）；RunsRoot 由 Worker 决定与创建。
    不注册服务、不写注册表、不开监听端口、不改业务代码。
    由于源码被复制到仓库之外构建，构建与求值一律传
    -p:ImportDirectoryBuildProps=false -p:ImportDirectoryBuildTargets=false
    -p:ImportDirectoryPackagesProps=false，桥项目必须自带全部属性
    （TargetFramework / AssemblyName 等），不依赖任何上层 props/targets。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Queue,

    [string]$InstallRoot = '',

    [string]$RunsRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- 常量 -------------------------------------------------------------------

# 所有权标记：只有带该标记（或本身为空目录）的安装目录才允许被本脚本写入
$MarkerName  = '.wsl-debug-bridge-tool-owner'
$MarkerToken = 'wsl-debug-bridge/tool-root/v1'

# 复制源码时跳过的目录名（避免搬运构建产物与 VCS 元数据）
$SkipDirNames = @('bin', 'obj', '.git', '.vs')

# 架构合同约定的桥项目入口（staging 目录下的 csproj；AssemblyName 由项目自带）
$ContractCsprojName = 'Bridge.csproj'

# 每次构建使用的干净 staging 目录前缀（build-<GUID>，只新建、永不删除）
$StagingDirPrefix = 'build-'

# 必须关闭的目录级属性导入：桥项目自带全部值，避免继承仓库外/上层任何 props 或 targets
$NoInheritArgs = @(
    '-p:ImportDirectoryBuildProps=false'
    '-p:ImportDirectoryBuildTargets=false'
    '-p:ImportDirectoryPackagesProps=false'
)

# 构建失败时用于跳出 try 块的中立标记（避免在 try 内部直接 exit）
$BuildFailedSentinel = 'wdb-bootstrap-build-failed'

# 构建配置
$BuildConfiguration = 'Release'

# 路径尾部去分隔符时使用的字符集
$SepChars = [char[]]@([char]'\', [char]'/')

# 统计与退出码初始化（Set-StrictMode 下必须先赋值）
$Script:CopiedFileCount = 0
$Script:ExitCode = 1
$LASTEXITCODE = 0


# --- 基础输出 ---------------------------------------------------------------

# 诊断输出统一走 stderr，避免与 worker 自身的 stdout（可能是机器可读输出）混流。
function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Message)
    [Console]::Error.WriteLine("[wdb] " + $Message)
}

function Write-Fail {
    param([Parameter(Mandatory = $true)][string]$Message)
    [Console]::Error.WriteLine("[wdb][ERROR] " + $Message)
}

# 把 argv 数组渲染成可直接复制执行的命令行（仅用于显示，执行时不走字符串）
function Format-Argv {
    param([Parameter(Mandatory = $true)][string[]]$Argv)

    $parts = @()
    foreach ($item in $Argv) {
        if ($item -match '[\s"]') {
            $parts += ('"' + ($item -replace '"', '\"') + '"')
        }
        else {
            $parts += $item
        }
    }
    return ($parts -join ' ')
}


# --- 参数校验 ---------------------------------------------------------------

# 校验“本机绝对路径”（InstallRoot / RunsRoot 共用）：非 UNC、非根相对、非盘根
function Resolve-LocalAbsoluteRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw ("{0} 不能为空。" -f $Name)
    }

    $raw = $Path.Trim()

    # 以分隔符开头即 UNC（\\server\share）或根相对（\foo）、或扩展前缀（\\?\），一律拒绝
    if ($raw.StartsWith('\') -or $raw.StartsWith('/')) {
        throw ("{0} 必须是本机盘符绝对路径，不接受 UNC 或根相对路径：{1}" -f $Name, $raw)
    }
    if (-not [System.IO.Path]::IsPathRooted($raw)) {
        throw ("{0} 必须是绝对路径：{1}" -f $Name, $raw)
    }

    $full = [System.IO.Path]::GetFullPath($raw)

    if ($full -notmatch '^[A-Za-z]:\\') {
        throw ("{0} 必须是本机盘符绝对路径：{1}" -f $Name, $full)
    }

    $normalized = $full.TrimEnd($SepChars)
    if ($normalized.Length -le 2) {
        throw ("{0} 不能是盘符根目录：{1}" -f $Name, $full)
    }

    return $normalized
}

# Queue 是 Windows 侧可见的绝对路径（UNC 或盘符绝对路径），不做存在性强校验
function Resolve-QueueArgument {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'Queue 不能为空：它是必填参数（WSL 队列目录在 Windows 上的 UNC 路径）。'
    }

    $raw = $Path.Trim()
    $isUnc = $raw.StartsWith('\\') -or $raw.StartsWith('//')
    $isDriveAbsolute = ($raw -match '^[A-Za-z]:[\\/]')

    if (-not ($isUnc -or $isDriveAbsolute)) {
        throw ("Queue 必须是 Windows 可见的绝对路径（UNC 或盘符绝对路径）：{0}" -f $raw)
    }

    return $raw
}

# 路径安全：给定路径的每一级“已存在”组件都不得是重解析点（junction/symlink）。
# 遇到第一个不存在的组件即停止（其后都不存在，也就无从穿透）。
# InstallRoot / RunsRoot / 复制目标的每个已存在组件都调用本函数。
function Assert-NoReparseComponent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $full = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw ("无法解析 {0} 的路径根：{1}" -f $Name, $full)
    }

    # 只逐级校验盘符绝对路径；UNC 的 \\server\share 前缀由 Windows 侧解析，不在本脚本校验范围
    if ($root -notmatch '^[A-Za-z]:\\$') {
        return
    }

    $current = $root.TrimEnd($SepChars)
    $segments = @($full.Substring($root.Length) -split '[\\/]' | Where-Object { $_ -ne '' })
    foreach ($segment in $segments) {
        $current = $current + '\' + $segment
        if (-not (Test-Path -LiteralPath $current)) {
            return
        }
        $item = Get-Item -LiteralPath $current -Force
        if ((($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw ("{0} 的路径组件是重解析点（junction/symlink），拒绝写入：{1}" -f $Name, $current)
        }
    }
}

# 判断 $Child 是否等于 $Parent、或位于 $Parent 之内（按 \ 边界比较，忽略大小写）
function Test-SameOrDescendantPath {
    param(
        [Parameter(Mandatory = $true)][string]$Child,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $childFull = $Child.TrimEnd($SepChars)
    $parentFull = $Parent.TrimEnd($SepChars)
    if ($childFull -eq $parentFull) {
        return $true
    }
    return $childFull.StartsWith($parentFull + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

# 安装目录所有权判定：不存在→Create；空目录→Adopt；带本脚本标记→Reuse；其余→拒绝
function Assert-InstallRootUsable {
    param([Parameter(Mandatory = $true)][string]$Root)

    Assert-NoReparseComponent -Path $Root -Name 'InstallRoot'

    if (-not (Test-Path -LiteralPath $Root)) {
        return 'Create'
    }

    $item = Get-Item -LiteralPath $Root -Force
    if (-not $item.PSIsContainer) {
        throw ("InstallRoot 已存在且不是目录，拒绝写入：{0}" -f $Root)
    }
    if ((($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw ("InstallRoot 是重解析点（junction/symlink），拒绝写入：{0}" -f $Root)
    }

    $markerPath = [System.IO.Path]::Combine($Root, $MarkerName)
    if (Test-Path -LiteralPath $markerPath) {
        $firstLine = Get-Content -LiteralPath $markerPath -TotalCount 1
        $firstLine = ([string]$firstLine).TrimStart([char]0xFEFF).Trim()
        if ($firstLine -ne $MarkerToken) {
            throw ("InstallRoot 的所有权标记内容不符（期望 {0}），拒绝写入：{1}" -f $MarkerToken, $Root)
        }
        return 'Reuse'
    }

    $children = @(Get-ChildItem -LiteralPath $Root -Force)
    if ($children.Count -gt 0) {
        throw ("InstallRoot 已存在但非本脚本所有（缺少标记文件 {0}），拒绝写入：{1}" -f $markerPath, $Root)
    }

    return 'Adopt'
}


# --- 源码复制 ---------------------------------------------------------------

# 手工递归复制：显式跳过 bin/obj/.git/.vs 与重解析点，不使用任何删除操作，
# 并在每次写入前校验目标侧每个已存在组件（防止既有的 junction/symlink 把写入引到别处）
function Copy-BridgeSourceTree {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$DestinationDir,
        [string]$SkipRoot = ''
    )

    Assert-NoReparseComponent -Path $DestinationDir -Name '复制目标目录'

    $entries = @(Get-ChildItem -LiteralPath $SourceDir -Force)
    foreach ($entry in $entries) {
        $entryPath = [System.IO.Path]::GetFullPath($entry.FullName)

        if ((($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
            Write-Step ("跳过重解析点：{0}" -f $entryPath)
            continue
        }

        if ($entry.PSIsContainer) {
            if ($SkipDirNames -contains $entry.Name) {
                Write-Step ("跳过目录：{0}" -f $entryPath)
                continue
            }

            $trimmedEntry = $entryPath.TrimEnd($SepChars)
            if ($SkipRoot -and (($trimmedEntry -eq $SkipRoot) -or $trimmedEntry.StartsWith($SkipRoot + '\', [System.StringComparison]::OrdinalIgnoreCase))) {
                Write-Step ("跳过安装目录自身：{0}" -f $entryPath)
                continue
            }

            $childDest = [System.IO.Path]::Combine($DestinationDir, $entry.Name)
            if (Test-Path -LiteralPath $childDest -PathType Leaf) {
                throw ("复制目标已存在同名文件，拒绝改为目录（本脚本不删除任何内容）：{0}" -f $childDest)
            }
            if (-not (Test-Path -LiteralPath $childDest)) {
                [System.IO.Directory]::CreateDirectory($childDest) | Out-Null
            }
            Copy-BridgeSourceTree -SourceDir $entryPath -DestinationDir $childDest -SkipRoot $SkipRoot
        }
        else {
            $destFile = [System.IO.Path]::Combine($DestinationDir, $entry.Name)
            Assert-NoReparseComponent -Path $destFile -Name '复制目标文件'
            if (Test-Path -LiteralPath $destFile -PathType Container) {
                throw ("复制目标已存在同名目录，拒绝覆盖（本脚本不删除任何内容）：{0}" -f $destFile)
            }
            Copy-Item -LiteralPath $entryPath -Destination $destFile -Force
            $Script:CopiedFileCount = $Script:CopiedFileCount + 1
        }
    }
}


# --- 构建产物定位 -----------------------------------------------------------

# 只认 MSBuild 求值得到的 TargetPath（.NET 8+ 的 -getProperty）：求值失败或结果不可用即报错。
# 绝不在 bin 目录里猜产物——桥项目的 AssemblyName 是 wdb，用 csproj 文件名去猜必然猜错。
function Resolve-BuildArtifact {
    param(
        [Parameter(Mandatory = $true)][string]$CsprojPath,
        [Parameter(Mandatory = $true)][string]$DotnetPath,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [string[]]$ExtraArgs = @()
    )

    $probeArgs = @('msbuild', $CsprojPath, '-nologo', '-getProperty:TargetPath', ("-p:Configuration={0}" -f $Configuration)) + $ExtraArgs
    Write-Step ("求值产物路径：" + (Format-Argv -Argv (@($DotnetPath) + $probeArgs)))

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $probe = $null
    $probeExit = -1
    try {
        $probe = & $DotnetPath @probeArgs
        $probeExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedEap
    }

    $probeLines = @($probe | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    $probeText = ($probeLines -join "`n")

    if ($probeExit -ne 0) {
        throw ("MSBuild 求值失败（退出码 {0}），无法取得 TargetPath；本脚本不猜测产物。项目：{1}`n--- 求值输出 ---`n{2}" -f $probeExit, $CsprojPath, $probeText)
    }
    if ($probeLines.Count -lt 1) {
        throw ("MSBuild 未返回 TargetPath；本脚本不猜测产物。项目：{0}" -f $CsprojPath)
    }

    $targetPath = ([string]$probeLines[$probeLines.Count - 1]).Trim()
    if ([string]::IsNullOrWhiteSpace($targetPath)) {
        throw ("MSBuild 返回的 TargetPath 为空；本脚本不猜测产物。项目：{0}" -f $CsprojPath)
    }
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        throw ("MSBuild 返回的 TargetPath 不是已存在的文件：{0}`n--- 求值输出 ---`n{1}" -f $targetPath, $probeText)
    }

    return $targetPath
}


# --- 主流程 -----------------------------------------------------------------

try {
    # 0. 参数与路径校验
    $queuePath = Resolve-QueueArgument -Path $Queue

    if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
        if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
            throw '未指定 -InstallRoot，且环境变量 LOCALAPPDATA 为空，无法推导默认安装目录。'
        }
        $InstallRoot = [System.IO.Path]::Combine($env:LOCALAPPDATA, 'WslDebugBridge\tool')
        Write-Step ("-InstallRoot 未指定，使用默认值：{0}" -f $InstallRoot)
    }
    $installRootFull = Resolve-LocalAbsoluteRoot -Path $InstallRoot -Name 'InstallRoot'

    if ([string]::IsNullOrWhiteSpace($RunsRoot)) {
        $installParentRaw = [System.IO.Path]::GetDirectoryName($installRootFull)
        if ([string]::IsNullOrWhiteSpace($installParentRaw)) {
            throw ("-RunsRoot 未指定，且无法从 InstallRoot 推导上级目录（{0}）；请显式传 -RunsRoot。" -f $installRootFull)
        }
        $installParent = $installParentRaw.TrimEnd($SepChars)
        if ($installParent.Length -le 2) {
            throw ("-RunsRoot 未指定，但 InstallRoot 位于盘符根下一级（{0}），推导出的默认 RunsRoot 会落在盘根；请显式传 -RunsRoot。" -f $installRootFull)
        }
        $runsRootCandidate = [System.IO.Path]::Combine($installParent, 'runs')
        $runsRootFull = Resolve-LocalAbsoluteRoot -Path $runsRootCandidate -Name 'RunsRoot'
        Write-Step ("-RunsRoot 未指定，取 InstallRoot 同基目录：{0}" -f $runsRootFull)
    }
    else {
        $runsRootFull = Resolve-LocalAbsoluteRoot -Path $RunsRoot -Name 'RunsRoot'
    }

    # RunsRoot 只校验、不创建：其专用性与所有权标记由 Worker 负责
    Assert-NoReparseComponent -Path $runsRootFull -Name 'RunsRoot'

    if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        throw '无法确定脚本所在目录（$PSScriptRoot 为空）；请以 -File 方式运行本脚本。'
    }
    $sourceRoot = ([System.IO.Path]::GetFullPath($PSScriptRoot)).TrimEnd($SepChars)

    Write-Step ("源码目录：{0}" -f $sourceRoot)
    Write-Step ("队列目录：{0}" -f $queuePath)
    Write-Step ("安装目录：{0}" -f $installRootFull)
    Write-Step ("运行目录：{0}" -f $runsRootFull)

    if ((-not $queuePath.StartsWith('\\')) -and (-not $queuePath.StartsWith('//'))) {
        Write-Step '提示：Queue 不是 UNC 路径，请确认它确实指向 WSL 侧队列目录。'
    }
    if (-not (Test-Path -LiteralPath $queuePath -ErrorAction SilentlyContinue)) {
        Write-Step ("提示：Queue 当前不可访问（{0}）；若由 WSL 侧延迟创建，可忽略本提示。" -f $queuePath)
    }

    if (($sourceRoot -eq $installRootFull) -or $sourceRoot.StartsWith($installRootFull + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("本脚本必须从源码目录（UNC）运行，不能从安装目录内运行，否则会自我复制：源码 {0}，InstallRoot {1}" -f $sourceRoot, $installRootFull)
    }
    if ($installRootFull.StartsWith($sourceRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("InstallRoot 不能位于源码目录内部（会把复制目标再次复制进来）：{0}" -f $installRootFull)
    }

    # RunsRoot 重叠校验：不得与 InstallRoot / 源码目录 / Queue 相等或互为祖先、后代
    if (Test-SameOrDescendantPath -Child $runsRootFull -Parent $installRootFull) {
        throw ("RunsRoot 不能等于或位于 InstallRoot 之内：RunsRoot {0}，InstallRoot {1}" -f $runsRootFull, $installRootFull)
    }
    if (Test-SameOrDescendantPath -Child $installRootFull -Parent $runsRootFull) {
        throw ("RunsRoot 不能是 InstallRoot 的祖先：RunsRoot {0}，InstallRoot {1}" -f $runsRootFull, $installRootFull)
    }
    if (Test-SameOrDescendantPath -Child $runsRootFull -Parent $sourceRoot) {
        throw ("RunsRoot 不能等于或位于源码目录之内：RunsRoot {0}，源码 {1}" -f $runsRootFull, $sourceRoot)
    }
    if (Test-SameOrDescendantPath -Child $sourceRoot -Parent $runsRootFull) {
        throw ("RunsRoot 不能是源码目录的祖先：RunsRoot {0}，源码 {1}" -f $runsRootFull, $sourceRoot)
    }
    if ($queuePath -match '^[A-Za-z]:[\\/]') {
        $queueFull = ([System.IO.Path]::GetFullPath($queuePath)).TrimEnd($SepChars)
        if ((Test-SameOrDescendantPath -Child $runsRootFull -Parent $queueFull) -or (Test-SameOrDescendantPath -Child $queueFull -Parent $runsRootFull)) {
            throw ("RunsRoot 与 Queue 不能相等或互为祖先、后代：RunsRoot {0}，Queue {1}" -f $runsRootFull, $queueFull)
        }
    }

    # 1. 安装目录安全校验 + 创建（不做任何删除；每次构建都在其下新建 staging 子目录）
    $ownershipState = Assert-InstallRootUsable -Root $installRootFull

    switch ($ownershipState) {
        'Create' { Write-Step '安装目录不存在，将创建。' }
        'Adopt'  { Write-Step '安装目录为空目录，接管为本脚本所有。' }
        'Reuse'  { Write-Step '安装目录已带本脚本所有权标记，复用（不删除任何既有内容）。' }
    }

    [System.IO.Directory]::CreateDirectory($installRootFull) | Out-Null

    $markerPath = [System.IO.Path]::Combine($installRootFull, $MarkerName)
    $markerLines = @(
        $MarkerToken
        ("source={0}" -f $sourceRoot)
        ("updated={0}" -f (Get-Date).ToString('yyyy-MM-ddTHH:mm:ssK'))
    )
    Set-Content -LiteralPath $markerPath -Value $markerLines -Encoding UTF8

    # RunsRoot 不在此创建：交由 Worker 建目录并校验其所有权标记
    Write-Step ("RunsRoot 由 Worker 自行创建并校验所有权：{0}" -f $runsRootFull)

    # 2. 新建一次性干净 staging 目录，把桥源码复制进去（跳过 bin/obj/.git/.vs 与重解析点）
    $buildSourceRoot = [System.IO.Path]::Combine($installRootFull, ($StagingDirPrefix + [guid]::NewGuid().ToString('N')))
    Assert-NoReparseComponent -Path $buildSourceRoot -Name 'staging 目录'
    [System.IO.Directory]::CreateDirectory($buildSourceRoot) | Out-Null
    Write-Step ("staging 目录（本次构建专用，永不删除）：{0}" -f $buildSourceRoot)

    Write-Step ("复制源码：{0} -> {1}" -f $sourceRoot, $buildSourceRoot)
    Copy-BridgeSourceTree -SourceDir $sourceRoot -DestinationDir $buildSourceRoot -SkipRoot $buildSourceRoot

    if ($Script:CopiedFileCount -le 0) {
        throw ("源码目录内没有可复制的文件：{0}" -f $sourceRoot)
    }
    Write-Step ("复制完成：{0} 个文件。" -f $Script:CopiedFileCount)

    # 3. 定位桥项目（合同约定：源码根目录 Bridge.csproj，已复制进 staging 目录）
    $csprojPath = [System.IO.Path]::Combine($buildSourceRoot, $ContractCsprojName)

    if (-not (Test-Path -LiteralPath $csprojPath)) {
        $candidates = @(Get-ChildItem -LiteralPath $buildSourceRoot -Filter '*.csproj' -File -Force)
        if ($candidates.Count -eq 1) {
            $csprojPath = $candidates[0].FullName
            Write-Step ("未找到 {0}，回退使用唯一的项目文件：{1}" -f $ContractCsprojName, $csprojPath)
        }
        else {
            $names = '（无）'
            if ($candidates.Count -gt 0) {
                $names = (($candidates | ForEach-Object { $_.Name }) -join ', ')
            }
            throw ("未找到桥项目 {0}（架构合同约定位于源码根目录）。staging 目录内的 csproj：{1}" -f $ContractCsprojName, $names)
        }
    }
    Write-Step ("桥项目：{0}" -f $csprojPath)

    # 4. dotnet 就绪检查（只认 Application，避免把 Alias/Function 当成 dotnet）
    $dotnetCommand = Get-Command -Name 'dotnet' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $dotnetCommand) {
        throw '未找到 dotnet 可执行文件（Application）：需要本机已安装 .NET 10 SDK，且 dotnet 在 PATH 中。'
    }
    $dotnetPath = [string]$dotnetCommand.Path
    if ([string]::IsNullOrWhiteSpace($dotnetPath)) {
        $dotnetPath = [string]$dotnetCommand.Source
    }
    if ([string]::IsNullOrWhiteSpace($dotnetPath)) {
        throw ("无法解析 dotnet 可执行文件路径：{0}" -f $dotnetCommand.Name)
    }

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $sdkVersion = ''
    $sdkProbeExit = -1
    try {
        $sdkVersion = & $dotnetPath --version
        $sdkProbeExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedEap
    }
    $sdkVersion = ([string]$sdkVersion).Trim()
    if ($sdkProbeExit -ne 0) {
        throw ("dotnet --version 失败（退出码 {0}）：{1}" -f $sdkProbeExit, $dotnetPath)
    }
    Write-Step ("dotnet：{0}（SDK {1}）" -f $dotnetPath, $sdkVersion)

    # 5. 本机构建（staging 本地目录内构建；失败即停）
    #    一律关闭目录级 props/targets 导入：桥项目自带全部属性，不继承上层任何配置。
    $csprojText = Get-Content -LiteralPath $csprojPath -Raw
    $extraBuildArgs = @()
    if ($csprojText -notmatch '<TargetFramework') {
        $extraBuildArgs = @('-p:TargetFramework=net10.0')
        Write-Step '提示：项目未声明 TargetFramework，按合同补 -p:TargetFramework=net10.0。'
    }

    $probeExtraArgs = @($NoInheritArgs) + $extraBuildArgs
    $buildArgs = @('build', $csprojPath, '-c', $BuildConfiguration, '--nologo', '-nodeReuse:false', '-p:UseSharedCompilation=false') + $NoInheritArgs + $extraBuildArgs

    Write-Step ("构建：" + (Format-Argv -Argv (@($dotnetPath) + $buildArgs)))

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $buildExit = -1
    try {
        & $dotnetPath @buildArgs
        $buildExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedEap
    }

    if ($buildExit -ne 0) {
        Write-Fail ("dotnet build 失败（退出码 {0}），已停止，不启动 worker。" -f $buildExit)
        $Script:ExitCode = $buildExit
        throw $BuildFailedSentinel
    }

    # 6. 前台运行 worker
    $artifactPath = Resolve-BuildArtifact -CsprojPath $csprojPath -DotnetPath $dotnetPath -Configuration $BuildConfiguration -ExtraArgs $probeExtraArgs
    Write-Step ("构建产物：{0}" -f $artifactPath)

    $entryExe = ''
    $workerArgv = @()
    if ($artifactPath -match '\.exe$') {
        $entryExe = $artifactPath
        $workerArgv = @($entryExe, 'worker', '--queue', $queuePath, '--runs-root', $runsRootFull)
    }
    else {
        $appHostPath = [System.IO.Path]::ChangeExtension($artifactPath, '.exe')
        if (Test-Path -LiteralPath $appHostPath) {
            $entryExe = $appHostPath
            $workerArgv = @($entryExe, 'worker', '--queue', $queuePath, '--runs-root', $runsRootFull)
        }
        else {
            $workerArgv = @($dotnetPath, $artifactPath, 'worker', '--queue', $queuePath, '--runs-root', $runsRootFull)
        }
    }

    Write-Step ("前台运行：" + (Format-Argv -Argv $workerArgv))

    $workerExitCode = 1
    Push-Location -LiteralPath $buildSourceRoot
    try {
        # 原生命令期间降 EAP：worker 写 stderr 不应被当成终止性错误，且必须拿到真实退出码
        $savedEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            if ($entryExe) {
                & $entryExe 'worker' '--queue' $queuePath '--runs-root' $runsRootFull
            }
            else {
                & $dotnetPath $artifactPath 'worker' '--queue' $queuePath '--runs-root' $runsRootFull
            }
            $workerExitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedEap
        }
    }
    finally {
        Pop-Location
    }

    if ($null -eq $workerExitCode) {
        $workerExitCode = 1
    }

    Write-Step ("worker 退出码：{0}" -f $workerExitCode)
    $Script:ExitCode = $workerExitCode
}
catch {
    # 构建失败已在上方把退出码写入 $Script:ExitCode（用中立标记跳出 try，不在 try 内 exit）
    if ($_.Exception.Message -ne $BuildFailedSentinel) {
        Write-Fail $_.Exception.Message
        # 实机排障用：异常类型 + 调用位置 + 语句 + 脚本堆栈（诊断自身失败不影响退出码）
        try {
            Write-Fail ("异常类型：{0}" -f $_.Exception.GetType().FullName)
            if ($_.InvocationInfo) {
                Write-Fail ("调用位置：{0}" -f $_.InvocationInfo.PositionMessage)
                Write-Fail ("涉及语句：{0}" -f $_.InvocationInfo.MyCommand)
            }
            Write-Fail ("脚本堆栈：{0}" -f $_.ScriptStackTrace)
        }
        catch {
            Write-Fail '（补充诊断信息输出失败，已忽略。）'
        }
        $Script:ExitCode = 1
    }
}

exit $Script:ExitCode
