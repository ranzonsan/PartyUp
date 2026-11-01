// Windows 调试桥 · Windows 安全进程树 runner（纯 BCL + Win32 P/Invoke）
//
// 为什么不是 Process.Start：
//   * .NET 不暴露 CREATE_SUSPENDED。先启动再 AssignProcessToJobObject 存在 spawn 竞态 ——
//     子进程一旦抢跑并派生出孙进程，杀树就杀不干净。
//   * 需要 KILL_ON_JOB_CLOSE 兜底：Worker 自身崩溃/被杀时，Job 句柄关闭即连带整棵树终止。
// 因此这里用原始 CreateProcessW(CREATE_SUSPENDED) + 自建可继承管道 + 自建 Job，
// 顺序恒定为：CreateProcessW → AssignProcessToJobObject → ResumeThread；
// 任一失败，用自己的进程句柄 TerminateProcess 掉这个「还没被放行的、自己新建的进程」
// （绝不用 taskkill，也绝不按队列里的 PID 杀进程）。
//
// 因为进程不是 System.Diagnostics.Process 启的，Process.StandardOutput/StandardError
// 拿不到数据，故由 runner 自己暴露 StreamReader/StreamWriter。
// 所有原生句柄一律由 SafeHandle 家族持有，异常路径同样不泄漏。
//
// 与 Worker 的接缝：本文件末尾的 [ModuleInitializer] 把 WindowsProcessJob（IJobProcess 实现）
// 注册进 JobRunnerGateway.WindowsProcessFactory —— Worker.cs 约定的唯一对接点。
//
// 平台：编译无平台限制（Linux 上可 dotnet build）；Start 在非 Windows 上抛
// PlatformNotSupportedException。纯字符串逻辑（命令行/环境块）为 internal 纯函数，可在 Linux 单测。

using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace GalleriesManager.Wdb;

/// <summary>
/// Windows 安全进程树 runner：CREATE_SUSPENDED 建进程、挂进 KILL_ON_JOB_CLOSE 的 Job 后再放行。
/// </summary>
/// <remarks>
/// 使用要点：
/// <list type="bullet">
/// <item><c>info.UseShellExecute</c> 必须为 <c>false</c>；argv 来源是 <c>info.ArgumentList</c>（为空时才透传 <c>info.Arguments</c>）。</item>
/// <item>stdout/stderr 恒被重定向到 runner 自建管道：读 <see cref="StandardOutput"/>/<see cref="StandardError"/>，不要用 <c>Process.StandardOutput</c>。</item>
/// <item><see cref="Dispose"/> 会终止整棵进程树（这是消除孤儿进程的手段），且此后读者流不可用；请先读完输出再 Dispose。</item>
/// <item>本类型仅 Windows 可运行；Linux 上可编译、可单测纯字符串逻辑，<see cref="Start"/> 抛 <see cref="PlatformNotSupportedException"/>。</item>
/// </list>
/// </remarks>
public sealed class WindowsJobRunner : IDisposable
{
    // ── Win32 常量 ────────────────────────────────────────────────────────────
    private const uint CREATE_SUSPENDED = 0x0000_0004;
    private const uint CREATE_UNICODE_ENVIRONMENT = 0x0000_0400;
    private const uint CREATE_NO_WINDOW = 0x0800_0000;
    private const uint EXTENDED_STARTUPINFO_PRESENT = 0x0008_0000;
    private const int STARTF_USESTDHANDLES = 0x0000_0100;
    private const int STARTF_USESHOWWINDOW = 0x0000_0001;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x0000_2000;
    private const uint HANDLE_FLAG_INHERIT = 0x0000_0001;
    private const int JOBOBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9;
    private static readonly IntPtr PROC_THREAD_ATTRIBUTE_HANDLE_LIST = new(0x0002_0002);
    private const uint GENERIC_READ = 0x8000_0000;
    private const uint FILE_SHARE_READ = 0x0000_0001;
    private const uint FILE_SHARE_WRITE = 0x0000_0002;
    private const uint OPEN_EXISTING = 3;
    private static readonly IntPtr INVALID_HANDLE_VALUE = new(-1);

    /// <summary>退出码捕获的最长阻塞时间（有人等 WaitForExitAsync 时）。</summary>
    private const int ExitDrainTimeoutMs = 5_000;

    /// <summary>无人在等时的收尾宽限：TerminateJobObject 后进程句柄通常毫秒级变信号，够用且不留延迟。</summary>
    private const int ExitShutdownGraceMs = 100;

    /// <summary>默认 stdio 解码：UTF-8（不写 BOM）。可被 ProcessStartInfo.Standard*Encoding 覆盖。</summary>
    private static readonly Encoding DefaultStdioEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    private readonly object _gate = new();
    private readonly ProcessWaitHandle _processHandle;   // 进程句柄：可等待对象（WaitForExit）+ Win32 操作入口
    private readonly SafeJobHandle _jobHandle;           // KILL_ON_JOB_CLOSE
    private readonly SafeFileHandle _stdoutReadHandle;
    private readonly SafeFileHandle _stderrReadHandle;
    private readonly SafeFileHandle? _stdinWriteHandle;  // 仅 RedirectStandardInput 时非 null
    private readonly Process _process;                   // 元数据包装（GetProcessById），其流不可用

