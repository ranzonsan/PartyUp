// ===========================================================================
// Windows 调试桥 · Worker（Windows 前台工作进程）
//
// 入口：  public static class Worker { public static Task<int> RunAsync(string[] args); }
// 用法：  wdb worker --queue <队列目录> --runs-root <本机运行根> [--dotnet <可执行文件>]
//                    [--allow-non-windows]
//
// 职责（与 Contracts.cs / BridgeLayout 一一对应）：
//   * 扫描 <queue>/jobs/<id>/request.json（CLI 最后原子提交），串行认领任务；
//   * 把 <queue>/jobs/<id>/payload 复制到本机专属副本 <runs-root>/<id>/src 后再构建/运行
//     —— 绝不在队列（UNC）目录里构建，绝不删除 payload 或宿主任意文件；
//   * 只写 status.json / worker.json，并按 250ms 轮询 cancel、1s 写心跳；
//   * 进程一律 ArgumentList 传参（不经 shell），stdout/stderr 双流异步 UTF-8 分别落盘；
//   * 超时 / cancel / shutdown 一律终止整棵进程树。
//
// 状态与阶段映射（Contracts.JobStates / JobPhases）：
//   认领              -> state=queued,   phase=submitted
//   复制 payload      -> state=queued,   phase=copying
//   构建              -> state=running,  phase=building
//   求值产物 / 启动   -> state=running,  phase=starting
//   运行              -> state=running,  phase=running
//   收到停止信号      -> state=running,  phase=stopping（随后写终态）
//   终态              -> succeeded / failed / canceled / timeout / rejected / interrupted, phase=done
//
// ---------------------------------------------------------------------------
// 进程启动（与 WindowsJobRunner.cs 的接线）
// ---------------------------------------------------------------------------
//   WindowsJobRunner.cs 的模块初始化器把 WindowsProcessJob 注册进
//   JobRunnerGateway.WindowsProcessFactory。它内部顺序恒为
//   CreateProcessW(CREATE_SUSPENDED) → AssignProcessToJobObject(KILL_ON_JOB_CLOSE) → ResumeThread，
//   故不存在「先启动再挂 Job」的派生竞态；Kill() 走 TerminateJobObject 全树终结，
//   Dispose() 关 Job 句柄由 KILL_ON_JOB_CLOSE 兜底收走整棵树。
//   工厂未注册时本文件以 ERR_JOB_RUNNER_UNAVAILABLE 熔断，绝不降级为普通 Process。
//
//   非 Windows 宿主默认拒绝启动（绝不能把目标进程以普通 Process 拉起来）；
//   只有显式 --allow-non-windows（仅 fixture 测试）才走 PlainProcessJob
//   （普通 Process + Kill(entireProcessTree)），并在 stderr 打印醒目告警。
//
// ---------------------------------------------------------------------------
// runs-root 所有权与安全门禁（与 start-worker.ps1 的约定对齐）
// ---------------------------------------------------------------------------
//   标记文件 <runs-root>/.wsl-debug-bridge-runs-owner，首行 token "wsl-debug-bridge/runs-root/v1"：
//     目录不存在        → 新建并写标记；
//     已存在且为空      → 接管并写标记；
//     标记首行匹配      → 复用；
//     非空且无标记/不符 → 拒绝启动（exit 6）—— 绝不写进用户的既有目录，且绝不删除任何文件。
//   runs-root 还必须：与 queue 双向不重叠；Windows 下为本机盘符绝对路径（拒 UNC）；
//   与工具目录（AppContext.BaseDirectory）双向不重叠；非盘符根/系统目录/用户主目录；祖先无重解析点。
//   每个任务目录 <runs-root>/<id> 用 FileMode.CreateNew 写 .wdb-run-owner 作独占认领（O_EXCL），
//   同名既有目录一律拒绝：重启的旧任务在启动时已被补写 interrupted 且永不重放，无需复用。
//   复制期间每个目标条目写入前都要再验一次「在副本根内 + 自身及祖先无重解析点」。
// ===========================================================================

using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace GalleriesManager.Wdb;

/// <summary>Worker 进程退出码（Job 自身的成败写在 status.json，不改变 Worker 退出码）。</summary>
internal static class WdbExitCode
{
    public const int Ok = 0;
    public const int Usage = 2;
    public const int NotWindows = 3;
    public const int AlreadyRunning = 4;
    public const int Internal = 5;

    /// <summary>运行根/宿主环境不合规（runs-root 非本桥所有、与 queue/工具目录重叠、UNC 等）。</summary>
    public const int EnvironmentViolation = 6;
}

/// <summary>带错误码的内部异常：错误码直接写进 status.json 的 Error 字段。</summary>
internal sealed class WdbException : Exception
{
    public WdbException(string code, string message, Exception? inner = null)
        : base(message, inner) => Code = code;

    public string Code { get; }
}

/// <summary>Worker 自身诊断输出：一律走 stderr（stdout 留给宿主，避免污染结构化输出）。</summary>
internal static class WdbLog
{
    private static readonly Lock Gate = new();

    public static void Info(string message) => Write("info", message);

    public static void Warn(string message) => Write("warn", message);

    public static void Error(string message) => Write("error", message);

    private static void Write(string level, string message)
    {
        var line = string.Create(
            CultureInfo.InvariantCulture,
            $"[wdb][{DateTimeOffset.UtcNow:yyyy-MM-ddTHH:mm:ss.fffZ}][{level}] {message}");
        lock (Gate)
        {
            Console.Error.WriteLine(line);
        }
    }
}

/// <summary>Worker 命令行参数。</summary>
internal sealed class WorkerOptions
{
    public const string DefaultDotnet = "dotnet";

    public string Queue { get; private init; } = string.Empty;

    public string RunsRoot { get; private init; } = string.Empty;

    public string DotnetPath { get; private init; } = DefaultDotnet;

    public bool AllowNonWindows { get; private init; }

    public bool ShowHelp { get; private init; }

    internal const string HelpText = """
        用法：wdb worker --queue <队列目录> --runs-root <本机运行根> [选项]

        必填：
          --queue <dir>        队列根目录（含 jobs/、worker.json、shutdown）。
          --runs-root <dir>    本机专属运行根；每个任务复制到 <runs-root>/<job-id>/src。

        可选：
          --dotnet <exe>       使用的 dotnet 可执行文件（默认 "dotnet"，按 PATH 解析）。
          --allow-non-windows  仅用于 fixture 测试：允许在非 Windows 宿主用普通 Process 启动
                               目标进程（无 Win32 Job Object 保护），并打印醒目告警。真实调试
                               必须在 Windows 宿主运行。
          -h, --help           显示本帮助。

        示例：
          wdb worker --queue \\wsl.localhost\<distro>\<user>\.wdb\queue ^
                     --runs-root %LOCALAPPDATA%\WslDebugBridge\runs
        """;

    public static WorkerOptions Parse(string[]? args)
    {
        args ??= [];
        string? queue = null;
        string? runsRoot = null;
        string? dotnet = null;
        var allowNonWindows = false;
        var help = false;

        // 允许把 `worker` 子命令原样带进来（start-worker.ps1 / README 均为 `wdb worker --queue ...`）。
        var i = args.Length > 0 && string.Equals(args[0], "worker", StringComparison.Ordinal) ? 1 : 0;

        for (; i < args.Length; i++)
        {
            var arg = args[i];
            if (TryTakeValue(arg, "--queue", args, ref i, out var queueValue))
            {
                queue = queueValue;
            }
            else if (TryTakeValue(arg, "--runs-root", args, ref i, out var runsValue))
            {
                runsRoot = runsValue;
            }
            else if (TryTakeValue(arg, "--dotnet", args, ref i, out var dotnetValue))
            {
                dotnet = dotnetValue;
            }
            else if (string.Equals(arg, "--allow-non-windows", StringComparison.Ordinal))
            {
                allowNonWindows = true;
            }
            else if (string.Equals(arg, "-h", StringComparison.Ordinal) || string.Equals(arg, "--help", StringComparison.Ordinal))
            {
                help = true;
            }
            else
            {
                throw new WdbException("ERR_USAGE", $"无法识别的参数：{arg}（用 --help 查看用法）");
            }
        }

        if (help)
        {
            return new WorkerOptions { ShowHelp = true };
        }

        if (string.IsNullOrWhiteSpace(queue))
        {
            throw new WdbException("ERR_USAGE", "缺少必填参数 --queue（队列根目录）。");
        }

        if (string.IsNullOrWhiteSpace(runsRoot))
        {
            throw new WdbException("ERR_USAGE", "缺少必填参数 --runs-root（本机运行根目录）。");
        }

        return new WorkerOptions
        {
            Queue = WdbPath.Full(queue),
            RunsRoot = WdbPath.Full(runsRoot),
            DotnetPath = NormalizeDotnetOption(dotnet),
            AllowNonWindows = allowNonWindows,
        };
    }

    /// <summary>
    /// --dotnet 归一化：裸可执行名（不含目录成分）保持原样、交给 PATH 查找；
    /// 含目录成分的相对路径一律按 <b>Worker 启动时的 CWD</b> 解析成绝对路径并校验存在 ——
    /// 目标进程的 CWD 是任务副本目录，绝不能按它去解析这个相对路径。
    /// 三处调用点（build / msbuild 探针 / dotnet &lt;dll&gt; 回退）都只使用这个已归一化的值。
    /// </summary>
    private static string NormalizeDotnetOption(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return DefaultDotnet;
        }

        var trimmed = value.Trim();
        if (Path.IsPathRooted(trimmed))
        {
            return trimmed;
        }