    private int? _exitCode;
    private bool _disposed;
    private TaskCompletionSource<int>? _exitTcs;
    private RegisteredWaitHandle? _wait;

    private WindowsJobRunner(
        ProcessWaitHandle processHandle,
        SafeJobHandle jobHandle,
        SafeFileHandle stdoutReadHandle,
        SafeFileHandle stderrReadHandle,
        SafeFileHandle? stdinWriteHandle,
        StreamReader standardOutput,
        StreamReader standardError,
        StreamWriter? standardInput,
        Process process,
        int id)
    {
        _processHandle = processHandle;
        _jobHandle = jobHandle;
        _stdoutReadHandle = stdoutReadHandle;
        _stderrReadHandle = stderrReadHandle;
        _stdinWriteHandle = stdinWriteHandle;
        StandardOutput = standardOutput;
        StandardError = standardError;
        StandardInput = standardInput;
        _process = process;
        Id = id;
    }

    /// <summary>System.Diagnostics 包装，仅用于读 Id/Handle 等元数据；它的 StandardOutput/StandardError 不可用。</summary>
    public Process Process => _process;

    /// <summary>子进程 PID（来自 CreateProcessW，绝不用它去杀进程）。</summary>
    public int Id { get; }

    /// <summary>子进程 stdout（UTF-8，BOM 自动识别）。</summary>
    public StreamReader StandardOutput { get; }

    /// <summary>子进程 stderr（UTF-8，BOM 自动识别）。</summary>
    public StreamReader StandardError { get; }

    /// <summary>子进程 stdin；仅当 <c>info.RedirectStandardInput == true</c> 时非 null。<c>Close()</c> 即向子进程发出 EOF。</summary>
    public StreamWriter? StandardInput { get; }

    /// <summary>进程是否已退出（Dispose 之后恒为 true）。</summary>
    public bool HasExited => TryGetExitCode(out _) || IsDisposed;

    /// <summary>退出码；未退出时抛 <see cref="InvalidOperationException"/>（与 <see cref="System.Diagnostics.Process"/> 同语义）。</summary>
    public int ExitCode
    {
        get
        {
            if (TryGetExitCode(out int code)) return code;
            if (IsDisposed) throw new ObjectDisposedException(nameof(WindowsJobRunner), "runner 已释放且退出码未能捕获。");
            throw new InvalidOperationException("进程尚未退出，ExitCode 不可用。");
        }
    }

    private bool IsDisposed
    {
        get { lock (_gate) return _disposed; }
    }

    /// <summary>终止整棵进程树（TerminateJobObject；Job 不可用时退化为终止本进程）。可在任意线程重复调用。</summary>
    public void Kill()
    {
        lock (_gate)
        {
            if (_disposed) return;   // Dispose 已经杀过树
        }

        KillTree();
    }

    /// <summary>
    /// 等待子进程退出并返回退出码（进程退出，不必等子孙进程；子孙由 Job 负责收尸）。
    /// </summary>
    public Task<int> WaitForExitAsync(CancellationToken cancellationToken = default)
    {
        Task<int> exitTask = GetExitTask();
        return cancellationToken.CanBeCanceled ? exitTask.WaitAsync(cancellationToken) : exitTask;
    }

    private Task<int> GetExitTask()
    {
        lock (_gate)
        {
            if (_exitCode is { } cached) return Task.FromResult(cached);
            ObjectDisposedException.ThrowIf(_disposed, this);
            _exitTcs ??= new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (_wait is null)
            {
                _wait = ThreadPool.RegisterWaitForSingleObject(
                    _processHandle,
                    static (state, timedOut) => ((WindowsJobRunner)state!).OnProcessSignaled(timedOut),
                    this,
                    Timeout.Infinite,
                    executeOnlyOnce: true);
            }

            return _exitTcs.Task;
        }
    }

    /// <summary>线程池等待回调：进程句柄被信号化。任何异常都不许逃出回调。</summary>
    private void OnProcessSignaled(bool timedOut)
    {
        try
        {
            if (timedOut) return;
            if (!GetExitCodeProcess(_processHandle.NativeHandle, out uint raw)) return;
            CompleteExit(unchecked((int)raw));
        }
        catch (ObjectDisposedException)
        {
            // Dispose 与本次回调竞争：句柄已关闭，退出码由 Dispose 自己兜。
        }
        catch (Exception)
        {
            // 回调里绝不抛出：线程池未捕获异常会直接杀进程。
        }
    }

    private void CompleteExit(int code)
    {
        RegisteredWaitHandle? wait;
        TaskCompletionSource<int>? tcs;
        int resolved;
        lock (_gate)
        {
            _exitCode ??= code;
            resolved = _exitCode.Value;
            tcs = _exitTcs;
            wait = _wait;
            _wait = null;
        }

        wait?.Unregister(null);
        tcs?.TrySetResult(resolved);
    }