        if (string.IsNullOrEmpty(Path.GetDirectoryName(trimmed)))
        {
            return trimmed; // "dotnet" / "dotnet.exe"：仍是裸名字，走 PATH。
        }

        string resolved;
        try
        {
            resolved = Path.GetFullPath(trimmed);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            throw new WdbException("ERR_USAGE", $"--dotnet 不是合法路径：{trimmed}");
        }

        if (!File.Exists(resolved))
        {
            throw new WdbException(
                "ERR_USAGE",
                $"--dotnet 指向的文件不存在（相对路径已按 Worker 启动目录解析为绝对路径）：{trimmed} -> {resolved}");
        }

        return resolved;
    }

    private static bool TryTakeValue(string arg, string name, string[] args, ref int index, out string value)
    {
        if (string.Equals(arg, name, StringComparison.Ordinal))
        {
            if (index + 1 >= args.Length)
            {
                throw new WdbException("ERR_USAGE", $"{name} 缺少取值。");
            }

            value = args[++index];
            return true;
        }

        var prefix = name + "=";
        if (arg.StartsWith(prefix, StringComparison.Ordinal))
        {
            value = arg[prefix.Length..];
            return true;
        }

        value = string.Empty;
        return false;
    }
}

/// <summary>路径安全门禁：越界、重解析点、NTFS 保留名、裸相对路径一律在这里拦下。</summary>
internal static class WdbPath
{
    /// <summary>Windows 上路径比较大小写不敏感，其余平台按 Ordinal。</summary>
    public static StringComparison Comparison =>
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    private static readonly string[] ReservedDeviceNames =
    [
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    ];

    public static string Full(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new WdbException("ERR_USAGE", "路径不能为空。");
        }

        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
    }

    public static bool IsSameOrInside(string candidate, string root)
    {
        var c = Full(candidate);
        var r = Full(root);
        if (string.Equals(c, r, Comparison))
        {
            return true;
        }

        return c.StartsWith(r + Path.DirectorySeparatorChar, Comparison)
            || c.StartsWith(r + Path.AltDirectorySeparatorChar, Comparison);
    }

    /// <summary>任务 id 必须能安全地当作单层目录名（同时是 runs-root 下的目录名）。</summary>
    public static bool IsSafeJobId(string? id)
    {
        if (string.IsNullOrWhiteSpace(id) || id.Length > 96)
        {
            return false;
        }

        if (id is "." or "..")
        {
            return false;
        }

        foreach (var ch in id)
        {
            // 只允许 ASCII 字母数字与 _ - . ：排除分隔符、冒号（NTFS ADS）、通配符与空白。
            if (!(char.IsAsciiLetterOrDigit(ch) || ch is '_' or '-' or '.'))
            {
                return false;
            }
        }

        // NTFS 不接受以点或空格结尾的名字；保留设备名同样拒绝。
        if (id[^1] is '.' or ' ')
        {
            return false;
        }

        var stem = id.Split('.')[0].ToUpperInvariant();
        return Array.IndexOf(ReservedDeviceNames, stem) < 0;
    }

    /// <summary>把请求里的仓库相对路径解析到某个根之下，并拒绝任何逃逸。</summary>
    public static string ResolveInside(string root, string relative, string what)
    {
        if (string.IsNullOrWhiteSpace(relative))
        {
            throw new WdbException("ERR_BAD_REQUEST", $"{what} 不能为空。");
        }

        if (Path.IsPathRooted(relative) || relative.StartsWith('\\') || relative.StartsWith('/'))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"{what} 必须是相对路径，拒绝绝对路径：{relative}");
        }

        if (relative.Contains(':', StringComparison.Ordinal))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"{what} 含冒号（NTFS 备用数据流/盘符），拒绝：{relative}");
        }

        var segments = relative.Split(['/', '\\'], StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0)
        {
            throw new WdbException("ERR_BAD_REQUEST", $"{what} 未指向任何路径：{relative}");
        }

        foreach (var segment in segments)
        {
            if (segment == "..")
            {
                throw new WdbException("ERR_SECURITY_VIOLATION", $"{what} 含上跳段 \"..\"，拒绝：{relative}");
            }
        }

        var combined = Path.Combine([Full(root), .. segments]);
        var full = Full(combined);
        if (!IsSameOrInside(full, root))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"{what} 逃逸出既定根目录，拒绝：{relative}");
        }

        return full;
    }

    /// <summary>目录自身必须是常规目录（不是符号链接/junction）。</summary>
    public static void AssertNotReparse(string path, string what)
    {
        var full = Full(path);
        if (!Directory.Exists(full))
        {
            throw new WdbException("ERR_BAD_REQUEST", $"{what} 不存在或不是目录：{full}");
        }

        if (TryGetAttributes(full, out var attributes) && (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"{what} 是重解析点/符号链接，拒绝跨链接读写：{full}");
        }
    }

    /// <summary>逐级检查祖先目录（含自身）是否含重解析点/junction/符号链接。</summary>
    public static void AssertNoReparseAncestors(string path, string what)
    {
        var current = Full(path);
        while (!string.IsNullOrEmpty(current))
        {
            if ((Directory.Exists(current) || File.Exists(current))
                && TryGetAttributes(current, out var attributes)
                && (attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new WdbException(
                    "ERR_SECURITY_VIOLATION",
                    $"{what} 的路径含重解析点/符号链接（拒绝跨链接读写）：{current}");
            }

            var parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) || string.Equals(parent, current, StringComparison.Ordinal))
            {
                break;
            }

            current = Path.TrimEndingDirectorySeparator(parent);
        }
    }

    public static bool TryGetAttributes(string path, out FileAttributes attributes)
    {
        try
        {
            attributes = File.GetAttributes(path);
            return true;
        }
        catch (IOException)
        {
            attributes = default;
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            attributes = default;
            return false;
        }
    }
}