    private bool TryGetExitCode(out int exitCode)
    {
        lock (_gate)
        {
            if (_exitCode is { } cached)
            {
                exitCode = cached;
                return true;
            }

            if (_disposed)
            {
                exitCode = 0;
                return false;
            }
        }

        // WaitOne(0) 先把「已退出但退出码恰为 STILL_ACTIVE(259)」的歧义排除掉。
        bool signaled;
        try
        {
            signaled = _processHandle.WaitOne(0);
        }
        catch (ObjectDisposedException)
        {
            // Dispose 竞争：句柄已释放，退出码由 Dispose 自己兜。
            exitCode = 0;
            return false;
        }

        if (!signaled)
        {
            exitCode = 0;
            return false;
        }

        if (!GetExitCodeProcess(_processHandle.NativeHandle, out uint raw))
        {
            exitCode = 0;
            return false;
        }

        exitCode = unchecked((int)raw);
        lock (_gate)
        {
            _exitCode ??= exitCode;
        }

        return true;
    }

    private void KillTree()
    {
        if (_jobHandle is { IsInvalid: false, IsClosed: false })
        {
            try
            {
                // Job 里只有本 runner 放进去的那棵树（子进程默认随父加入 Job），杀 Job == 杀树。
                if (TerminateJobObject(_jobHandle, 1)) return;
            }
            catch (ObjectDisposedException)
            {
                // 句柄已被 Dispose 关掉：树也随之被 KILL_ON_JOB_CLOSE 收走。
            }
        }

        if (_processHandle.NativeHandle is { IsInvalid: false, IsClosed: false })
        {
            try
            {
                TerminateProcess(_processHandle.NativeHandle, 1);
            }
            catch (ObjectDisposedException)
            {
            }
        }
    }

    /// <summary>
    /// 关闭 stdin → 终止整棵树 → 捕获退出码 → 释放全部原生句柄与读者流。可重复调用。
    /// </summary>
    public void Dispose()
    {
        RegisteredWaitHandle? pendingWait;
        TaskCompletionSource<int>? pendingExit;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            pendingWait = _wait;
            _wait = null;
            pendingExit = _exitTcs;
        }