/// <summary>同目录 .tmp + Flush(disk) + rename：跨 9P/CIFS 也只让读者看到完整文件。</summary>
internal static class AtomicFile
{
    public static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: false);

    public static void WriteAllText(string path, string content)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var temp = path + ".tmp";
        var bytes = Utf8NoBom.GetBytes(content);
        using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            stream.Write(bytes, 0, bytes.Length);
            stream.Flush(flushToDisk: true);
        }

        File.Move(temp, path, overwrite: true);
    }

    public static void WriteJson<T>(string path, T value) => WriteAllText(path, BridgeJson.Serialize(value));

    /// <summary>稳态读：最多 3 次、每次间隔 50ms，避免把半可见文件误判成损坏。</summary>
    public static string? ReadAllTextSteady(string path)
    {
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                return File.Exists(path) ? File.ReadAllText(path, Utf8NoBom) : null;
            }
            catch (IOException)
            {
                // 正在被写入或跨系统暂不可见：退避后重试。
            }
            catch (UnauthorizedAccessException)
            {
                // 同上：不把权限瞬时问题当永久错误。
            }

            Thread.Sleep(50);
        }

        return null;
    }

    public static T? ReadJsonOrNull<T>(string path)
        where T : class
    {
        var text = ReadAllTextSteady(path);
        if (text is null)
        {
            return null;
        }

        try
        {
            return BridgeJson.Deserialize<T>(text);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>一次进程启动的完整描述（绝对可执行文件 + argv + 工作目录）。</summary>
internal sealed record JobLaunch(string FileName, IReadOnlyList<string> Arguments, string WorkingDirectory);

/// <summary>
/// 目标进程句柄。Windows 实现必须由 WindowsJobRunner 提供（挂起启动 → 并入 Job → Resume），
/// 非 Windows 只能用 <see cref="PlainProcessJob"/>（fixture 测试）。
/// </summary>
internal interface IJobProcess : IDisposable
{
    int Id { get; }

    int? ExitCode { get; }

    TextReader StandardOutput { get; }

    TextReader StandardError { get; }

    Task WaitForExitAsync(CancellationToken cancellationToken);

    /// <summary>
    /// 终止整棵进程树（Windows：TerminateJobObject / 关闭 Job 句柄触发 KillOnJobClose）。
    /// 要求幂等，且在目标进程已自然退出后调用也不得抛异常。
    /// </summary>
    void Kill();
}

/// <summary>
/// 进程启动分派：Windows 只认 WindowsJobRunner 注册的工厂（挂起启动 + Job 接管），
/// 其余平台默认拒绝，仅 fixture 测试可显式降级为普通 Process。
/// </summary>
internal static class JobRunnerGateway
{
    /// <summary>
    /// Windows 侧工厂，由 WindowsJobRunner.cs 的 [ModuleInitializer]（WindowsJobRunnerRegistration）注册，
    /// 其返回值是 WindowsProcessJob（CREATE_SUSPENDED → AssignProcessToJobObject → ResumeThread）。
    /// 恒为 null 说明该文件未参与编译 —— 此时必须熔断，绝不退回普通 Process。
    /// </summary>
    public static Func<JobLaunch, IJobProcess>? WindowsProcessFactory { get; set; }

    public static IJobProcess Start(JobLaunch launch, bool allowNonWindows)
    {
        if (OperatingSystem.IsWindows())
        {
            var factory = WindowsProcessFactory;
            if (factory is null)
            {
                throw new WdbException(
                    "ERR_JOB_RUNNER_UNAVAILABLE",
                    "Windows 目标进程必须由 WindowsJobRunner（CreateProcessW 挂起启动 + Job Object "
                    + "KillOnJobClose）接管，但该 runner 未注册；拒绝以普通 Process 降级启动"
                    + "（会产生脱离 Job 的孤儿进程）。");
            }

            try
            {
                return factory(launch);
            }
            catch (PlatformNotSupportedException ex)
            {
                throw new WdbException("ERR_JOB_RUNNER_UNAVAILABLE", $"WindowsJobRunner 不可用：{ex.Message}", ex);
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                throw new WdbException(
                    "ERR_START_FAILED",
                    $"CreateProcessW 失败（Win32 {ex.NativeErrorCode}）：{ex.Message}",
                    ex);
            }
            catch (Exception ex) when (ex is InvalidOperationException or NotSupportedException)
            {
                throw new WdbException("ERR_START_FAILED", $"启动参数非法：{ex.Message}", ex);
            }
        }

        if (!allowNonWindows)
        {
            throw new WdbException(
                "ERR_NOT_WINDOWS",
                "当前宿主不是 Windows：拒绝启动目标进程。仅 fixture 测试可显式使用 --allow-non-windows。");
        }

        return PlainProcessJob.Start(launch);
    }

    /// <summary>
    /// 构造子进程启动信息：argv 只经 ArgumentList（绝不拼命令行、绝不 shell、绝不 shell 转义）。
    /// </summary>
    public static ProcessStartInfo BuildStartInfo(JobLaunch launch, bool redirectStandardInput)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = launch.FileName,
            WorkingDirectory = launch.WorkingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = redirectStandardInput,
            CreateNoWindow = true,
            StandardOutputEncoding = AtomicFile.Utf8NoBom,
            StandardErrorEncoding = AtomicFile.Utf8NoBom,
        };

        foreach (var argument in launch.Arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        WorkerHost.ApplyChildEnvironment(startInfo.Environment);
        return startInfo;
    }
}

/// <summary>非 Windows 测试用实现：普通 Process + 整树 Kill。没有任何 Job Object 保护。</summary>
internal sealed class PlainProcessJob : IJobProcess
{
    private readonly Process _process;
    private readonly StreamReader _stdout;
    private readonly StreamReader _stderr;

    private PlainProcessJob(Process process)
    {
        _process = process;
        _stdout = process.StandardOutput;
        _stderr = process.StandardError;
    }

    public int Id => _process.Id;

    public int? ExitCode
    {
        get
        {
            try
            {
                return _process.HasExited ? _process.ExitCode : null;
            }
            catch (InvalidOperationException)
            {
                return null;
            }
        }
    }

    public TextReader StandardOutput => _stdout;

    public TextReader StandardError => _stderr;

    public static IJobProcess Start(JobLaunch launch)
    {
        var startInfo = JobRunnerGateway.BuildStartInfo(launch, redirectStandardInput: true);
        var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            process.Dispose();
            throw new WdbException("ERR_START_FAILED", $"无法启动进程：{launch.FileName}");
        }

        try
        {
            // 子进程不该读前台控制台：立刻关闭 stdin（等效 NUL）。
            process.StandardInput.Close();
        }
        catch (IOException)
        {
            // 忽略：进程可能已退出。
        }

        return new PlainProcessJob(process);
    }

    public Task WaitForExitAsync(CancellationToken cancellationToken) =>
        _process.WaitForExitAsync(cancellationToken);

    public void Kill()
    {
        try
        {
            if (!_process.HasExited)
            {
                _process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
            // 已退出：幂等。
        }
        catch (NotSupportedException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }

    public void Dispose() => _process.Dispose();
}

/// <summary>当前任务的停止原因：决定终态写 canceled / timeout / interrupted。</summary>
internal sealed class CancelCause
{
    private string? _reason;

    public string? Reason => Volatile.Read(ref _reason);

    public bool IsCanceled => Reason is not null;

    public void Set(string reason) => Interlocked.CompareExchange(ref _reason, reason, null);

    public string TerminalState => Reason switch
    {
        "timeout" => JobStates.Timeout,
        "shutdown" => JobStates.Interrupted,
        _ => JobStates.Canceled,
    };
}

/// <summary>一次进程执行的结局。<see cref="StreamsDrained"/> 为 false 表示日志流未在限时内排空，
/// 此时输出可能仍被写入，调用方不得基于捕获内容做判定。</summary>
internal sealed record ProcessOutcome(int? ExitCode, bool Canceled, bool StreamsDrained);

/// <summary>
/// Worker 宿主：独占锁 → 心跳/停机监视 → 启动恢复 → 串行任务循环。
/// </summary>
internal sealed class WorkerHost : IDisposable
{
    private const int PollIntervalMs = 250;
    private const int HeartbeatIntervalMs = 1000;
    private const int StreamDrainGraceMs = 5000;
    private const int ExitWaitGraceSeconds = 15;
    private const string RunOwnerMarkerName = ".wdb-run-owner";
    private const string RunOwnerToken = "wdb-run/v1";

    /// <summary>
    /// runs-root 所有权标记（名字与 token 与 start-worker.ps1 的建议一致，便于两侧对齐）：
    /// 首行为 token 才视为本桥所有；非空且无标记/标记不符一律拒绝写入。
    /// </summary>
    private const string RunsOwnerMarkerName = ".wsl-debug-bridge-runs-owner";
    private const string RunsOwnerToken = "wsl-debug-bridge/runs-root/v1";

    private const string ClaimMarkerName = ".wdb-claim";

    private readonly WorkerOptions _options;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly string _queueRoot;
    private readonly string _jobsDir;
    private readonly string _runsRoot;
    private readonly string _workerId = Convert.ToHexString(RandomNumberGenerator.GetBytes(8)).ToLowerInvariant();
    private readonly DateTimeOffset _startedUtc = DateTimeOffset.UtcNow;
    private readonly string _version = typeof(Worker).Assembly.GetName().Version?.ToString() ?? "0.0.0";

    private FileStream? _lockStream;
    private Mutex? _queueMutex;
    private bool _mutexOwned;
    private DateTime? _shutdownBaselineUtc;
    private string? _currentJobId;
    private PosixSignalRegistration? _sigtermRegistration;
    private bool _disposed;

    public WorkerHost(WorkerOptions options)
    {
        _options = options;
        _queueRoot = options.Queue;
        _jobsDir = BridgeLayout.JobsDir(_queueRoot);
        _runsRoot = options.RunsRoot;
    }

    public async Task<int> RunAsync()
    {
        if (!Directory.Exists(_queueRoot))
        {
            WdbLog.Error($"队列根目录不存在：{_queueRoot}");
            return WdbExitCode.Usage;
        }

        if (!TryAcquireExclusiveLock(out var lockReason))
        {
            WdbLog.Error(lockReason);
            return WdbExitCode.AlreadyRunning;
        }

        try
        {
            PrepareDirectories();

            // shutdown 用「基线比对」而不是时钟比较（跨系统时钟不可信）：
            // 启动时已存在的 shutdown 视为上一轮遗留，仅当它被重新创建时才停机。
            _shutdownBaselineUtc = ReadMarkerTimestamp(BridgeLayout.ShutdownPath(_queueRoot));
            if (_shutdownBaselineUtc is not null)
            {
                WdbLog.Warn(
                    "启动时检测到残留 shutdown 标记；按基线比对只消费本次启动后新建的标记"
                    + "（不删除任何文件，需要立即停机请重新创建该标记）。");
            }

            InstallSignalHandlers();

            WdbLog.Info($"Worker 启动：id={_workerId} pid={Environment.ProcessId} queue={_queueRoot} runs-root={_runsRoot}");
            if (!OperatingSystem.IsWindows())
            {
                WdbLog.Warn(
                    "--allow-non-windows 已启用：目标进程使用普通 Process 启动，没有 Win32 Job Object 保护。"
                    + "仅限 fixture 测试，禁止用于真实调试。");
            }

            using var heartbeatStop = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
            var heartbeatTask = HeartbeatLoopAsync(heartbeatStop.Token);
            var shutdownTask = ShutdownWatchLoopAsync(heartbeatStop.Token);

            try
            {
                RecoverStaleJobs();
                await MainLoopAsync().ConfigureAwait(false);
            }
            finally
            {
                heartbeatStop.Cancel();
                await AwaitQuietlyAsync([heartbeatTask, shutdownTask], "心跳/停机监视").ConfigureAwait(false);
                WriteHeartbeat();
                WdbLog.Info("Worker 已停止。");
            }

            return WdbExitCode.Ok;
        }
        finally
        {
            ReleaseExclusiveLock();
        }
    }

    // ---------------------------------------------------------------- 启动准备

    private void PrepareDirectories()
    {
        // 顺序固定：先只读校验（此时仍可拒绝启动），确认所有权后才创建/接管，最后才写任何东西。
        WdbPath.AssertNoReparseAncestors(_runsRoot, "runs-root");
        AssertSafeRunsRoot(_runsRoot);
        AssertRunsRootNotOverlapping(_runsRoot, _queueRoot);

        var ownership = EnsureRunsRootOwnership(_runsRoot);
        Directory.CreateDirectory(_jobsDir);
        WdbLog.Info($"runs-root 就绪（{ownership}）：{_runsRoot}");
    }

    /// <summary>
    /// runs-root 所有权：不存在 → 新建；空目录 → 接管；带匹配标记 → 复用；
    /// 已存在、非空且无标记（或标记不符）→ 拒绝。绝不把副本写进用户的既有目录，也绝不删除任何文件。
    /// </summary>
    private static string EnsureRunsRootOwnership(string runsRoot)
    {
        var markerPath = Path.Combine(runsRoot, RunsOwnerMarkerName);

        if (!Directory.Exists(runsRoot))
        {
            Directory.CreateDirectory(runsRoot);
            WriteRunsOwnerMarker(markerPath);
            return "新建";
        }

        WdbPath.AssertNotReparse(runsRoot, "runs-root");

        var existing = AtomicFile.ReadAllTextSteady(markerPath);
        if (existing is not null)
        {
            if (!FirstLineEquals(existing, RunsOwnerToken))
            {
                throw new WdbException(
                    "ERR_RUNS_ROOT_NOT_OWNED",
                    $"runs-root 的所有权标记 {RunsOwnerMarkerName} 内容不符"
                    + $"（期望首行 \"{RunsOwnerToken}\"）：{runsRoot}");
            }

            return "复用（标记匹配）";
        }

        if (Directory.EnumerateFileSystemEntries(runsRoot).Any())
        {
            throw new WdbException(
                "ERR_RUNS_ROOT_NOT_OWNED",
                $"runs-root 已存在且非空，但缺少所有权标记 {RunsOwnerMarkerName}，拒绝写入：{runsRoot}。"
                + "请改用空目录或本桥专用目录（本桥绝不删除既有文件）。");
        }

        WriteRunsOwnerMarker(markerPath);
        return "接管（空目录）";
    }

    private static void WriteRunsOwnerMarker(string markerPath)
        => AtomicFile.WriteAllText(
            markerPath,
            $"{RunsOwnerToken}\ncreatedUtc={DateTimeOffset.UtcNow:O}\npid={Environment.ProcessId}\n");

    private static bool FirstLineEquals(string text, string expected)
    {
        foreach (var line in text.Split(['\r', '\n']))
        {
            var trimmed = line.Trim().TrimStart('\uFEFF').Trim();
            if (trimmed.Length == 0)
            {
                continue;
            }

            return string.Equals(trimmed, expected, StringComparison.Ordinal);
        }

        return false;
    }

    /// <summary>runs-root 与 queue 必须双向不重叠：相等、或互为祖先/后代都拒绝。</summary>
    private static void AssertRunsRootNotOverlapping(string runsRoot, string queueRoot)
    {
        if (WdbPath.IsSameOrInside(runsRoot, queueRoot) || WdbPath.IsSameOrInside(queueRoot, runsRoot))
        {
            throw new WdbException(
                "ERR_SECURITY_VIOLATION",
                $"runs-root 与 queue 不得相等或互为祖先/后代：runs-root {runsRoot}，queue {queueRoot}");
        }
    }

    /// <summary>拒绝把运行根放在盘符根、UNC、系统目录、用户主目录，或与工具目录重叠。</summary>
    private static void AssertSafeRunsRoot(string runsRoot)
    {
        var full = WdbPath.Full(runsRoot);
        if (string.IsNullOrEmpty(Path.GetDirectoryName(full)))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"runs-root 不能是文件系统根目录：{full}");
        }

        // 副本必须落在本机盘上：UNC/网络路径既慢又会让 FileShare/rename 语义失效。
        if (OperatingSystem.IsWindows()
            && (full.StartsWith(@"\\", StringComparison.Ordinal) || full.StartsWith("//", StringComparison.Ordinal)))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"runs-root 必须是本机盘符绝对路径，不接受 UNC：{full}");
        }

        // 工具目录（AppContext.BaseDirectory）双向不重叠：否则构建产物会写进桥自身。
        var toolDir = AppContext.BaseDirectory;
        if (!string.IsNullOrWhiteSpace(toolDir))
        {
            var toolFull = WdbPath.Full(toolDir);
            if (WdbPath.IsSameOrInside(full, toolFull) || WdbPath.IsSameOrInside(toolFull, full))
            {
                throw new WdbException(
                    "ERR_SECURITY_VIOLATION",
                    $"runs-root 不得与工具目录相等或互为祖先/后代：runs-root {full}，工具目录 {toolFull}");
            }
        }

        foreach (var systemDir in new[]
                 {
                     Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                     Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                     Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                     Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                 })
        {
            if (!string.IsNullOrEmpty(systemDir) && WdbPath.IsSameOrInside(full, systemDir))
            {
                throw new WdbException("ERR_SECURITY_VIOLATION", $"runs-root 不能位于系统目录内：{full}");
            }
        }

        var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (!string.IsNullOrEmpty(userProfile) && string.Equals(full, WdbPath.Full(userProfile), WdbPath.Comparison))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"runs-root 不能直接使用用户主目录：{full}");
        }
    }

    /// <summary>
    /// 独占锁：本地排他文件句柄（Windows 由 OS 强制共享模式；Unix 由 .NET 映射为 flock 排他锁）
    /// + Windows 命名互斥体（UNC/CIFS 上 FileShare 语义不可靠时的兜底）。两者都必须拿到。
    /// 锁文件只写诊断信息；本文件从不删除它（绝不删除任何文件）。
    /// </summary>
    private bool TryAcquireExclusiveLock(out string reason)
    {
        reason = string.Empty;
        var lockPath = Path.Combine(_queueRoot, "worker.lock");

        try
        {
            // FileShare.None：跨平台都能真正排他（FileShare.Read 在 Unix 上只是共享锁，不排他）。
            _lockStream = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException ex)
        {
            reason = $"队列已被另一个 Worker 独占（锁文件无法排他打开）：{lockPath} —— {ex.Message}";
            return false;
        }
        catch (UnauthorizedAccessException ex)
        {
            reason = $"无法打开 Worker 锁文件：{lockPath} —— {ex.Message}";
            return false;
        }

        try
        {
            var bytes = AtomicFile.Utf8NoBom.GetBytes(
                $"{{\"pid\":{Environment.ProcessId.ToString(CultureInfo.InvariantCulture)},"
                + $"\"workerId\":\"{_workerId}\",\"startedUtc\":\"{_startedUtc:O}\"}}\n");
            _lockStream.SetLength(0);
            _lockStream.Write(bytes, 0, bytes.Length);
            _lockStream.Flush(flushToDisk: true);
        }
        catch (IOException)
        {
            // 锁文件内容只是诊断信息，写不进去不影响互斥本身。
        }

        if (OperatingSystem.IsWindows() && !TryAcquireNamedMutex(out reason))
        {
            return false;
        }

        return true;
    }

    private bool TryAcquireNamedMutex(out string reason)
    {
        reason = string.Empty;
        var hash = Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(_queueRoot.ToLowerInvariant())))[..32];

        foreach (var prefix in new[] { @"Global\", @"Local\" })
        {
            Mutex? mutex = null;
            try
            {
                mutex = new Mutex(initiallyOwned: false, prefix + "wdb-worker-" + hash);
                bool acquired;
                try
                {
                    acquired = mutex.WaitOne(0);
                }
                catch (AbandonedMutexException)
                {
                    acquired = true;
                }

                if (!acquired)
                {
                    mutex.Dispose();
                    reason = $"队列已被另一个 Worker 独占（命名互斥体 {prefix}wdb-worker-{hash} 已被占用）。";
                    return false;
                }

                _queueMutex = mutex;
                _mutexOwned = true;
                return true;
            }
            catch (UnauthorizedAccessException)
            {
                mutex?.Dispose();
            }
            catch (IOException)
            {
                mutex?.Dispose();
            }
            catch (WaitHandleCannotBeOpenedException)
            {
                mutex?.Dispose();
            }
        }

        reason = "无法创建或获取 Windows 命名互斥体（Global 与 Local 均失败），拒绝启动以避免双 Worker。";
        return false;
    }

    private void ReleaseExclusiveLock()
    {
        if (_mutexOwned && _queueMutex is not null)
        {
            try
            {
                _queueMutex.ReleaseMutex();
            }
            catch (ApplicationException)
            {
                // 未持有：忽略。
            }
        }

        _queueMutex?.Dispose();
        _queueMutex = null;
        _mutexOwned = false;

        // 只释放句柄，不删除锁文件（绝不删除宿主任意文件）。
        _lockStream?.Dispose();
        _lockStream = null;
    }

    private void InstallSignalHandlers()
    {
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            WdbLog.Info("收到 Ctrl+C，开始停机（当前任务将被终止）。");
            _shutdown.Cancel();
        };

        try
        {
            _sigtermRegistration = PosixSignalRegistration.Create(PosixSignal.SIGTERM, context =>
            {
                context.Cancel = true;
                WdbLog.Info("收到 SIGTERM，开始停机。");
                _shutdown.Cancel();
            });
        }
        catch (Exception ex) when (ex is PlatformNotSupportedException or IOException)
        {
            // 平台不支持：忽略（Ctrl+C 仍然有效）。
        }
    }

    // ---------------------------------------------------------------- 心跳 / 停机

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            WriteHeartbeat();
            try
            {
                await Task.Delay(HeartbeatIntervalMs, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
    }

    private void WriteHeartbeat()
    {
        try
        {
            var state = new WorkerState(
                BridgeProtocol.Version,
                _workerId,
                Environment.ProcessId,
                SafeHostName(),
                _queueRoot,
                _runsRoot,
                _version,
                _startedUtc,
                DateTimeOffset.UtcNow,
                Volatile.Read(ref _currentJobId));

            AtomicFile.WriteJson(BridgeLayout.WorkerPath(_queueRoot), state);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            WdbLog.Warn($"写 worker.json 失败（不致命）：{ex.Message}");
        }
    }

    private async Task ShutdownWatchLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var marker = ReadMarkerTimestamp(BridgeLayout.ShutdownPath(_queueRoot));
                if (marker is not null && marker != _shutdownBaselineUtc)
                {
                    WdbLog.Info("检测到 shutdown 标记，开始停机。");
                    _shutdown.Cancel();
                    return;
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                WdbLog.Warn($"检查 shutdown 标记失败：{ex.Message}");
            }

            try
            {
                await Task.Delay(PollIntervalMs, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    private static DateTime? ReadMarkerTimestamp(string path)
    {
        try
        {
            var info = new FileInfo(path);
            return info.Exists ? info.LastWriteTimeUtc : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string SafeHostName()
    {
        try
        {
            return Environment.MachineName;
        }
        catch (InvalidOperationException)
        {
            return "unknown";
        }
    }

    // ---------------------------------------------------------------- 启动恢复

    /// <summary>
    /// 重启恢复：已认领但未终态的任务一律写 interrupted，绝不重放、绝不按 PID 杀进程；
    /// 从未被认领、也无状态的任务留给主循环正常执行。
    /// </summary>
    private void RecoverStaleJobs()
    {
        foreach (var jobDir in EnumerateJobDirs())
        {
            var id = Path.GetFileName(jobDir);
            if (!WdbPath.IsSafeJobId(id))
            {
                WdbLog.Warn($"忽略非法任务目录名：{id}");
                continue;
            }

            if (!File.Exists(BridgeLayout.RequestPath(_queueRoot, id)))
            {
                // 提交未完成（request.json 尚未出现）：不是任务，不碰。
                continue;
            }

            var status = AtomicFile.ReadJsonOrNull<Status>(BridgeLayout.StatusPath(_queueRoot, id));
            if (status is not null && JobStates.IsTerminal(status.State))
            {
                // 结果已存在：忽略重复请求。
                continue;
            }

            var claimed = status is not null || File.Exists(Path.Combine(jobDir, ClaimMarkerName));
            if (!claimed)
            {
                continue;
            }

            var mode = status?.Mode
                ?? AtomicFile.ReadJsonOrNull<Request>(BridgeLayout.RequestPath(_queueRoot, id))?.Mode
                ?? string.Empty;

            WriteStatus(
                id,
                mode,
                JobStates.Interrupted,
                JobPhases.Done,
                error: "Worker 重启时发现该任务处于未终态：按合同不重放、不按 PID 杀进程，标记为 interrupted。",
                startedUtc: status?.StartedUtc,
                finishedUtc: DateTimeOffset.UtcNow);

            WdbLog.Warn($"[{id}] 上次执行未终态 → 标记 interrupted（不重放）。");
        }
    }

    private IEnumerable<string> EnumerateJobDirs()
    {
        string[] directories;
        try
        {
            directories = Directory.GetDirectories(_jobsDir);
        }
        catch (DirectoryNotFoundException)
        {
            yield break;
        }
        catch (IOException ex)
        {
            WdbLog.Warn($"枚举任务目录失败：{ex.Message}");
            yield break;
        }

        Array.Sort(directories, StringComparer.Ordinal);
        foreach (var directory in directories)
        {
            yield return directory;
        }
    }

    // ---------------------------------------------------------------- 主循环

    private async Task MainLoopAsync()
    {
        while (!_shutdown.IsCancellationRequested)
        {
            var next = FindNextRunnableJob();
            if (next is null)
            {
                try
                {
                    await Task.Delay(PollIntervalMs, _shutdown.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    break;
                }

                continue;
            }

            await ExecuteJobAsync(next).ConfigureAwait(false);
        }
    }

    private string? FindNextRunnableJob()
    {
        foreach (var jobDir in EnumerateJobDirs())
        {
            var id = Path.GetFileName(jobDir);
            if (!WdbPath.IsSafeJobId(id))
            {
                continue;
            }

            if (!File.Exists(BridgeLayout.RequestPath(_queueRoot, id)))
            {
                // request.json 是提交完成标记：尚未出现即尚未提交。
                continue;
            }

            var status = AtomicFile.ReadJsonOrNull<Status>(BridgeLayout.StatusPath(_queueRoot, id));
            if (status is not null && JobStates.IsTerminal(status.State))
            {
                continue; // 已有结果：忽略重复请求。
            }

            if (status is not null || File.Exists(Path.Combine(jobDir, ClaimMarkerName)))
            {
                continue; // 已被认领过：绝不重放（启动时已补写 interrupted）。
            }

            return id;
        }

        return null;
    }

    // ---------------------------------------------------------------- 单任务执行

    private async Task ExecuteJobAsync(string id)
    {
        Volatile.Write(ref _currentJobId, id);
        var cause = new CancelCause();
        var mode = string.Empty;

        try
        {
            var request = AtomicFile.ReadJsonOrNull<Request>(BridgeLayout.RequestPath(_queueRoot, id));
            if (request is null)
            {
                // request.json 是原子提交的最后一个文件：读不出来即视为无效请求。
                WriteStatus(id, string.Empty, JobStates.Rejected, JobPhases.Done,
                    error: "request.json 无法解析（缺失或不是合法 JSON）。", finishedUtc: DateTimeOffset.UtcNow);
                WdbLog.Warn($"[{id}] 拒绝：request.json 无法解析。");
                return;
            }

            mode = request.Mode;

            if (!Validate(request, out var rejection))
            {
                WriteStatus(id, request.Mode, JobStates.Rejected, JobPhases.Done,
                    error: rejection, finishedUtc: DateTimeOffset.UtcNow);
                WdbLog.Warn($"[{id}] 拒绝：{rejection}");
                return;
            }

            // 认领：在任何副作用之前留下标记，保证「重启不重放」有据可依。
            AtomicFile.WriteAllText(
                Path.Combine(BridgeLayout.JobDir(_queueRoot, id), ClaimMarkerName),
                $"workerId={_workerId}\npid={Environment.ProcessId}\nclaimedUtc={DateTimeOffset.UtcNow:O}\n");

            var status = new JobStatusWriter(this, id, request.Mode, DateTimeOffset.UtcNow);
            status.Update(JobStates.Queued, JobPhases.Submitted);

            using var jobCts = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
            using var shutdownReg = _shutdown.Token.Register(
                static state => ((CancelCause)state!).Set("shutdown"), cause);

            // cancel 轮询（250ms）与超时都独立于目标进程，保证控制面不被长构建饿死。
            using var watchCts = new CancellationTokenSource();
            var cancelWatchTask = CancelWatchAsync(id, jobCts, watchCts.Token, cause);
            var timeoutWatchTask = TimeoutWatchAsync(request, jobCts, watchCts.Token, cause);

            try
            {
                await RunJobBodyAsync(request, cause, jobCts, status).ConfigureAwait(false);
            }
            finally
            {
                watchCts.Cancel();
                await AwaitQuietlyAsync([cancelWatchTask, timeoutWatchTask], $"[{id}] 信号监视").ConfigureAwait(false);
            }
        }
        catch (WdbException ex)
        {
            // 执行前的拒绝（请求/路径/归属不合规）记 rejected；真正开始执行后的失败记 failed。
            var state = IsRejectionCode(ex.Code) ? JobStates.Rejected : JobStates.Failed;
            WriteStatus(id, mode, state, JobPhases.Done, error: $"{ex.Code}: {ex.Message}",
                finishedUtc: DateTimeOffset.UtcNow);
            WdbLog.Error($"[{id}] {(state == JobStates.Rejected ? "拒绝" : "失败")}：{ex.Code}: {ex.Message}");
        }
        catch (Exception ex)
        {
            // 逐任务异常一律收敛为 failed，绝不拖垮 Worker。
            WriteStatus(id, mode, JobStates.Failed, JobPhases.Done,
                error: $"{ex.GetType().Name}: {ex.Message}", finishedUtc: DateTimeOffset.UtcNow);
            WdbLog.Error($"[{id}] 未预期异常：{ex}");
        }
        finally
        {
            Volatile.Write(ref _currentJobId, null);
        }
    }

    private static bool IsRejectionCode(string code) => code is
        "ERR_BAD_REQUEST" or "ERR_SECURITY_VIOLATION"
        or "ERR_RUN_DIR_EXISTS" or "ERR_RUN_DIR_CLAIMED" or "ERR_PAYLOAD_MISSING";

    private static bool Validate(Request request, out string rejection)
    {
        rejection = string.Empty;

        if (request.ProtocolVersion != BridgeProtocol.Version)
        {
            rejection = $"协议版本不匹配：收到 {request.ProtocolVersion}，本 Worker 支持 {BridgeProtocol.Version}。";
            return false;
        }

        if (!WdbPath.IsSafeJobId(request.Id))
        {
            rejection = $"任务 id 不安全：{request.Id}";
            return false;
        }

        if (!JobModes.IsKnown(request.Mode))
        {
            rejection = $"未知 mode：{request.Mode}";
            return false;
        }

        if (string.IsNullOrWhiteSpace(request.Project))
        {
            rejection = "project 为空。";
            return false;
        }

        if (request.Payload is null)
        {
            rejection = "payload 引用缺失。";
            return false;
        }

        var declared = request.Payload.RelativePath;
        if (!string.IsNullOrWhiteSpace(declared)
            && !string.Equals(declared.Trim(), BridgeLayout.PayloadDirName, StringComparison.Ordinal))
        {
            rejection = $"payload.relativePath 必须是 \"{BridgeLayout.PayloadDirName}\"：{declared}";
            return false;
        }

        return true;
    }

    /// <summary>单任务 status.json 的可变视图：pid/artifact 随执行推进，每次写入都刷新 UpdatedUtc。</summary>
    private sealed class JobStatusWriter(WorkerHost host, string id, string mode, DateTimeOffset startedUtc)
    {
        public int? Pid { get; set; }

        public string? Artifact { get; set; }

        public void Update(string state, string? phase, int? exitCode = null, string? error = null)
            => host.WriteStatus(id, mode, state, phase, exitCode, Pid, error, Artifact, startedUtc, finishedUtc: null);

        public void Finish(string state, int? exitCode, string? error)
            => host.WriteStatus(id, mode, state, JobPhases.Done, exitCode, Pid, error, Artifact, startedUtc,
                DateTimeOffset.UtcNow);
    }

    private async Task RunJobBodyAsync(
        Request request,
        CancelCause cause,
        CancellationTokenSource jobCts,
        JobStatusWriter status)
    {
        var id = request.Id;
        var cancellationToken = jobCts.Token;

        if (File.Exists(BridgeLayout.CancelPath(_queueRoot, id)))
        {
            cause.Set("cancel");
            status.Finish(cause.TerminalState, null, "任务在开始执行前已收到 cancel 标记。");
            return;
        }

        // --- 1) payload → <runs-root>/<id>/src ----------------------------------
        status.Update(JobStates.Queued, JobPhases.Copying);
        var runDir = EnsureRunDirectory(id);
        var sourceDir = Path.Combine(runDir, "src");
        CopyPayload(id, runDir, sourceDir);

        // 仓库相对路径必须在副本根之内解析：显式拒绝 ../ 逃逸、绝对路径与 ADS。
        var projectPath = WdbPath.ResolveInside(sourceDir, NormalizeProjectPath(request.Project), "project");
        if (!Path.GetExtension(projectPath).Equals(".csproj", StringComparison.OrdinalIgnoreCase))
        {
            throw new WdbException("ERR_BAD_REQUEST", $"project 必须以 .csproj 结尾：{request.Project}");
        }

        if (!File.Exists(projectPath))
        {
            throw new WdbException("ERR_BAD_REQUEST", $"project 不在 payload 快照中：{request.Project}");
        }

        // --- 2) 构建 ------------------------------------------------------------
        if (request.Mode is JobModes.Build or JobModes.BuildRun)
        {
            status.Update(JobStates.Running, JobPhases.Building);

            var outcome = await RunProcessAsync(
                id, _options.DotnetPath, BuildArguments(request, projectPath), sourceDir,
                cancellationToken, cause, pid => status.Pid = pid, captureStdout: null).ConfigureAwait(false);

            if (outcome.Canceled)
            {
                status.Finish(cause.TerminalState, outcome.ExitCode, $"任务在构建阶段被停止（{cause.Reason ?? "cancel"}）。");
                return;
            }

            if (outcome.ExitCode != 0)
            {
                status.Finish(JobStates.Failed, outcome.ExitCode,
                    $"构建失败：dotnet build 退出码 {outcome.ExitCode?.ToString(CultureInfo.InvariantCulture) ?? "(未知)"}。");
                return;
            }
        }

        if (request.Mode == JobModes.Build)
        {
            status.Finish(JobStates.Succeeded, 0, null);
            return;
        }

        // --- 3) 求值可执行体 ----------------------------------------------------
        string fileName;
        string[] arguments;

        if (request.Mode == JobModes.BuildRun)
        {
            status.Update(JobStates.Running, JobPhases.Starting);

            var probe = new StreamCapture();
            var probeOutcome = await RunProcessAsync(
                id, _options.DotnetPath, ArtifactProbeArguments(request, projectPath), sourceDir,
                cancellationToken, cause, onStarted: null, captureStdout: probe).ConfigureAwait(false);

            if (probeOutcome.Canceled)
            {
                status.Finish(cause.TerminalState, probeOutcome.ExitCode, $"任务在求值产物阶段被停止（{cause.Reason ?? "cancel"}）。");
                return;
            }

            if (probeOutcome.ExitCode != 0)
            {
                status.Finish(JobStates.Failed, probeOutcome.ExitCode, "求值构建产物失败：dotnet msbuild -getProperty:TargetPath 退出码非 0。");
                return;
            }

            if (!probeOutcome.StreamsDrained)
            {
                // 排空超时：缓冲可能仍被追加，绝不基于可能不完整的输出判定产物。
                status.Finish(JobStates.Failed, probeOutcome.ExitCode,
                    "求值构建产物时日志流未在限时内排空，拒绝基于可能不完整的输出判定产物。");
                return;
            }

            var targetPath = LastArtifactLine(probe.Snapshot());
            if (string.IsNullOrWhiteSpace(targetPath))
            {
                status.Finish(JobStates.Failed, null,
                    "ERR_FRAMEWORK_REQUIRED: 无法求值 TargetPath（多 TFM 项目必须显式指定 framework）。");
                return;
            }

            if (!File.Exists(targetPath))
            {
                status.Finish(JobStates.Failed, null, $"求值得到的构建产物不存在：{targetPath}");
                return;
            }

            status.Artifact = targetPath;

            // 裁决：apphost .exe 直接运行；否则用 dotnet <dll>。绝不退回 `run --no-build` 冒充。
            var appHost = Path.GetExtension(targetPath).Equals(".dll", StringComparison.OrdinalIgnoreCase)
                ? Path.ChangeExtension(targetPath, ".exe")
                : null;

            if (appHost is not null && File.Exists(appHost))
            {
                fileName = appHost;
                arguments = request.Arguments ?? [];
            }
            else if (Path.GetExtension(targetPath).Equals(".exe", StringComparison.OrdinalIgnoreCase))
            {
                fileName = targetPath;
                arguments = request.Arguments ?? [];
            }
            else
            {
                fileName = ResolveDotnetExecutable();
                arguments = [targetPath, .. request.Arguments ?? []];
            }
        }
        else
        {
            fileName = ResolveDotnetExecutable();
            arguments = RunArguments(request, projectPath);
        }

        // --- 4) 启动并运行 ------------------------------------------------------
        status.Update(JobStates.Running, JobPhases.Starting);
        var runOutcome = await RunProcessAsync(
            id, fileName, arguments, sourceDir, cancellationToken, cause, pid => status.Pid = pid, captureStdout: null).ConfigureAwait(false);

        if (runOutcome.Canceled)
        {
            status.Finish(cause.TerminalState, runOutcome.ExitCode, $"任务被停止（{cause.Reason ?? "cancel"}）。");
            return;
        }

        if (runOutcome.ExitCode == 0)
        {
            status.Finish(JobStates.Succeeded, 0, null);
        }
        else
        {
            status.Finish(JobStates.Failed, runOutcome.ExitCode,
                $"目标进程退出码 {runOutcome.ExitCode?.ToString(CultureInfo.InvariantCulture) ?? "(未知)"}。");
        }
    }

    // ---------------------------------------------------------------- 信号监视

    private async Task CancelWatchAsync(string id, CancellationTokenSource jobCts, CancellationToken watchToken, CancelCause cause)
    {
        var cancelPath = BridgeLayout.CancelPath(_queueRoot, id);
        while (!watchToken.IsCancellationRequested && !jobCts.IsCancellationRequested)
        {
            try
            {
                if (File.Exists(cancelPath))
                {
                    cause.Set("cancel");
                    WdbLog.Info($"[{id}] 检测到 cancel 标记，停止任务进程树。");
                    jobCts.Cancel();
                    return;
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                WdbLog.Warn($"[{id}] 检查 cancel 标记失败：{ex.Message}");
            }

            // 串行执行期间，排在本任务之后的排队任务也必须能被取消：把「已提交、未被认领、带 cancel」
            // 的任务直接写成 canceled，否则 CLI 的 wait 会被前面的长任务永久阻塞。
            SweepCanceledPendingJobs();

            try
            {
                await Task.Delay(PollIntervalMs, watchToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    /// <summary>
    /// 控制面清扫：对「request.json 已提交、尚未被任何 Worker 认领、且存在 cancel 标记」的任务
    /// 直接写 canceled（绝不执行）。已被认领的任务由执行路径自己收尾，这里不碰。
    /// </summary>
    private void SweepCanceledPendingJobs()
    {
        try
        {
            foreach (var jobDir in EnumerateJobDirs())
            {
                var id = Path.GetFileName(jobDir);
                if (!WdbPath.IsSafeJobId(id)
                    || !File.Exists(BridgeLayout.RequestPath(_queueRoot, id))
                    || !File.Exists(BridgeLayout.CancelPath(_queueRoot, id)))
                {
                    continue;
                }

                var status = AtomicFile.ReadJsonOrNull<Status>(BridgeLayout.StatusPath(_queueRoot, id));
                if (status is not null)
                {
                    // 已终态 = 无需处理；非终态 = 已被认领（认领时必写 queued），交给执行路径。
                    continue;
                }

                if (File.Exists(Path.Combine(jobDir, ClaimMarkerName)))
                {
                    continue;
                }

                var mode = AtomicFile.ReadJsonOrNull<Request>(BridgeLayout.RequestPath(_queueRoot, id))?.Mode ?? string.Empty;
                WriteStatus(
                    id,
                    mode,
                    JobStates.Canceled,
                    JobPhases.Done,
                    error: "任务在排队期间收到 cancel 标记，未被执行。",
                    finishedUtc: DateTimeOffset.UtcNow);
                WdbLog.Info($"[{id}] 排队期间收到 cancel → 直接标记 canceled（不执行）。");
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            WdbLog.Warn($"清扫排队取消标记失败（不影响当前任务）：{ex.Message}");
        }
    }

    private async Task TimeoutWatchAsync(Request request, CancellationTokenSource jobCts, CancellationToken watchToken, CancelCause cause)
    {
        if (request.TimeoutSeconds <= 0)
        {
            return;
        }

        // 上限 7 天，避免 TimeSpan/参数溢出。
        var timeout = TimeSpan.FromSeconds(Math.Min(request.TimeoutSeconds, 7 * 24 * 3600));
        try
        {
            await Task.Delay(timeout, watchToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (!jobCts.IsCancellationRequested)
        {
            cause.Set("timeout");
            WdbLog.Warn($"[{request.Id}] 超过 {request.TimeoutSeconds}s 硬超时，停止任务进程树。");
            jobCts.Cancel();
        }
    }

    // ---------------------------------------------------------------- 目录与复制

    /// <summary>
    /// 为本任务独占创建 <c>&lt;runs-root&gt;/&lt;id&gt;</c>：任何既存同名条目一律拒绝（不复用），
    /// 然后用 FileMode.CreateNew（O_EXCL）写所有权标记作为唯一的独占认领动作。
    /// </summary>
    private string EnsureRunDirectory(string id)
    {
        var runsRoot = WdbPath.Full(_runsRoot);
        var runDir = WdbPath.Full(Path.Combine(runsRoot, id));
        if (!WdbPath.IsSameOrInside(runDir, runsRoot) || string.Equals(runDir, runsRoot, WdbPath.Comparison))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"运行目录逃逸出 runs-root：{runDir}");
        }

        // 祖先（含 runs-root 自身）任意一级是重解析点都拒绝：否则整棵副本会被写到 runs-root 之外。
        WdbPath.AssertNoReparseAncestors(runDir, "运行目录");

        // 不复用：重启后旧任务已写 interrupted、永不重放，所以同名目录一律视为他人/历史残留。
        if (Directory.Exists(runDir) || File.Exists(runDir))
        {
            throw new WdbException(
                "ERR_RUN_DIR_EXISTS",
                $"运行目录已存在，按不复用策略拒绝：{runDir}（如需重跑请更换任务 id 或清理由操作者处理）");
        }

        Directory.CreateDirectory(runDir);
        WdbPath.AssertNoReparseAncestors(runDir, "运行目录");

        var markerPath = Path.Combine(runDir, RunOwnerMarkerName);
        try
        {
            // CreateNew 是 O_EXCL：并发 Worker / 竞争者已认领时必然失败，绝不覆盖其标记。
            using var stream = new FileStream(markerPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            var bytes = AtomicFile.Utf8NoBom.GetBytes(
                $"{RunOwnerToken}\nid={id}\ncreatedUtc={DateTimeOffset.UtcNow:O}\npid={Environment.ProcessId}\n");
            stream.Write(bytes, 0, bytes.Length);
            stream.Flush(flushToDisk: true);
        }
        catch (IOException ex)
        {
            throw new WdbException(
                "ERR_RUN_DIR_CLAIMED",
                $"运行目录已被其它进程独占认领，拒绝写入：{runDir} —— {ex.Message}",
                ex);
        }

        return runDir;
    }

    private void CopyPayload(string id, string runDir, string sourceDir)
    {
        var payloadRoot = WdbPath.Full(BridgeLayout.PayloadPath(_queueRoot, id));
        var jobDir = WdbPath.Full(BridgeLayout.JobDir(_queueRoot, id));

        if (!WdbPath.IsSameOrInside(payloadRoot, jobDir))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"payload 路径逃逸出任务目录：{payloadRoot}");
        }

        if (!WdbPath.IsSameOrInside(sourceDir, runDir))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", $"副本目录逃逸出运行目录：{sourceDir}");
        }

        WdbPath.AssertNotReparse(payloadRoot, "payload 目录");
        WdbPath.AssertNoReparseAncestors(payloadRoot, "payload 目录");

        if (WdbPath.IsSameOrInside(sourceDir, payloadRoot) || WdbPath.IsSameOrInside(payloadRoot, sourceDir))
        {
            throw new WdbException("ERR_SECURITY_VIOLATION", "运行副本目录与 payload 目录重叠，拒绝复制。");
        }

        Directory.CreateDirectory(sourceDir);
        WdbPath.AssertNoReparseAncestors(sourceDir, "副本根");

        var skipped = new List<string>();
        CopyTree(payloadRoot, sourceDir, sourceDir, skipped);

        if (skipped.Count > 0)
        {
            WdbLog.Warn($"[{id}] payload 内含 {skipped.Count} 个符号链接/重解析点，已跳过（不跟随链接）。");
        }
    }

    /// <summary>
    /// 逐条复制。每个目标条目在写入前都要：① 落在副本根之内；② 自身及已存在的每一级祖先都不是重解析点
    /// —— 预置的 junction/符号链接会把写入带到 runs-root 之外（TOCTOU 面收敛到单条目级别）。
    /// 源侧的重解析点只跳过、不跟随。
    /// </summary>
    private static void CopyTree(string sourceDir, string destinationRoot, string destinationDir, List<string> skipped)
    {
        foreach (var entry in new DirectoryInfo(sourceDir).EnumerateFileSystemInfos())
        {
            if (WdbPath.TryGetAttributes(entry.FullName, out var attributes)
                && (attributes & FileAttributes.ReparsePoint) != 0)
            {
                // 只跳过、不跟随：绝不把队列外的宿主数据读进副本。
                skipped.Add(entry.FullName);
                continue;
            }

            var destination = Path.Combine(destinationDir, entry.Name);

            if (!WdbPath.IsSameOrInside(destination, destinationRoot))
            {
                throw new WdbException("ERR_SECURITY_VIOLATION", $"副本目标逃逸出副本根：{destination}");
            }

            WdbPath.AssertNoReparseAncestors(destination, "副本目标");

            if (entry is DirectoryInfo directory)
            {
                Directory.CreateDirectory(destination);
                WdbPath.AssertNoReparseAncestors(destination, "副本目标");
                CopyTree(directory.FullName, destinationRoot, destination, skipped);
            }
            else if (entry is FileInfo file)
            {
                // 覆盖同名文件，但绝不删除副本目录里已存在的其它文件。
                file.CopyTo(destination, overwrite: true);
            }
        }
    }

    // ---------------------------------------------------------------- 进程执行

    private static string NormalizeProjectPath(string project)
    {
        var normalized = project.Replace('\\', '/').Trim();
        while (normalized.StartsWith("./", StringComparison.Ordinal))
        {
            normalized = normalized[2..];
        }

        return normalized;
    }

    private string[] BuildArguments(Request request, string projectPath)
    {
        var arguments = new List<string>
        {
            "build",
            projectPath,
            "--nologo",
            "--disable-build-servers",
            "-p:UseSharedCompilation=false",
            "-p:MSBuildNodeReuse=false",
        };

        if (!string.IsNullOrWhiteSpace(request.Configuration))
        {
            arguments.Add("-c");
            arguments.Add(request.Configuration);
        }

        if (!string.IsNullOrWhiteSpace(request.Framework))
        {
            arguments.Add("-f");
            arguments.Add(request.Framework);
        }

        return [.. arguments];
    }

    private string[] ArtifactProbeArguments(Request request, string projectPath)
    {
        var arguments = new List<string>
        {
            "msbuild",
            projectPath,
            "-nologo",
            "-getProperty:TargetPath",
            "-p:UseSharedCompilation=false",
            "-p:MSBuildNodeReuse=false",
        };

        if (!string.IsNullOrWhiteSpace(request.Configuration))
        {
            arguments.Add("-p:Configuration=" + request.Configuration);
        }

        if (!string.IsNullOrWhiteSpace(request.Framework))
        {
            arguments.Add("-p:TargetFramework=" + request.Framework);
        }

        return [.. arguments];
    }

    private string[] RunArguments(Request request, string projectPath)
    {
        // dotnet run 不接受 -p:NAME=VALUE（会被当成应用参数），因此这里只用
        // --disable-build-servers + 子进程环境变量关闭 MSBuild 服务器/节点复用。
        var arguments = new List<string>
        {
            "run",
            "--project",
            projectPath,
            "--disable-build-servers",
        };

        if (!string.IsNullOrWhiteSpace(request.Configuration))
        {
            arguments.Add("-c");
            arguments.Add(request.Configuration);
        }

        if (!string.IsNullOrWhiteSpace(request.Framework))
        {
            arguments.Add("--framework");
            arguments.Add(request.Framework);
        }

        arguments.Add("--");
        if (request.Arguments is { Length: > 0 })
        {
            arguments.AddRange(request.Arguments);
        }

        return [.. arguments];
    }

    private string ResolveDotnetExecutable()
    {
        var configured = _options.DotnetPath;
        if (Path.IsPathRooted(configured))
        {
            return configured;
        }

        var pathVariable = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        foreach (var directory in pathVariable.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            var trimmed = directory.Trim('"');
            if (trimmed.Length == 0)
            {
                continue;
            }

            var names = OperatingSystem.IsWindows() ? new[] { configured, configured + ".exe" } : new[] { configured };
            foreach (var name in names)
            {
                try
                {
                    var candidate = Path.Combine(trimmed, name);
                    if (File.Exists(candidate))
                    {
                        return candidate;
                    }
                }
                catch (ArgumentException)
                {
                    // PATH 里有非法项：跳过。
                }
            }
        }

        // 找不到绝对路径就交给系统解析（Windows 侧 runner 会以 lpApplicationName 再核验）。
        return configured;
    }

    private async Task<ProcessOutcome> RunProcessAsync(
        string id,
        string fileName,
        IReadOnlyList<string> arguments,
        string workingDirectory,
        CancellationToken cancellationToken,
        CancelCause cause,
        Action<int>? onStarted,
        StreamCapture? captureStdout)
    {
        var launch = new JobLaunch(fileName, arguments, workingDirectory);
        WdbLog.Info($"[{id}] 启动进程：{Describe(launch)}");

        IJobProcess process;
        try
        {
            process = JobRunnerGateway.Start(launch, _options.AllowNonWindows);
        }
        catch (WdbException)
        {
            throw;
        }
        catch (Exception ex)
        {
            throw new WdbException("ERR_START_FAILED", $"启动进程失败：{ex.Message}", ex);
        }

        using (process)
        using (var stdoutWriter = OpenLogWriter(BridgeLayout.StdoutPath(_queueRoot, id)))
        using (var stderrWriter = OpenLogWriter(BridgeLayout.StderrPath(_queueRoot, id)))
        {
            int pid;
            try
            {
                pid = process.Id;
            }
            catch (InvalidOperationException)
            {
                pid = -1;
            }

            onStarted?.Invoke(pid);

            var stdoutTask = PumpAsync(process.StandardOutput, stdoutWriter, captureStdout);
            var stderrTask = PumpAsync(process.StandardError, stderrWriter, capture: null);

            var canceled = false;
            try
            {
                await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                canceled = true;
            }

            if (canceled)
            {
                WdbLog.Info($"[{id}] 停止原因 {cause.Reason ?? "cancel"}：先终止进程树，再排空日志。");
            }

            // 先杀树（Windows 关 Job → KillOnJobClose 级联；非 Windows 整树 Kill）。
            // 自然退出时也必须做：后代可能仍持有 stdout/stderr 句柄，否则泵流任务永不结束。
            TryKill(process, id);

            await WaitExitQuietlyAsync(process, id).ConfigureAwait(false);
            var streamsDrained = await DrainAsync([stdoutTask, stderrTask], id).ConfigureAwait(false);

            return new ProcessOutcome(SafeExitCode(process), canceled, streamsDrained);
        }
    }

    private async Task PumpAsync(TextReader reader, StreamWriter writer, StreamCapture? capture)
    {
        var buffer = new char[4096];
        try
        {
            while (true)
            {
                var read = await reader.ReadAsync(buffer.AsMemory()).ConfigureAwait(false);
                if (read <= 0)
                {
                    break;
                }

                await writer.WriteAsync(buffer.AsMemory(0, read)).ConfigureAwait(false);
                capture?.Append(buffer, 0, read);
                await writer.FlushAsync().ConfigureAwait(false);
            }
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            // 读端被拆掉（进程被杀或句柄关闭）：正常收尾，不升级为任务失败。
        }
    }

    private static StreamWriter OpenLogWriter(string path)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        // FileShare.ReadWrite：允许 CLI 在写入过程中 tail。
        var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
        return new StreamWriter(stream, AtomicFile.Utf8NoBom) { AutoFlush = false };
    }

    private static void TryKill(IJobProcess process, string id)
    {
        try
        {
            process.Kill();
        }
        catch (Exception ex)
        {
            WdbLog.Warn($"[{id}] 终止进程树失败：{ex.Message}");
        }
    }

    private static async Task WaitExitQuietlyAsync(IJobProcess process, string id)
    {
        try
        {
            await process.WaitForExitAsync(CancellationToken.None)
                .WaitAsync(TimeSpan.FromSeconds(ExitWaitGraceSeconds))
                .ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            WdbLog.Warn($"[{id}] 等待进程退出超过 {ExitWaitGraceSeconds}s，继续回收。");
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            WdbLog.Warn($"[{id}] 等待进程退出异常：{ex.Message}");
        }
    }

    private static async Task<bool> DrainAsync(IReadOnlyList<Task> pumps, string id)
    {
        foreach (var pump in pumps)
        {
            // 即使超时放弃等待，也要观察异常，避免未观察任务异常堆积。
            _ = pump.ContinueWith(
                static task => _ = task.Exception,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        try
        {
            await Task.WhenAll(pumps).WaitAsync(TimeSpan.FromMilliseconds(StreamDrainGraceMs)).ConfigureAwait(false);
            return true;
        }
        catch (TimeoutException)
        {
            WdbLog.Warn($"[{id}] 日志流在 {StreamDrainGraceMs}ms 内未排空（可能有后代仍持有管道），已继续。");
            return false;
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            // 泵流已异常终止，之后不会再有写入，捕获内容可安全读取。
            WdbLog.Warn($"[{id}] 日志流排空异常：{ex.Message}");
            return true;
        }
    }

    /// <summary>
    /// 仅供探针使用的捕获缓冲：排空超时后泵流可能仍在追加，因此写入与快照都必须加锁
    /// （StringBuilder 不是线程安全的）。调用方在排空失败时不得读取快照。
    /// </summary>
    private sealed class StreamCapture
    {
        private readonly StringBuilder _builder = new();
        private readonly Lock _gate = new();

        public void Append(char[] buffer, int offset, int count)
        {
            lock (_gate)
            {
                _builder.Append(buffer, offset, count);
            }
        }

        public string Snapshot()
        {
            lock (_gate)
            {
                return _builder.ToString();
            }
        }
    }

    private static int? SafeExitCode(IJobProcess process)
    {
        try
        {
            return process.ExitCode;
        }
        catch (Exception ex) when (ex is InvalidOperationException or IOException)
        {
            return null;
        }
    }

    /// <summary>
    /// 从 <c>-getProperty:TargetPath</c> 的输出里取出产物路径：优先从末尾找形如 *.dll / *.exe 的行，
    /// 避免 MSBuild 追加在值之后的杂项输出被误当成路径；实在找不到再退回最后一个非空行。
    /// </summary>
    private static string? LastArtifactLine(string text)
    {
        var lines = text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
        for (var i = lines.Length - 1; i >= 0; i--)
        {
            var line = lines[i].Trim().Trim('"');
            if (line.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)
                || line.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                return line;
            }
        }

        return LastNonEmptyLine(text);
    }

    private static string? LastNonEmptyLine(string text)
    {
        var lines = text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
        for (var i = lines.Length - 1; i >= 0; i--)
        {
            var line = lines[i].Trim();
            if (line.Length > 0)
            {
                return line;
            }
        }

        return null;
    }

    private static string Describe(JobLaunch launch)
    {
        var builder = new StringBuilder();
        builder.Append(Quote(launch.FileName));
        foreach (var argument in launch.Arguments)
        {
            builder.Append(' ').Append(Quote(argument));
        }

        return builder.ToString();
    }

    private static string Quote(string value)
    {
        if (value.Length == 0)
        {
            return "\"\"";
        }

        return value.IndexOfAny([' ', '\t', '"']) >= 0
            ? "\"" + value.Replace("\"", "\\\"", StringComparison.Ordinal) + "\""
            : value;
    }

    private static async Task AwaitQuietlyAsync(IReadOnlyList<Task> tasks, string what)
    {
        try
        {
            await Task.WhenAll(tasks).WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            WdbLog.Warn($"{what} 未在 5s 内结束，已继续。");
        }
        catch (Exception ex)
        {
            WdbLog.Warn($"{what} 结束异常：{ex.Message}");
        }
    }

    // ---------------------------------------------------------------- 状态写入

    private void WriteStatus(
        string id,
        string mode,
        string state,
        string? phase,
        int? exitCode = null,
        int? pid = null,
        string? error = null,
        string? artifact = null,
        DateTimeOffset? startedUtc = null,
        DateTimeOffset? finishedUtc = null)
    {
        try
        {
            var status = new Status(
                BridgeProtocol.Version,
                id,
                mode,
                state,
                phase,
                exitCode,
                pid,
                error,
                artifact,
                DateTimeOffset.UtcNow,
                startedUtc,
                finishedUtc);

            AtomicFile.WriteJson(BridgeLayout.StatusPath(_queueRoot, id), status);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            WdbLog.Error($"[{id}] 写 status.json 失败：{ex.Message}");
        }
    }

    /// <summary>把子进程环境对齐到「不遗留常驻构建服务器」。</summary>
    public static void ApplyChildEnvironment(IDictionary<string, string?> environment)
    {
        environment["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1";
        environment["DOTNET_NOLOGO"] = "1";
        environment["DOTNET_SKIP_FIRST_TIME_EXPERIENCE"] = "1";
        environment["DOTNET_CLI_DO_NOT_USE_MSBUILD_SERVER"] = "1";
        environment["MSBUILDDISABLENODEREUSE"] = "1";
        environment["UseSharedCompilation"] = "false";
        environment["MSBuildNodeReuse"] = "false";
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _sigtermRegistration?.Dispose();
        _shutdown.Dispose();
        ReleaseExclusiveLock();
    }
}

/// <summary>桥的 Worker 入口（由 Program 分派 `worker` 子命令时调用）。</summary>
public static class Worker
{
    public static async Task<int> RunAsync(string[]? args)
    {
        WorkerOptions options;
        try
        {
            options = WorkerOptions.Parse(args);
        }
        catch (WdbException ex)
        {
            WdbLog.Error($"{ex.Code}: {ex.Message}");
            Console.Error.WriteLine();
            Console.Error.WriteLine(WorkerOptions.HelpText);
            return WdbExitCode.Usage;
        }

        if (options.ShowHelp)
        {
            Console.Out.WriteLine(WorkerOptions.HelpText);
            return WdbExitCode.Ok;
        }

        if (!OperatingSystem.IsWindows() && !options.AllowNonWindows)
        {
            WdbLog.Error(
                $"当前宿主不是 Windows（{Environment.OSVersion.Platform}）：Worker 拒绝启动。"
                + "Windows 前台 Worker 必须在 Windows 宿主运行；仅 fixture 测试可显式传 --allow-non-windows。");
            return WdbExitCode.NotWindows;
        }

        using var host = new WorkerHost(options);
        try
        {
            return await host.RunAsync().ConfigureAwait(false);
        }
        catch (WdbException ex)
        {
            WdbLog.Error($"{ex.Code}: {ex.Message}");
            return ex.Code is "ERR_RUNS_ROOT_NOT_OWNED" or "ERR_SECURITY_VIOLATION"
                ? WdbExitCode.EnvironmentViolation
                : WdbExitCode.Usage;
        }
        catch (Exception ex)
        {
            WdbLog.Error($"Worker 异常退出：{ex}");
            return WdbExitCode.Internal;
        }
    }
}