        try
        {
            try
            {
                StandardInput?.Dispose();
            }
            catch (IOException)
            {
                // stdin 关闭失败不影响后续杀树。
            }

            KillTree();

            // 退出码必须在释放进程句柄之前抓走：
            //   * 有人在 WaitForExitAsync 等 → 给足时间，绝不能让它挂死；
            //   * 没人在等 → 只给极短的收尾宽限，不留无谓延迟。
            int timeout = pendingExit is not null ? ExitDrainTimeoutMs : ExitShutdownGraceMs;
            bool exited = false;
            try
            {
                exited = _processHandle.WaitOne(timeout);
            }
            catch (ObjectDisposedException)
            {
            }

            if (!exited)
            {
                pendingExit?.TrySetException(new TimeoutException($"Dispose 等待进程退出超时（{timeout}ms），未取得退出码。"));
            }
            else if (GetExitCodeProcess(_processHandle.NativeHandle, out uint raw))
            {
                int code = unchecked((int)raw);
                lock (_gate)
                {
                    _exitCode ??= code;
                    pendingExit?.TrySetResult(_exitCode.Value);
                }
            }
            else
            {
                pendingExit?.TrySetException(new ObjectDisposedException(nameof(WindowsJobRunner), "runner 释放时未能取得退出码。"));
            }
        }
        finally
        {
            pendingWait?.Unregister(null);

            // 读者流可能与上面的查询并发被别处使用，逐个兜异常，保证句柄一定会被释放。
            SafeDispose(StandardOutput);
            SafeDispose(StandardError);
            SafeDispose(_stdinWriteHandle);
            SafeDispose(_stdoutReadHandle);
            SafeDispose(_stderrReadHandle);
            _jobHandle.Dispose();        // 关 Job 句柄 → KILL_ON_JOB_CLOSE 兜底收树
            _processHandle.Dispose();
            _process.Dispose();
        }
    }

    private static void SafeDispose(IDisposable? disposable)
    {
        try
        {
            disposable?.Dispose();
        }
        catch (Exception)
        {
        }
    }

    // ── 启动 ────────────────────────────────────────────────────────────────

    /// <summary>
    /// 以 CREATE_SUSPENDED 启动进程，挂入 KILL_ON_JOB_CLOSE 的 Job 后再放行。
    /// </summary>
    /// <exception cref="InvalidOperationException"><c>UseShellExecute</c> 为 true、设置了 <c>Verb</c>、或 <c>FileName</c> 为空。</exception>
    /// <exception cref="NotSupportedException">设置了 <c>UserName</c>/<c>Password</c>/<c>Domain</c>（原始 CreateProcessW 无凭据参数）。</exception>
    /// <exception cref="PlatformNotSupportedException">非 Windows。</exception>
    /// <exception cref="Win32Exception">任一 Win32 步骤失败（此时已终止那个仍挂起的自己新建的进程）。</exception>
    public static WindowsJobRunner Start(ProcessStartInfo info)
    {
        ArgumentNullException.ThrowIfNull(info);

        // 纯托管校验先行：让非法参数在 Linux 上也能被单测覆盖。
        if (info.UseShellExecute) throw new InvalidOperationException("UseShellExecute 必须为 false：本 runner 直接调用 CreateProcessW，不经过 shell。");
        if (!string.IsNullOrEmpty(info.Verb)) throw new InvalidOperationException("Verb 仅适用于 UseShellExecute=true，本 runner 不支持。");
        if (string.IsNullOrWhiteSpace(info.FileName)) throw new InvalidOperationException("ProcessStartInfo.FileName 不能为空。");

        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException(
                $"WindowsJobRunner 只能在 Windows 上运行（当前 {RuntimeInformation.RuntimeIdentifier}）。");
        }

        // 凭据相关属性在非 Windows 上连 getter 都会抛 PlatformNotSupportedException，故必须放在平台守卫之后。
        if (!string.IsNullOrEmpty(info.UserName) || !string.IsNullOrEmpty(info.Domain) || info.Password is { Length: > 0 })
        {
            throw new NotSupportedException("不支持以其它用户身份启动（CreateProcessW 无凭据参数，需要 CreateProcessWithLogonW）。");
        }

        string fileName = info.FileName.Trim();
        // argv 唯一来源是 ArgumentList；为空时才原样透传 Arguments（与 .NET 自身行为一致）。
        string arguments = info.ArgumentList.Count > 0 ? BuildArguments(info.ArgumentList) : info.Arguments ?? string.Empty;
        string commandLine = BuildCommandLine(fileName, arguments);
        string? workingDirectory = string.IsNullOrEmpty(info.WorkingDirectory) ? null : info.WorkingDirectory;
        // 绝对路径走 lpApplicationName（避免 CreateProcessW 的搜索路径劫持）；相对名走命令行搜索。
        string? applicationName = Path.IsPathRooted(fileName) ? fileName : null;

        // Standard*Encoding 的 getter 在对应 Redirect 标志为 false 时会抛，必须短路保护。
        Encoding outputEncoding = info.RedirectStandardOutput && info.StandardOutputEncoding is { } outEnc ? outEnc : DefaultStdioEncoding;
        Encoding errorEncoding = info.RedirectStandardError && info.StandardErrorEncoding is { } errEnc ? errEnc : DefaultStdioEncoding;
        Encoding inputEncoding = info.RedirectStandardInput && info.StandardInputEncoding is { } inEnc ? inEnc : DefaultStdioEncoding;
        bool redirectStandardInput = info.RedirectStandardInput;

        SECURITY_ATTRIBUTES inheritable = default;
        inheritable.nLength = Marshal.SizeOf<SECURITY_ATTRIBUTES>();
        inheritable.bInheritHandle = 1;   // 子进程侧句柄必须可继承
        inheritable.lpSecurityDescriptor = IntPtr.Zero;

        SafeFileHandle? stdoutRead = null;
        SafeFileHandle? stdoutWrite = null;
        SafeFileHandle? stderrRead = null;
        SafeFileHandle? stderrWrite = null;
        SafeFileHandle? stdinRead = null;
        SafeFileHandle? stdinWrite = null;
        ProcessWaitHandle? processHandle = null;
        SafeWaitHandle? threadHandle = null;
        SafeJobHandle? jobHandle = null;
        IntPtr attributeList = IntPtr.Zero;
        IntPtr handleListPtr = IntPtr.Zero;
        IntPtr commandLinePtr = IntPtr.Zero;
        IntPtr environmentPtr = IntPtr.Zero;
        bool initializedAttributeList = false;   // 只有初始化成功的缓冲才允许 DeleteProcThreadAttributeList
        IntPtr rawProcessHandle = IntPtr.Zero;   // CreateProcessW 刚返回、尚未交给 SafeHandle 的裸句柄
        IntPtr rawThreadHandle = IntPtr.Zero;
        bool handedOver = false;

        try
        {
            // 1) 三条 stdio 管道：读端父进程独占（清掉继承位），写端可继承交给子进程。
            CreatePipe(ref inheritable, out stdoutRead, out stdoutWrite);
            MakeNonInheritable(stdoutRead, "stdout 读端");
            CreatePipe(ref inheritable, out stderrRead, out stderrWrite);
            MakeNonInheritable(stderrRead, "stderr 读端");
            if (redirectStandardInput)
            {
                CreatePipe(ref inheritable, out stdinRead, out stdinWrite);
                MakeNonInheritable(stdinWrite, "stdin 写端");
            }
            else
            {
                // 不重定向 stdin 时给 NUL：读到 EOF，绝不把父进程的 stdin 漏给子进程。
                stdinRead = OpenInheritableNullDevice(ref inheritable);
            }

            // 托管对象先建好：一旦 CreateProcessW 成功，后面任何失败都必须发生在「仍挂起」阶段。
            var standardOutput = new StreamReader(
                new FileStream(stdoutRead, FileAccess.Read, bufferSize: 4096),
                outputEncoding,
                detectEncodingFromByteOrderMarks: true,
                bufferSize: 4096);
            var standardError = new StreamReader(
                new FileStream(stderrRead, FileAccess.Read, bufferSize: 4096),
                errorEncoding,
                detectEncodingFromByteOrderMarks: true,
                bufferSize: 4096);
            StreamWriter? standardInput = null;
            if (redirectStandardInput && stdinWrite is not null)
            {
                standardInput = new StreamWriter(
                    new FileStream(stdinWrite, FileAccess.Write, bufferSize: 4096),
                    inputEncoding,
                    bufferSize: 4096)
                {
                    AutoFlush = true,
                };
            }

            // 2) STARTUPINFOEX + 句柄白名单：bInheritHandles=true 时只继承列出的这三个句柄，
            //    避免把本进程里其它可继承句柄（含别的 job 的管道写端）漏进子进程导致读端等不到 EOF。
            IntPtr attributeListSize = IntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref attributeListSize);   // 预期失败(ERROR_INSUFFICIENT_BUFFER)，只取所需大小
            int attributeListSizeError = Marshal.GetLastWin32Error();
            if (attributeListSize == IntPtr.Zero)
            {
                // 尺寸都没拿到就不能 AllocHGlobal(0) 硬撑；DeleteProcThreadAttributeList 也只能对初始化成功的缓冲调用。
                throw new Win32Exception(attributeListSizeError, "InitializeProcThreadAttributeList 未返回所需缓冲大小。");
            }

            attributeList = Marshal.AllocHGlobal(attributeListSize);
            if (!InitializeProcThreadAttributeList(attributeList, 1, 0, ref attributeListSize))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "InitializeProcThreadAttributeList 失败。");
            }

            initializedAttributeList = true;

            IntPtr[] inheritedHandles = { stdinRead.DangerousGetHandle(), stdoutWrite.DangerousGetHandle(), stderrWrite.DangerousGetHandle() };
            handleListPtr = Marshal.AllocHGlobal(IntPtr.Size * inheritedHandles.Length);
            Marshal.Copy(inheritedHandles, 0, handleListPtr, inheritedHandles.Length);
            if (!UpdateProcThreadAttribute(
                    attributeList,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    handleListPtr,
                    (IntPtr)(IntPtr.Size * inheritedHandles.Length),
                    IntPtr.Zero,
                    IntPtr.Zero))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_HANDLE_LIST) 失败。");
            }

            var startupInfo = new STARTUPINFOEX();
            startupInfo.StartupInfo.cb = Marshal.SizeOf<STARTUPINFOEX>();
            startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.StartupInfo.hStdInput = stdinRead.DangerousGetHandle();
            startupInfo.StartupInfo.hStdOutput = stdoutWrite.DangerousGetHandle();
            startupInfo.StartupInfo.hStdError = stderrWrite.DangerousGetHandle();
            startupInfo.lpAttributeList = attributeList;

            if (info.WindowStyle != ProcessWindowStyle.Normal)
            {
                startupInfo.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
                startupInfo.StartupInfo.wShowWindow = ToShowWindow(info.WindowStyle);
            }

            uint creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
            if (info.CreateNoWindow) creationFlags |= CREATE_NO_WINDOW;

            string? environmentBlock = BuildEnvironmentBlock(info.Environment);
            if (environmentBlock is not null) environmentPtr = Marshal.StringToHGlobalUni(environmentBlock);
            commandLinePtr = Marshal.StringToHGlobalUni(commandLine);   // CreateProcessW 可能改写命令行缓冲区，不能传托管 string

            // 3) 建进程，但先不放行。
            if (!CreateProcessW(
                    applicationName,
                    commandLinePtr,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    bInheritHandles: true,
                    creationFlags,
                    environmentPtr,
                    workingDirectory,
                    ref startupInfo,
                    out PROCESS_INFORMATION processInformation))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"CreateProcessW 失败：{fileName}");
            }

            // 进程句柄先留在裸局部变量里，再逐个交给 SafeHandle：
            // 包装动作本身也可能抛（OOM），那时句柄还没人接管，只能靠下面的 catch/finally 收尾。
            rawProcessHandle = processInformation.hProcess;
            rawThreadHandle = processInformation.hThread;

            // 进程句柄包装成可等待的 WaitHandle：既是 Win32 调用凭据，也是 WaitForExit 的等待对象。
            processHandle = new ProcessWaitHandle(rawProcessHandle);
            rawProcessHandle = IntPtr.Zero;   // 所有权已转移给 SafeHandle
            threadHandle = new SafeWaitHandle(rawThreadHandle, ownsHandle: true);
            rawThreadHandle = IntPtr.Zero;

            // 进程此刻仍挂起，PID 不可能被回收 —— 在这里建元数据包装最安全。
            Process process = Process.GetProcessById(processInformation.dwProcessId);

            // 4) KILL_ON_JOB_CLOSE 的 Job（在 CreateProcessW 之后创建，句柄不会被刚建的子进程继承）。
            jobHandle = CreateKillOnCloseJob();

            // 5) 先入 Job。失败即终止，绝不「放行后再补挂」。
            if (!AssignProcessToJobObject(jobHandle, processHandle.NativeHandle))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "AssignProcessToJobObject 失败（父进程可能已处于不允许嵌套的 Job 中）。");
            }

            // 6) 至此才放行。ResumeThread 返回上一个挂起计数，失败返回 (uint)-1。
            if (ResumeThread(threadHandle) == unchecked((uint)-1))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "ResumeThread 失败。");
            }

            // 放行之后线程句柄不再有用，立刻关掉（否则每次 Start 泄漏一个句柄）。
            threadHandle.Dispose();
            threadHandle = null;

            var runner = new WindowsJobRunner(
                processHandle,
                jobHandle,
                stdoutRead,
                stderrRead,
                redirectStandardInput ? stdinWrite : null,
                standardOutput,
                standardError,
                standardInput,
                process,
                processInformation.dwProcessId);

            handedOver = true;
            return runner;
        }
        catch
        {
            // 任何一步失败：用自己持有的句柄终止这个「新建的、可控的」进程。
            // 若 SafeHandle 包装尚未完成（OOM），退回裸句柄 —— 否则会留下一个永远挂起、无人认领的子进程。
            if (processHandle is { NativeHandle.IsInvalid: false, NativeHandle.IsClosed: false })
            {
                try
                {
                    TerminateProcess(processHandle.NativeHandle, 1);
                }
                catch (Exception)
                {
                }
            }
            else if (rawProcessHandle != IntPtr.Zero)
            {
                try
                {
                    TerminateProcessRaw(rawProcessHandle, 1);
                }
                catch (Exception)
                {
                }
            }

            throw;
        }
        finally
        {
            // 父进程侧的写端副本必须立刻关掉，否则读端永远等不到 EOF。
            SafeDispose(stdoutWrite);
            SafeDispose(stderrWrite);
            SafeDispose(stdinRead);

            if (!handedOver)
            {
                SafeDispose(stdinWrite);
                SafeDispose(stdoutRead);
                SafeDispose(stderrRead);
                SafeDispose(threadHandle);
                SafeDispose(processHandle);
                SafeDispose(jobHandle);
            }

            // 没被 SafeHandle 接管的裸句柄在这里兜底关闭（转移所有权后变量已置零，不会二次关闭）。
            if (rawProcessHandle != IntPtr.Zero) CloseHandle(rawProcessHandle);
            if (rawThreadHandle != IntPtr.Zero) CloseHandle(rawThreadHandle);

            // DeleteProcThreadAttributeList 只能对「初始化成功」的缓冲调用；分配了但初始化失败的缓冲只能 Free。
            if (initializedAttributeList) DeleteProcThreadAttributeList(attributeList);
            if (attributeList != IntPtr.Zero) Marshal.FreeHGlobal(attributeList);
            if (handleListPtr != IntPtr.Zero) Marshal.FreeHGlobal(handleListPtr);
            if (commandLinePtr != IntPtr.Zero) Marshal.FreeHGlobal(commandLinePtr);
            if (environmentPtr != IntPtr.Zero) Marshal.FreeHGlobal(environmentPtr);
        }
    }

    private static SafeJobHandle CreateKillOnCloseJob()
    {
        IntPtr raw = CreateJobObjectW(IntPtr.Zero, null);
        if (raw == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObjectW 失败。");

        var job = new SafeJobHandle(raw);
        var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        if (!SetInformationJobObject(
                job,
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION_CLASS,
                ref limits,
                (uint)Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>()))
        {
            int error = Marshal.GetLastWin32Error();
            job.Dispose();
            throw new Win32Exception(error, "SetInformationJobObject(KILL_ON_JOB_CLOSE) 失败。");
        }

        return job;
    }

    private static void CreatePipe(ref SECURITY_ATTRIBUTES inheritable, out SafeFileHandle read, out SafeFileHandle write)
    {
        if (!CreatePipe(out IntPtr readRaw, out IntPtr writeRaw, ref inheritable, 0))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreatePipe 失败。");
        }

        read = new SafeFileHandle(readRaw, ownsHandle: true);
        write = new SafeFileHandle(writeRaw, ownsHandle: true);
    }

    private static void MakeNonInheritable(SafeFileHandle handle, string what)
    {
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"SetHandleInformation 失败（{what}）。");
        }
    }

    private static SafeFileHandle OpenInheritableNullDevice(ref SECURITY_ATTRIBUTES inheritable)
    {
        IntPtr raw = CreateFileW("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, ref inheritable, OPEN_EXISTING, 0, IntPtr.Zero);
        if (raw == INVALID_HANDLE_VALUE || raw == IntPtr.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "打开 NUL 设备失败。");
        }

        return new SafeFileHandle(raw, ownsHandle: true);
    }

    private static short ToShowWindow(ProcessWindowStyle style) => style switch
    {
        ProcessWindowStyle.Hidden => 0,      // SW_HIDE
        ProcessWindowStyle.Minimized => 2,   // SW_SHOWMINIMIZED
        ProcessWindowStyle.Maximized => 3,   // SW_SHOWMAXIMIZED
        _ => 1,                              // SW_SHOWNORMAL
    };

    // ── 纯字符串逻辑（internal：可在 Linux 上直接单测） ──────────────────────

    /// <summary>
    /// 按 Windows CRT / CommandLineToArgvW 的规则把 argv 拼成命令行（等价于 .NET 的 PasteArguments）。
    /// </summary>
    internal static string BuildArguments(IReadOnlyList<string> argumentList)
    {
        var builder = new StringBuilder();
        foreach (string argument in argumentList)
        {
            AppendArgument(builder, argument);
        }

        return builder.ToString();
    }

    /// <summary>
    /// 追加单个参数并做 CRT 转义：
    /// 空串 → <c>""</c>；含**任意 Unicode 空白**（char.IsWhiteSpace：空格/制表/CR/LF/VT/FF/NBSP/U+2028/U+3000…）
    /// 或引号才加引号；引号前反斜杠按 2N+1 规则翻倍，串尾反斜杠按 2N 规则翻倍（否则会吃掉收尾引号）。
    /// 判定与 .NET 运行时 System.PasteArguments.ContainsNoWhitespaceOrQuotes 等价（测试里有反射差分）。
    /// </summary>
    internal static void AppendArgument(StringBuilder builder, string argument)
    {
        if (builder.Length != 0) builder.Append(' ');
        if (argument.Length == 0)
        {
            builder.Append("\"\"");
            return;
        }

        bool needsQuoting = false;
        foreach (char c in argument)
        {
            if (char.IsWhiteSpace(c) || c == '"')
            {
                needsQuoting = true;
                break;
            }
        }

        if (!needsQuoting)
        {
            builder.Append(argument);
            return;
        }

        builder.Append('"');
        int backslashes = 0;
        for (int i = 0; i < argument.Length; i++)
        {
            char c = argument[i];
            if (c == '\\')
            {
                backslashes++;
            }
            else if (c == '"')
            {
                builder.Append('\\', (backslashes * 2) + 1).Append('"');
                backslashes = 0;
            }
            else
            {
                builder.Append('\\', backslashes).Append(c);
                backslashes = 0;
            }
        }

        builder.Append('\\', backslashes * 2).Append('"');
    }

    /// <summary>argv[0] 用引号包住（已带引号则原样），再接已转义的 argv。</summary>
    internal static string BuildCommandLine(string fileName, string arguments)
    {
        string executable = fileName.Trim();
        bool alreadyQuoted = executable.Length >= 2 && executable[0] == '"' && executable[^1] == '"';

        var builder = new StringBuilder();
        if (!alreadyQuoted) builder.Append('"');
        builder.Append(executable);
        if (!alreadyQuoted) builder.Append('"');
        if (arguments.Length != 0)
        {
            builder.Append(' ').Append(arguments);
        }

        return builder.ToString();
    }

    /// <summary>
    /// 构造 CREATE_UNICODE_ENVIRONMENT 用的环境块（"K=V\0...\0"，不排序不许）。无条目时返回 null 表示继承父环境。
    /// </summary>
    internal static string? BuildEnvironmentBlock(IEnumerable<KeyValuePair<string, string?>> environment)
    {
        var entries = new List<string>();
        foreach (var pair in environment)
        {
            string key = pair.Key;
            if (string.IsNullOrEmpty(key)) continue;

            string value = pair.Value ?? string.Empty;
            if (key.Contains('\0') || value.Contains('\0')) continue;

            // 驱动器当前目录这类隐藏变量在块里的形态是 "=C:=C:\dir"。
            // .NET 的 Environment 字典里这个 Value 是否带前导 '=' 随实现版本而异，两种都兜住。
            entries.Add(key[0] == '='
                ? (value.Length != 0 && value[0] == '=' ? key + value : key + "=" + value)
                : key + "=" + value);
        }

        if (entries.Count == 0) return null;

        entries.Sort(StringComparer.OrdinalIgnoreCase);
        var builder = new StringBuilder();
        foreach (string entry in entries)
        {
            builder.Append(entry).Append('\0');
        }

        builder.Append('\0');
        return builder.ToString();
    }

    // ── 原生互操作 ──────────────────────────────────────────────────────────

    [StructLayout(LayoutKind.Sequential)]
    private struct SECURITY_ATTRIBUTES
    {
        public int nLength;
        public IntPtr lpSecurityDescriptor;
        public int bInheritHandle;   // BOOL：用 int 保持结构体 blittable
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct STARTUPINFO
    {
        public int cb;
        public IntPtr lpReserved;
        public IntPtr lpDesktop;
        public IntPtr lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct STARTUPINFOEX
    {
        public STARTUPINFO StartupInfo;
        public IntPtr lpAttributeList;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    /// <summary>
    /// 把进程句柄包成可等待对象：<see cref="WaitHandle"/> 侧提供 WaitOne / 线程池等待，
    /// <see cref="NativeHandle"/> 侧供 AssignProcessToJobObject / TerminateProcess / GetExitCodeProcess 使用。
    /// </summary>
    private sealed class ProcessWaitHandle : WaitHandle
    {
        public ProcessWaitHandle(IntPtr handle)
        {
            SafeWaitHandle = new SafeWaitHandle(handle, ownsHandle: true);
        }

        public SafeWaitHandle NativeHandle => SafeWaitHandle;
    }

    private sealed class SafeJobHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public SafeJobHandle(IntPtr handle)
            : base(ownsHandle: true)
        {
            SetHandle(handle);
        }

        protected override bool ReleaseHandle() => CloseHandle(handle);
    }

    /// <summary>关闭裸原生句柄（未被 SafeHandle 接管时的兜底释放路径）。</summary>
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessW(
        string? lpApplicationName,
        IntPtr lpCommandLine,   // 交由 Marshal.StringToHGlobalUni 分配的缓冲区（CreateProcessW 可能改写它）
        IntPtr lpProcessAttributes,
        IntPtr lpThreadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool bInheritHandles,
        uint dwCreationFlags,
        IntPtr lpEnvironment,
        string? lpCurrentDirectory,
        ref STARTUPINFOEX lpStartupInfo,
        out PROCESS_INFORMATION lpProcessInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(SafeWaitHandle hThread);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(SafeWaitHandle hProcess, uint uExitCode);

    /// <summary>裸句柄版：仅用于 SafeHandle 包装抛异常（OOM）时收尾那个仍挂起的自建进程。</summary>
    [DllImport("kernel32.dll", EntryPoint = "TerminateProcess", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcessRaw(IntPtr hProcess, uint uExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeProcess(SafeWaitHandle hProcess, out uint lpExitCode);

    [DllImport("kernel32.dll", EntryPoint = "CreateJobObjectW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObjectW(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetInformationJobObject(
        SafeJobHandle hJob,
        int jobObjectInformationClass,
        ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION lpJobObjectInformation,
        uint cbJobObjectInformationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AssignProcessToJobObject(SafeJobHandle hJob, SafeWaitHandle hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateJobObject(SafeJobHandle hJob, uint uExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreatePipe(
        out IntPtr hReadPipe,
        out IntPtr hWritePipe,
        ref SECURITY_ATTRIBUTES lpPipeAttributes,
        uint nSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetHandleInformation(SafeFileHandle hObject, uint dwMask, uint dwFlags);

    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        ref SECURITY_ATTRIBUTES lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool InitializeProcThreadAttributeList(
        IntPtr lpAttributeList,
        int dwAttributeCount,
        int dwFlags,
        ref IntPtr lpSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateProcThreadAttribute(
        IntPtr lpAttributeList,
        uint dwFlags,
        IntPtr attribute,
        IntPtr lpValue,
        IntPtr cbSize,
        IntPtr lpPreviousValue,
        IntPtr lpReturnSize);

    [DllImport("kernel32.dll")]
    private static extern void DeleteProcThreadAttributeList(IntPtr lpAttributeList);
}

// ── 与 Worker 的接缝（Worker.cs 的 JobLaunch / IJobProcess / JobRunnerGateway） ──────────────
// Worker 在 Windows 上只认 JobRunnerGateway.WindowsProcessFactory：未注册即直接以
// ERR_JOB_RUNNER_UNAVAILABLE 拒绝启动，绝不降级成普通 Process（那会产生脱离 Job 的孤儿进程）。
// 这里的模块初始化器是「runner 接入 Worker」的唯一动作，Worker.cs 的注释亦以此为准。

/// <summary>把 <see cref="WindowsJobRunner"/> 适配成 Worker 的 <see cref="IJobProcess"/>。</summary>
internal sealed class WindowsProcessJob : IJobProcess
{
    private readonly WindowsJobRunner _runner;

    private WindowsProcessJob(WindowsJobRunner runner) => _runner = runner;

    /// <summary>按 Worker 的启动语义（无 shell、UTF-8 无 BOM、隐藏窗口、子进程环境变量）拉起目标进程。</summary>
    public static IJobProcess Start(JobLaunch launch)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = launch.FileName,
            WorkingDirectory = launch.WorkingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true,
            StandardOutputEncoding = AtomicFile.Utf8NoBom,
            StandardErrorEncoding = AtomicFile.Utf8NoBom,
        };

        foreach (string argument in launch.Arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        WorkerHost.ApplyChildEnvironment(startInfo.Environment);

        WindowsJobRunner runner = WindowsJobRunner.Start(startInfo);

        // 子进程不该读前台控制台：立刻关闭 stdin（等效 NUL，读到 EOF）。
        try
        {
            runner.StandardInput?.Dispose();
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException)
        {
        }

        return new WindowsProcessJob(runner);
    }

    public int Id => _runner.Id;

    /// <summary>运行中返回 null；退出后返回真实退出码（Dispose 之后若退出码不可得亦返回 null）。</summary>
    public int? ExitCode
    {
        get
        {
            try
            {
                return _runner.HasExited ? _runner.ExitCode : null;
            }
            catch (ObjectDisposedException)
            {
                return null;
            }
        }
    }

    public TextReader StandardOutput => _runner.StandardOutput;

    public TextReader StandardError => _runner.StandardError;

    public Task WaitForExitAsync(CancellationToken cancellationToken) => _runner.WaitForExitAsync(cancellationToken);

    /// <summary>幂等，且进程已自然退出后调用不抛：关 Job 触发 KillOnJobClose 级联收走整棵树。</summary>
    public void Kill() => _runner.Kill();

    public void Dispose() => _runner.Dispose();
}

/// <summary>模块初始化即完成注册：Worker 任何代码执行前，Windows 工厂已就位。</summary>
internal static class WindowsJobRunnerRegistration
{
    [ModuleInitializer]
    internal static void Register()
        => JobRunnerGateway.WindowsProcessFactory = static launch => WindowsProcessJob.Start(launch);
}
