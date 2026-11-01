using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using GalleriesManager.Wdb;

namespace Wdb.LifecycleProbe;

/// <summary>
/// 真实 Windows 宿主上的「副 Worker 生命周期」探针。由主桥（真实 Windows Worker）作为
/// 一个 build-run 任务启动；探针自己再拉起一个专用副 Worker（本 exe 的 --secondary-worker
/// 子模式 → 公共 <see cref="Worker.RunAsync"/>），在**只属于本次运行的新目录**里验证：
///
///   1) nested Job：副 Worker 自己运行在「外层主 Job」之内，却还能给它的子进程建自己的
///      Job（AssignProcessToJobObject 在嵌套不允许时会失败并熔断，故"任务真的跑起来"即证明嵌套 Assign 成功）；
///   2) hard kill：探针只 Kill 自己 Process.Start 得到的那个副 Worker 进程对象（Kill(false)），
///      副 Worker 的 Job 句柄随之关闭，KILL_ON_JOB_CLOSE 必须收走它整棵树（含孙进程）；
///   3) restart：同一 queue 重启副 Worker，被杀时未终态的任务必须被写 interrupted 且绝不重放；
///   4) shutdown：给 queue 写 shutdown 标记，副 Worker 必须主动清掉正在运行的整棵树并退出。
///
/// 两种 queue 格式都跑一遍：workspace 的 \\wsl.localhost UNC 子目录，与 Windows 本地盘子目录。
/// 硬性边界：只碰自己新建的目录；只杀自己 Process.Start 得到的进程对象；绝不 taskkill、
/// 绝不按持久化的 PID 杀进程、绝不触碰主 Worker 与默认 runs-root。
/// </summary>
internal static class Program
{
    private const string BoardPrefix = "WDB_LIFECYCLE ";
    private const string ProgressPrefix = "WDB_LIFECYCLE_PROGRESS ";
    /// <summary>期望的完整三层：root(depth=0) → child(depth=1) → grandchild(depth=2)。</summary>
    private const int ExpectedLevels = 3;
    private const string SelfTestPrefix = "WDB_LIFECYCLE_SELFTEST ";

    private static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

        if (args.Length > 0 && string.Equals(args[0], "--secondary-worker", StringComparison.Ordinal))
        {
            // 真正的副 Worker：与主桥同一个 Worker 实现、同一个 WindowsJobRunner 接线。
            return await Worker.RunAsync(args.Skip(1).ToArray()).ConfigureAwait(false);
        }

        if (args.Length > 0 && string.Equals(args[0], "--self-test", StringComparison.Ordinal))
        {
            return RunSelfTest(args.Skip(1).ToArray());
        }

        if (args.Length > 0 && string.Equals(args[0], "--verify-echoapp-spawn", StringComparison.Ordinal))
        {
            return await RunEchoAppSpawnCheckAsync().ConfigureAwait(false);
        }

        return await RunScenarioAsync(args).ConfigureAwait(false);
    }

    // ── 主场景 ──────────────────────────────────────────────────────────────

    private static async Task<int> RunScenarioAsync(string[] args)
    {
        var board = new Board();
        var startedUtc = DateTimeOffset.UtcNow;
        board.Probe = "WorkerLifecycleProbe/v1";
        board.StartedUtc = startedUtc;
        board.Host = RuntimeInformation.OSDescription;
        board.IsWindows = OperatingSystem.IsWindows();
        board.Cwd = Environment.CurrentDirectory;
        board.ToolDir = AppContext.BaseDirectory;
        board.ProcessPath = Environment.ProcessPath ?? "(null)";
        board.ProbePid = Environment.ProcessId;
        board.ProbeUpTime = Process.GetCurrentProcess().StartTime.ToUniversalTime();

        Progress("scenario-begin", $"probe pid={Environment.ProcessId} cwd={Environment.CurrentDirectory}");

        ScenarioOptions opts;
        try
        {
            opts = ScenarioOptions.Parse(args);
        }
        catch (ArgumentException ex)
        {
            // 没有现场主 Worker PID 就无法核对"主 Worker 未被停"，宁可拒绝执行也不产出占位式假绿。
            board.Fail("usage.main-pid-required", ex.Message);
            return board.Emit(exitCode: 90);
        }
        board.Args = args;
        board.UncQueue = opts.UncQueue;
        board.MainPid = opts.MainPid;
        board.Dotnet = opts.Dotnet;

        // 主 Worker 与默认 runs-root 的「事前」快照：全程只读，用来证明没被触碰。
        board.MainWorkerBefore = DescribeProcess(opts.MainPid);
        board.DefaultRunsRoot = DefaultRunsRoot();
        board.DefaultRunsRootBefore = DescribeRunsRoot(board.DefaultRunsRoot);

        var created = new List<Process>();
        var shutdownRequested = false;

        try
        {
            if (!board.IsWindows)
            {
                board.Fail("env.windows-required", "非 Windows 宿主：生命周期探针只允许在真实 Windows 上运行（本进程不做任何进程启动）。");
                return board.Emit(exitCode: 90);
            }

            if (string.IsNullOrWhiteSpace(opts.UncQueue))
            {
                board.Fail("env.unc-queue-required", "缺少 --unc-queue <workspace UNC 子目录>；探针拒绝在未知目录里创建队列。");
                return board.Emit(exitCode: 90);
            }

            if (!LooksLikeUnc(opts.UncQueue))
            {
                board.Fail("env.unc-queue-not-unc", $"--unc-queue 必须是 \\\\wsl.localhost\\... 形式的 UNC 路径，实际：{opts.UncQueue}");
                return board.Emit(exitCode: 90);
            }

            board.Check(
                "preflight.self-exe-resolvable",
                ResolveSelfCommand(out var selfExe, out var selfArg0),
                $"selfExe={selfExe} arg0={selfArg0 ?? "(none)"} processPath={board.ProcessPath}");

            var stamp = opts.Stamp.Length > 0 ? opts.Stamp : DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'", CultureInfo.InvariantCulture);

            // 两个 phase：同一套检查，两种 queue 格式（UNC 与 Windows 本地盘）。
            var phases = new List<PhaseSpec>
            {
                new(
                    "unc",
                    opts.UncQueue,
                    Path.Combine(board.Cwd, $"wdb-lifecycle-{stamp}-runs-unc"),
                    Path.Combine(board.Cwd, $"wdb-lifecycle-{stamp}-probe-unc"),
                    IsUnc: true),
                new(
                    "local",
                    Path.Combine(board.Cwd, $"wdb-lifecycle-{stamp}-localqueue"),
                    Path.Combine(board.Cwd, $"wdb-lifecycle-{stamp}-runs-local"),
                    Path.Combine(board.Cwd, $"wdb-lifecycle-{stamp}-probe-local"),
                    IsUnc: false),
            };

            var overallDeadline = DateTime.UtcNow.AddSeconds(opts.OverallTimeoutSeconds);
            foreach (var phase in phases)
            {
                if (DateTime.UtcNow >= overallDeadline)
                {
                    board.Fail($"scenario.overall-timeout.{phase.Name}", $"总时限 {opts.OverallTimeoutSeconds}s 用尽，未开始 {phase.Name} 阶段（避免长期占用主队列）。");
                    break;
                }

                Progress("phase-begin", $"{phase.Name} queue={phase.QueueRoot} runs={phase.RunsRoot}");
                var phaseOk = await RunPhaseAsync(board, phase, opts, created, token => shutdownRequested = true).ConfigureAwait(false);
                Progress("phase-end", $"{phase.Name} ok={phaseOk}");
                if (!phaseOk)
                {
                    // fail-fast：同一根因没必要让另一个 queue 格式再白等一整个 phase（主队列是共享资源）。
                    board.Fail(
                        $"scenario.fail-fast-skipped.{phase.Name}",
                        $"{phase.Name} 阶段失败 ⇒ 提前中止，未验证的后续 phase 一律不声称覆盖（本轮未跑到的不算已测）。");
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            board.Fail("scenario.unhandled", $"{ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            // 有界清理：只处理本探针自己 Process.Start 出来的进程对象，绝不按 PID 杀、绝不 taskkill。
            var killed = new List<int>();
            foreach (var p in created)
            {
                try
                {
                    if (!p.HasExited)
                    {
                        p.Kill(entireProcessTree: false);
                        killed.Add(p.Id);
                    }
                }
                catch (Exception ex)
                {
                    board.Fail("cleanup.kill-failed", $"pid={TryId(p)} {ex.GetType().Name}: {ex.Message}");
                }
                finally
                {
                    p.Dispose();
                }
            }

            board.CleanupKilledPids = killed;
            board.CleanupMode = "bounded: 仅本探针 Process.Start 得到的副 Worker 进程对象；未删除任何文件、未触碰主 Worker/默认 runs-root";
            board.ShutdownMarkersWritten = shutdownRequested;

            board.MainWorkerAfter = DescribeProcess(board.MainPid);
            board.DefaultRunsRootAfter = DescribeRunsRoot(board.DefaultRunsRoot);
            board.Check(
                "safety.main-worker-unchanged",
                SameProcess(board.MainWorkerBefore, board.MainWorkerAfter),
                $"before={board.MainWorkerBefore} after={board.MainWorkerAfter}");
            board.Check(
                "safety.default-runs-root-untouched",
                SameRunsRoot(board.DefaultRunsRootBefore, board.DefaultRunsRootAfter),
                $"before={board.DefaultRunsRootBefore} after={board.DefaultRunsRootAfter}");
            board.FinishedUtc = DateTimeOffset.UtcNow;
        }

        return board.Emit(board.AllPassed ? 0 : 1);
    }

    private static async Task<bool> RunPhaseAsync(Board board, PhaseSpec phase, ScenarioOptions opts, List<Process> created, Action<string> onShutdown)
    {
        var tag = phase.Name;
        // 三个目录互不重叠，且 runs-root 交给 Worker 自己新建+写 marker（探针绝不预先污染它）。
        board.Check($"phase.{tag}.paths-unique-and-new",
            IsNewDir(phase.QueueRoot) && IsNewDir(phase.RunsRoot) && IsNewDir(phase.ProbeDir)
            && !Overlaps(phase.QueueRoot, phase.RunsRoot) && !Overlaps(phase.RunsRoot, board.ToolDir)
            && !Overlaps(phase.RunsRoot, phase.ProbeDir) && !Overlaps(phase.QueueRoot, phase.ProbeDir),
            $"queue={phase.QueueRoot} (unc={LooksLikeUnc(phase.QueueRoot)}) runsRoot={phase.RunsRoot} probeDir={phase.ProbeDir} toolDir={board.ToolDir}");

        Directory.CreateDirectory(BridgeLayout.JobsDir(phase.QueueRoot));
        Directory.CreateDirectory(phase.ProbeDir);

        Progress("worker1-start", tag);
        var worker1 = StartWorker(board, phase, opts, created, tag);
        if (worker1 is null)
        {
            return false;
        }

        board.Check($"phase.{tag}.worker1-starts",
            await WaitAsync(() => ProbeAssets.TryReadWorkerState(phase.QueueRoot) is { Pid: > 0 }, opts.StartTimeoutSeconds).ConfigureAwait(false),
            $"worker.json={Shape(ProbeAssets.TryReadWorkerState(phase.QueueRoot))}");

        // ── 任务 J1：跑起来（nested assign 证明）→ 强杀副 Worker（KILL_ON_JOB_CLOSE 证明）──
        var j1 = ProbeAssets.NewJobId("lifecycle1");
        var pids1 = Path.Combine(phase.ProbeDir, "pids-j1.txt");
        ProbeAssets.SubmitRunJob(phase.QueueRoot, j1, ["--pidfile", pids1, "--repeat", "--spawn", "1", "--levels", "3", "--heartbeat-ms", "60000"], timeoutSeconds: 0);
        Progress("j1-submitted", $"{tag} {j1} pidfile={pids1}");

        var up1 = await WaitTreeUpAsync(phase.QueueRoot, j1, pids1, opts.RunTimeoutSeconds).ConfigureAwait(false);
        Progress(up1.Ok ? "j1-tree-complete" : "j1-tree-incomplete", $"{tag} {up1.Detail}");
        board.PhaseFacts[$"{tag}.j1"] = new { jobId = j1, status = Shape(ProbeAssets.TryReadStatus(phase.QueueRoot, j1)), entries = Describe(up1.Entries), alive = up1.Alive, detail = up1.Detail };
        if (!up1.Ok)
        {
            // 树没起全就不许往下走：否则"空集合确认 Gone"会变成假绿的已测结论。
            board.Fail($"phase.{tag}.j1-three-level-tree-complete", $"未观察到完整三层树（root+child+grandchild），本阶段后续检查一律不做。{up1.Detail}");
            return false;
        }

        board.Check($"phase.{tag}.j1-three-level-tree-complete", true,
            $"恰好 3 个进程、深度 0/1/2 各一、ppid 逐层相接、全部存活：[{Describe(up1.Entries)}]；副队列任务状态=running（⇒ 副 Worker 正在执行该任务，即它在外层主 Job 之内成功建了自己的嵌套 Job 并把子进程 Assign 了进去）");

        // 直接观测 Job 归属（不靠架构假设）：IsProcessInJob(handle, NULL) 判定"是否在调用方所在 Job"。
        var (probeQueryOk, probeIn) = QueryCallerJobMembership(GetCurrentProcess());
        var (workerQueryOk, workerIn) = QueryCallerJobMembership(worker1.Handle);
        var echoOk = TryIsPidInCallerJob(up1.Entries[0].Pid, out var echoInCallerJob, out var echoErr);
        board.PhaseFacts[$"{tag}.nested"] = new
        {
            probeQueryOk = probeQueryOk,
            probeInCallerJob = probeIn,
            workerQueryOk = workerQueryOk,
            workerInCallerJob = workerIn,
            echoAppInCallerJob = echoOk ? echoInCallerJob : (bool?)null,
            echoQuery = echoErr,
            note = "嵌套 Job 是层级包含关系：内层 Job 的进程在外层 Job 查询同样为 TRUE，故 echoAppInCallerJob 不能区分「嵌套」与「同一个 Job」；嵌套的判别证据是 hardkill 级联。",
        };
        board.Check($"phase.{tag}.nested.probe-and-worker-in-outer-job", probeQueryOk && probeIn && workerQueryOk && workerIn,
            $"IsProcessInJob(probe,NULL)={probeIn}(ok={probeQueryOk}) IsProcessInJob(副Worker,NULL)={workerIn}(ok={workerQueryOk}) ⇒ 副 Worker 确实运行在外层主 Job 之内（它继承了主 Job，却又为自己的子进程建了独立 Job）");

        var j1Tree = up1.Entries.Select(e => e.Pid).ToList();
        var killOk = KillOwnedWorker(board, worker1, tag, "worker1");
        var gone1 = await WaitPidsGoneAsync(j1Tree, opts.GoneTimeoutSeconds).ConfigureAwait(false);
        board.PhaseFacts[$"{tag}.hardkill"] = new { jobId = j1, killedWorkerPid = TryId(worker1), tree = j1Tree, gone = gone1.Gone, queryErrors = gone1.QueryErrors, detail = gone1.Detail };
        Progress("j1-hardkilled", $"{tag} workerPid={TryId(worker1)} killOk={killOk}");
        board.Check($"phase.{tag}.hardkill-kills-worker", killOk, $"只 Kill 本探针 Process.Start 得到的进程对象（Kill(entireProcessTree:false)）");
        board.Check($"phase.{tag}.hardkill-tree-gone-kill-on-job-close", gone1.AllGone && gone1.QueryErrors == 0 && gone1.Gone.Count == ExpectedLevels,
            $"**嵌套 Job 的判别证据**：只强杀副 Worker（不碰任何子进程），它自建 Job 的句柄随之关闭 ⇒ KILL_ON_JOB_CLOSE 收走完整三层（期望 {ExpectedLevels} 个，实测 gone={string.Join(',', gone1.Gone)} queryErrors={gone1.QueryErrors}）；若这三层只在外层主 Job 里，它们此刻必须还活着。{gone1.Detail}");
        board.Check($"phase.{tag}.hardkill-outer-job-survives", IsAlive(board.ProbePid).Alive && IsAlive(opts.MainPid).Alive,
            "探针自身与主 Worker 均在强杀副 Worker 后存活 ⇒ 嵌套 Job 没有波及外层");

        // ── 重启同一 queue：旧任务 interrupted、绝不重放 ──
        var beforeReplay = ProbeAssets.ReadPids(pids1).Count;
        Progress("worker2-start", tag);
        var worker2 = StartWorker(board, phase, opts, created, tag, "worker2");
        if (worker2 is null)
        {
            return false;
        }

        var interrupted = await WaitAsync(
            () => ProbeAssets.TryReadStatus(phase.QueueRoot, j1)?.State == JobStates.Interrupted,
            opts.StateTimeoutSeconds).ConfigureAwait(false);
        var status1 = ProbeAssets.TryReadStatus(phase.QueueRoot, j1);
        var afterReplay = ProbeAssets.ReadPids(pids1).Count;
        var lingering = new List<int>();
        foreach (var pid in j1Tree)
        {
            if (IsAlive(pid).Alive)
            {
                lingering.Add(pid);
            }
        }

        Progress("j1-restart-observed", $"{tag} state={status1?.State} interrupted={interrupted}");
        board.Check($"phase.{tag}.restart-marks-interrupted", interrupted && status1?.FinishedUtc is not null,
            $"state={status1?.State} finishedUtc={status1?.FinishedUtc:O} error={status1?.Error}");
        board.Check($"phase.{tag}.restart-never-replays", afterReplay == beforeReplay && lingering.Count == 0,
            $"pidfile 条数 before={beforeReplay} after={afterReplay}（未重放 ⇒ 没有新的 EchoApp 进程）；残留存活={string.Join(',', lingering)}");
        board.Check($"phase.{tag}.restart-worker-healthy-idle",
            ProbeAssets.TryReadWorkerState(phase.QueueRoot) is { CurrentJobId: null, Pid: > 0 } ws2 && ws2.Pid != TryId(worker1),
            $"worker.json={Shape(ProbeAssets.TryReadWorkerState(phase.QueueRoot))}（新 PID，与原副 Worker {TryId(worker1)} 不同）");

        // ── 任务 J2：shutdown 标记必须主动清树并让副 Worker 退出 ──
        var j2 = ProbeAssets.NewJobId("lifecycle2");
        var pids2 = Path.Combine(phase.ProbeDir, "pids-j2.txt");
        ProbeAssets.SubmitRunJob(phase.QueueRoot, j2, ["--pidfile", pids2, "--repeat", "--spawn", "1", "--levels", "3", "--heartbeat-ms", "60000"], timeoutSeconds: 0);

        Progress("j2-submitted", $"{tag} {j2} pidfile={pids2}");
        var up2 = await WaitTreeUpAsync(phase.QueueRoot, j2, pids2, opts.RunTimeoutSeconds).ConfigureAwait(false);
        Progress(up2.Ok ? "j2-tree-complete" : "j2-tree-incomplete", $"{tag} {up2.Detail}");
        if (!up2.Ok)
        {
            board.Fail($"phase.{tag}.j2-three-level-tree-complete", $"未观察到完整三层树，shutdown 清树无从验证，本阶段后续检查一律不做。{up2.Detail}");
            return false;
        }

        board.Check($"phase.{tag}.j2-three-level-tree-complete", true, $"[{Describe(up2.Entries)}]");

        onShutdown(tag);
        ProbeAssets.WriteLast(phase.QueueRoot, BridgeLayout.ShutdownFileName, string.Empty);
        Progress("shutdown-marker-written", tag);

        var shutdownState = await WaitAsync(
            () => ProbeAssets.TryReadStatus(phase.QueueRoot, j2)?.State == JobStates.Interrupted,
            opts.StateTimeoutSeconds).ConfigureAwait(false);
        var gone2 = await WaitPidsGoneAsync(up2.Entries.Select(e => e.Pid).ToList(), opts.GoneTimeoutSeconds).ConfigureAwait(false);
        var worker2Exited = await WaitAsync(() => TryHasExited(worker2), opts.StateTimeoutSeconds).ConfigureAwait(false);
        var status2 = ProbeAssets.TryReadStatus(phase.QueueRoot, j2);

        board.PhaseFacts[$"{tag}.shutdown"] = new { jobId = j2, status = Shape(status2), entries = Describe(up2.Entries), gone = gone2.Gone, queryErrors = gone2.QueryErrors, workerExitCode = TryExitCode(worker2) };
        Progress("shutdown-observed", $"{tag} state={status2?.State} gone={gone2.Gone.Count}/{ExpectedLevels} workerExited={worker2Exited}");
        board.Check($"phase.{tag}.shutdown-clears-tree", shutdownState && gone2.AllGone && gone2.QueryErrors == 0 && gone2.Gone.Count == ExpectedLevels,
            $"state={status2?.State}（shutdown ⇒ interrupted）gone={string.Join(',', gone2.Gone)} queryErrors={gone2.QueryErrors}。{gone2.Detail}");
        board.Check($"phase.{tag}.shutdown-worker-exits-clean", worker2Exited && TryExitCode(worker2) == 0,
            $"副 Worker 退出码={TryExitCode(worker2)}（Worker 退出码不随任务成败变化）");

        return board.Checks.Where(c => c.Name.StartsWith($"phase.{tag}.", StringComparison.Ordinal)).All(c => c.Status != "fail");
    }

    // ── 副 Worker 的启停（唯一允许的进程操作面：自己 Process.Start 的对象） ──

    private static Process? StartWorker(Board board, PhaseSpec phase, ScenarioOptions opts, List<Process> created, string tag, string which = "worker1")
    {
        if (!ResolveSelfCommand(out var exe, out var arg0))
        {
            board.Fail($"phase.{tag}.{which}-self-exe", "无法解析自身可执行文件：Environment.ProcessPath 为空");
            return null;
        }

        var psi = new ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = board.Cwd,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        if (arg0 is not null)
        {
            psi.ArgumentList.Add(arg0);
        }

        psi.ArgumentList.Add("--secondary-worker");
        psi.ArgumentList.Add("--queue");
        psi.ArgumentList.Add(phase.QueueRoot);
        psi.ArgumentList.Add("--runs-root");
        psi.ArgumentList.Add(phase.RunsRoot);

        // 主桥可能用 `dotnet <dll>` 拉起本探针：此时 ProcessPath 就是 dotnet 宿主本身，
        // 把它原样透传给副 Worker，避免副 Worker 依赖 Windows PATH 里的 dotnet。
        var dotnetForSecondary = opts.Dotnet.Length > 0
            ? opts.Dotnet
            : (IsDotnetHost(board.ProcessPath) ? board.ProcessPath : string.Empty);
        if (dotnetForSecondary.Length > 0)
        {
            psi.ArgumentList.Add("--dotnet");
            psi.ArgumentList.Add(dotnetForSecondary);
        }

        try
        {
            var process = Process.Start(psi);
            if (process is null)
            {
                board.Fail($"phase.{tag}.{which}-start", "Process.Start 返回 null");
                return null;
            }

            created.Add(process);

            // 副 Worker 的 stdout/stderr 落到本阶段的 probeDir（runs-root 归 Worker 独占）。
            var logDir = phase.ProbeDir;
            Directory.CreateDirectory(logDir);
            PumpTo(process.StandardOutput, Path.Combine(logDir, $"{which}.stdout.log"));
            PumpTo(process.StandardError, Path.Combine(logDir, $"{which}.stderr.log"));

            board.Check($"phase.{tag}.{which}-started",
                true,
                $"pid={process.Id} queue={phase.QueueRoot} runsRoot={phase.RunsRoot} cmd={exe} {string.Join(' ', psi.ArgumentList)}");
            return process;
        }
        catch (Exception ex)
        {
            board.Fail($"phase.{tag}.{which}-start", $"{ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private static void PumpTo(StreamReader reader, string path)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await using var writer = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), ProbeAssets.Utf8NoBom);
                var buffer = new char[4096];
                int read;
                while ((read = await reader.ReadAsync(buffer).ConfigureAwait(false)) > 0)
                {
                    await writer.WriteAsync(buffer.AsMemory(0, read)).ConfigureAwait(false);
                    await writer.FlushAsync().ConfigureAwait(false);
                }
            }
            catch (Exception)
            {
                // 读端被拆掉属正常收尾。
            }
        });
    }

    /// <summary>只对本探针自己 Process.Start 得到的对象调用 Kill(entireProcessTree:false)。</summary>
    private static bool KillOwnedWorker(Board board, Process process, string tag, string which)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: false);
            }

            var exited = process.WaitForExit(30_000);
            board.Check($"phase.{tag}.{which}-kill-owned-only", exited,
                $"pid={process.Id} Kill(false) 后 30s 内退出={exited}（仅此对象，未用 taskkill、未按持久化 PID 杀）");
            return exited;
        }
        catch (Exception ex)
        {
            board.Fail($"phase.{tag}.{which}-kill-owned-only", $"{ex.GetType().Name}: {ex.Message}");
            return false;
        }
    }

    // ── 等待与只读探测 ──────────────────────────────────────────────────────

    /// <summary>
    /// 等到**完整的三层树**：副队列任务状态=running、pidfile 里 3 个进程深度齐全且 ppid 相接、且全部存活。
    /// 刻意不把 status.Pid 当判据：Worker 的 status.json 在阶段转换时才落盘，运行中 pid 仍可能为空
    /// （第一版就因此把"树其实已经起全"误判成超时）。
    /// 拿不到就返回 Ok=false —— 调用方必须立刻判失败并停止，绝不能拿空集合去"确认 Gone"。
    /// </summary>
    private static async Task<TreeUp> WaitTreeUpAsync(string queueRoot, string jobId, string pidFile, int timeoutSeconds)
    {
        var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
        string detail = "(未开始)";
        while (DateTime.UtcNow < deadline)
        {
            var status = ProbeAssets.TryReadStatus(queueRoot, jobId);
            var entries = ProbeAssets.ReadPidEntries(pidFile)
                .GroupBy(e => e.Pid)
                .Select(g => g.First())
                .ToList();
            var alive = entries.Where(e => IsAlive(e.Pid).Alive).Count();
            var chainOk = ChainComplete(entries, out var why);

            detail = $"state={status?.State} phase={status?.Phase} statusPid={status?.Pid ?? -1}（运行中可能尚未落盘，不作判据） exit={status?.ExitCode} entries=[{Describe(entries)}] alive={alive} chainOk={chainOk}{(chainOk ? string.Empty : " why=" + why)}";
            if (status?.State == JobStates.Running && chainOk && alive == entries.Count)
            {
                return new TreeUp(true, entries, alive, detail);
            }

            if (status is not null && JobStates.IsTerminal(status.State))
            {
                return new TreeUp(false, entries, alive, $"任务提前终态：{detail}");
            }

            await Task.Delay(500).ConfigureAwait(false);
        }

        return new TreeUp(false, ProbeAssets.ReadPidEntries(pidFile), 0, $"等待超时（{timeoutSeconds}s）：{detail}");
    }

    private static async Task<GoneResult> WaitPidsGoneAsync(List<int> pids, int timeoutSeconds)
    {
        var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
        var gone = new List<int>();
        var queryErrors = 0;
        string detail = string.Empty;
        while (true)
        {
            gone.Clear();
            queryErrors = 0;
            var alive = new List<int>();
            foreach (var pid in pids)
            {
                var (aliveNow, error) = IsAlive(pid);
                if (aliveNow)
                {
                    alive.Add(pid);
                }
                else if (error is null)
                {
                    gone.Add(pid);
                }
                else
                {
                    queryErrors++;
                    detail = $"pid={pid} 查询异常：{error}";
                }
            }

            if (alive.Count == 0)
            {
                return new GoneResult(true, gone, queryErrors, detail);
            }

            if (DateTime.UtcNow >= deadline)
            {
                return new GoneResult(false, gone, queryErrors, $"超时 {timeoutSeconds}s 仍有存活：{string.Join(',', alive)}");
            }

            await Task.Delay(400).ConfigureAwait(false);
        }
    }

    private static async Task<bool> WaitAsync(Func<bool> condition, int timeoutSeconds)
    {
        var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (condition())
                {
                    return true;
                }
            }
            catch (Exception)
            {
                // 读到半截文件属正常，下一轮再试。
            }

            await Task.Delay(300).ConfigureAwait(false);
        }

        try
        {
            return condition();
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>只读存活探测：进程不存在 = 诚实返回 Gone；其它异常计为 query error（必须为 0）。</summary>
    private static (bool Alive, string? Error) IsAlive(int? pid)
    {
        if (pid is not > 0)
        {
            return (false, null);
        }

        try
        {
            using var p = Process.GetProcessById(pid.Value);
            return p.HasExited ? (false, null) : (true, null);
        }
        catch (ArgumentException)
        {
            return (false, null);   // 进程不存在：正是 Gone
        }
        catch (InvalidOperationException)
        {
            return (false, null);
        }
        catch (Exception ex)
        {
            return (false, $"{ex.GetType().Name}: {ex.Message}");
        }
    }

    private static bool TryHasExited(Process? p)
    {
        try
        {
            return p?.HasExited ?? true;
        }
        catch (Exception)
        {
            return true;
        }
    }

    private static int? TryExitCode(Process? p)
    {
        try
        {
            return p is not null && p.HasExited ? p.ExitCode : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static int TryId(Process? p)
    {
        try
        {
            return p?.Id ?? -1;
        }
        catch (Exception)
        {
            return -1;
        }
    }

    /// <summary>自身可执行文件 + 可选 argv[0]（apphost 直接用；被 dotnet 宿主拉起时回退 dotnet &lt;dll&gt;）。</summary>
    private static bool ResolveSelfCommand(out string exe, out string? arg0)
    {
        var self = Environment.ProcessPath;
        if (string.IsNullOrEmpty(self))
        {
            exe = string.Empty;
            arg0 = null;
            return false;
        }

        var isDotnet = IsDotnetHost(self);
        var assembly = Assembly.GetEntryAssembly()?.Location;
        if (isDotnet && !string.IsNullOrEmpty(assembly))
        {
            exe = self;
            arg0 = assembly;
            return true;
        }

        exe = self;
        arg0 = null;
        return true;
    }

    // ── 路径/进程事实的小工具 ───────────────────────────────────────────────

    /// <summary>真正的 UNC（\\host\share\...）；\\?\ 与 \\.\ 是设备命名空间，不算 UNC。</summary>
    /// <summary>
    /// 校验 pidfile 里是**完整的三层链**：恰好 3 个进程、深度各为 0/1/2、且 ppid 逐层相接。
    /// 只数"≥3 个 PID"会假绿：root + 两个直子同样满足 3，却证明不了孙层真的起来了。
    /// </summary>
    private static bool ChainComplete(List<ProbeAssets.PidEntry> entries, out string why)
    {
        why = string.Empty;
        if (entries.Count != ExpectedLevels)
        {
            why = $"进程数 {entries.Count} != 期望 {ExpectedLevels}";
            return false;
        }

        var byDepth = new Dictionary<int, List<ProbeAssets.PidEntry>>();
        foreach (var e in entries)
        {
            if (!byDepth.TryGetValue(e.Depth, out var list))
            {
                list = [];
                byDepth[e.Depth] = list;
            }

            list.Add(e);
        }

        for (var depth = 0; depth < ExpectedLevels; depth++)
        {
            if (!byDepth.TryGetValue(depth, out var list) || list.Count != 1)
            {
                why = $"深度 {depth} 的进程数 != 1（实际 {(byDepth.TryGetValue(depth, out var l) ? l.Count : 0)}）；entries={Describe(entries)}";
                return false;
            }
        }

        if (byDepth[1][0].ParentPid != byDepth[0][0].Pid)
        {
            why = $"depth1.ppid={byDepth[1][0].ParentPid} != depth0.pid={byDepth[0][0].Pid}；entries={Describe(entries)}";
            return false;
        }

        if (byDepth[2][0].ParentPid != byDepth[1][0].Pid)
        {
            why = $"depth2.ppid={byDepth[2][0].ParentPid} != depth1.pid={byDepth[1][0].Pid}；entries={Describe(entries)}";
            return false;
        }

        return true;
    }

    private static string Describe(List<ProbeAssets.PidEntry> entries)
        => string.Join(';', entries.Select(e => $"(pid={e.Pid},depth={e.Depth},ppid={e.ParentPid})"));

    /// <summary>
    /// IsProcessInJob(handle, NULL, out r)：NULL 表示"调用方所在的那个 Job"。
    /// 注意 GetCurrentProcess() 返回的是伪句柄 (HANDLE)-1，它**合法**，不能当非法句柄拒掉
    /// （第一版正是把它拒了，于是探针自己永远报告"不在 Job 内"）。
    /// </summary>
    private static (bool Ok, bool InJob) QueryCallerJobMembership(IntPtr processHandle)
    {
        if (processHandle == IntPtr.Zero)
        {
            return (false, false);
        }

        try
        {
            var ok = IsProcessInJob(processHandle, IntPtr.Zero, out var inJob);
            return (ok, inJob);
        }
        catch (Exception)
        {
            return (false, false);
        }
    }

    private static bool TryIsPidInCallerJob(int pid, out bool inJob, out string? error)
    {
        inJob = false;
        error = null;
        var handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, bInheritHandle: false, pid);
        if (handle == IntPtr.Zero)
        {
            error = $"OpenProcess(pid={pid}) 失败 win32={Marshal.GetLastWin32Error()}";
            return false;
        }

        try
        {
            return IsProcessInJob(handle, IntPtr.Zero, out inJob);
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    private static bool IsDotnetHost(string processPath)
        => processPath.EndsWith("dotnet", StringComparison.OrdinalIgnoreCase)
           || processPath.EndsWith("dotnet.exe", StringComparison.OrdinalIgnoreCase);

    private static bool LooksLikeUnc(string path)
        => path.StartsWith(@"\\", StringComparison.Ordinal)
           && !path.StartsWith(@"\\?\", StringComparison.Ordinal)
           && !path.StartsWith(@"\\.\", StringComparison.Ordinal);

    private static bool IsNewDir(string path) => !Directory.Exists(path) || !Directory.EnumerateFileSystemEntries(path).Any();

    private static bool Overlaps(string a, string b)
    {
        static string Norm(string p) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(p)) + Path.DirectorySeparatorChar;
        string na, nb;
        try
        {
            na = Norm(a);
            nb = Norm(b);
        }
        catch (Exception)
        {
            return false;
        }

        return na.StartsWith(nb, StringComparison.OrdinalIgnoreCase) || nb.StartsWith(na, StringComparison.OrdinalIgnoreCase);
    }

    private static bool Throws<T>(Action action) where T : Exception
    {
        try
        {
            action();
            return false;
        }
        catch (T)
        {
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static void TryDeleteDir(string path)
    {
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    Directory.Delete(path, recursive: true);
                }

                return;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Thread.Sleep(100);
            }
        }
    }

    private static string DefaultRunsRoot()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return string.IsNullOrEmpty(local) ? "(unavailable)" : Path.Combine(local, "WslDebugBridge", "runs");
    }

    private static string DescribeProcess(int pid)
    {
        if (pid <= 0)
        {
            return "(未提供 --main-pid)";
        }

        try
        {
            using var p = Process.GetProcessById(pid);
            return $"pid={p.Id} name={p.ProcessName} startUtc={p.StartTime.ToUniversalTime():O} hasExited={p.HasExited}";
        }
        catch (Exception ex)
        {
            return $"pid={pid} 不可读：{ex.GetType().Name}: {ex.Message}";
        }
    }

    private static bool SameProcess(string before, string after)
        => !before.Contains("未提供", StringComparison.Ordinal)
           && !before.Contains("不可读", StringComparison.Ordinal)
           && !after.Contains("不可读", StringComparison.Ordinal)
           && string.Equals(before, after, StringComparison.Ordinal);   // 占位串相等绝不算"主 Worker 未变"

    private static string DescribeRunsRoot(string path)
    {
        try
        {
            if (!Directory.Exists(path))
            {
                return $"{path} | 不存在";
            }

            var marker = Path.Combine(path, ".wsl-debug-bridge-runs-owner");
            var markerState = File.Exists(marker)
                ? $"marker mtime={File.GetLastWriteTimeUtc(marker):O} len={new FileInfo(marker).Length}"
                : "marker 缺失";
            return $"{path} | 存在 | 顶层条目={Directory.EnumerateFileSystemEntries(path).Count()} | {markerState}";
        }
        catch (Exception ex)
        {
            return $"{path} | 读取失败：{ex.GetType().Name}: {ex.Message}";
        }
    }

    private static bool SameRunsRoot(string before, string after) => string.Equals(before, after, StringComparison.Ordinal);

    private static string Shape(Status? s)
        => s is null ? "(无 status.json)" : $"state={s.State} phase={s.Phase} pid={s.Pid} exit={s.ExitCode} error={s.Error}";

    private static string Shape(WorkerState? w)
        => w is null ? "(无 worker.json)" : $"pid={w.Pid} workerId={w.WorkerId} currentJob={w.CurrentJobId} queue={w.QueueRoot} runsRoot={w.RunsRoot}";

    // ── 夹具级回归：自带 EchoApp 备份真的能起三层树（首轮 5 分钟空等的根因） ──

    private const string SpawnCheckPrefix = "WDB_LIFECYCLE_SPAWNCHECK ";

    /// <summary>
    /// 把自带 EchoApp 备份写到临时目录，用 `dotnet build` 构建，再以
    /// `dotnet &lt;dll&gt; --pidfile … --repeat --spawn 1 --depth 2` 启动（与 Windows 上桥的启动方式
    /// 完全一致），断言 pidfile 里出现**完整三层**（深度 0/1/2 各一 + ppid 相接）。
    /// 这正是首轮失败的地方：子进程 argv[0] 用了相对 dll 名，子/孙从未起来，pidfile 只有 1 行。
    /// 跨平台可跑；不含任何 Job 语义断言。
    /// </summary>
    private static async Task<int> RunEchoAppSpawnCheckAsync()
    {
        var board = new Board
        {
            Probe = "WorkerLifecycleProbe/v1/echoapp-spawn",
            StartedUtc = DateTimeOffset.UtcNow,
            Host = RuntimeInformation.OSDescription,
            IsWindows = OperatingSystem.IsWindows(),
            Cwd = Environment.CurrentDirectory,
            ToolDir = AppContext.BaseDirectory,
            ProbePid = Environment.ProcessId,
        };

        var root = Path.Combine(Path.GetTempPath(), "wdb-echoapp-spawn-" + Guid.NewGuid().ToString("N")[..8]);
        Process? spawner = null;
        try
        {
            var projectDir = Path.Combine(root, "EchoApp");
            Directory.CreateDirectory(projectDir);
            File.WriteAllText(Path.Combine(projectDir, "EchoApp.csproj"), ProbeAssets.EchoAppCsproj, ProbeAssets.Utf8NoBom);
            File.WriteAllText(Path.Combine(projectDir, "Program.cs"), ProbeAssets.EchoAppProgram, ProbeAssets.Utf8NoBom);

            var build = await RunDotnetAsync(["build", Path.Combine(projectDir, "EchoApp.csproj"), "-c", "Release", "-v", "q", "--nologo"], root).ConfigureAwait(false);
            board.Check("spawncheck.echoapp-builds", build.ExitCode == 0, $"exit={build.ExitCode} {Tail(build.Output, 300)}");
            if (build.ExitCode != 0)
            {
                return board.EmitSpawnCheck(1);
            }

            var dll = Path.Combine(projectDir, "bin", "Release", "net10.0", "EchoApp.dll");
            var pidFile = Path.Combine(root, "pids.txt");
            var logFile = Path.Combine(root, "spawner.log");
            var psi = new ProcessStartInfo
            {
                FileName = ResolveDotnetForSelf(),
                WorkingDirectory = projectDir,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            foreach (var a in new[] { dll, "--pidfile", pidFile, "--repeat", "--repeat-seconds", "12", "--spawn", "1", "--levels", "3", "--heartbeat-ms", "2000" })
            {
                psi.ArgumentList.Add(a);
            }

            spawner = Process.Start(psi);
            if (spawner is null)
            {
                board.Fail("spawncheck.spawner-starts", "Process.Start 返回 null");
                return board.EmitSpawnCheck(1);
            }

            PumpTo(spawner.StandardOutput, logFile + ".out");
            PumpTo(spawner.StandardError, logFile + ".err");

            var up = await WaitTreeUpAsyncForRoot(root, pidFile, timeoutSeconds: 60).ConfigureAwait(false);
            board.Check("spawncheck.three-level-tree-complete", up.Ok, up.Detail);
            board.Check("spawncheck.three-level-all-alive", up.Ok && up.Alive == ExpectedLevels, $"{Describe(up.Entries)} alive={up.Alive}");

            // 根进程 Kill 后，Linux 上没有 Job ⇒ 子孙只会因自身 12s 窗口自然退出；等它们全走完。
            try
            {
                if (!spawner.HasExited)
                {
                    spawner.Kill(entireProcessTree: false);
                }
            }
            catch (Exception ex)
            {
                board.Fail("spawncheck.kill-root", $"{ex.GetType().Name}: {ex.Message}");
            }

            var goneWait = await WaitPidsGoneAsync(up.Entries.Select(e => e.Pid).ToList(), 45).ConfigureAwait(false);
            board.Check("spawncheck.all-gone-after-window", goneWait.AllGone && goneWait.QueryErrors == 0,
                $"gone={goneWait.Gone.Count}/{up.Entries.Count} queryErrors={goneWait.QueryErrors}。{goneWait.Detail}");
        }
        catch (Exception ex)
        {
            board.Fail("spawncheck.unhandled", $"{ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            try
            {
                if (spawner is { HasExited: false })
                {
                    spawner.Kill(entireProcessTree: false);
                }

                spawner?.Dispose();
            }
            catch (Exception)
            {
            }

            TryDeleteDir(root);
        }

        return board.EmitSpawnCheck(board.AllPassed ? 0 : 1);
    }

    private static async Task<TreeUp> WaitTreeUpAsyncForRoot(string root, string pidFile, int timeoutSeconds)
    {
        var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
        string detail = "(未开始)";
        while (DateTime.UtcNow < deadline)
        {
            var entries = ProbeAssets.ReadPidEntries(pidFile).GroupBy(e => e.Pid).Select(g => g.First()).ToList();
            var alive = entries.Where(e => IsAlive(e.Pid).Alive).Count();
            var chainOk = ChainComplete(entries, out var why);
            detail = $"entries=[{Describe(entries)}] alive={alive} chainOk={chainOk}{(chainOk ? string.Empty : " why=" + why)}";
            if (chainOk && alive == entries.Count)
            {
                return new TreeUp(true, entries, alive, detail);
            }

            await Task.Delay(300).ConfigureAwait(false);
        }

        return new TreeUp(false, ProbeAssets.ReadPidEntries(pidFile), 0, $"等待超时（{timeoutSeconds}s）：{detail}");
    }

    private static string LastSpawnCheckLog = string.Empty;

    private static async Task<(int ExitCode, string Output)> RunDotnetAsync(string[] args, string workingDir)
    {
        var psi = new ProcessStartInfo
        {
            FileName = ResolveDotnetForSelf(),
            WorkingDirectory = workingDir,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (var a in args)
        {
            psi.ArgumentList.Add(a);
        }

        using var p = Process.Start(psi)!;
        var output = await p.StandardOutput.ReadToEndAsync().ConfigureAwait(false);
        var error = await p.StandardError.ReadToEndAsync().ConfigureAwait(false);
        p.WaitForExit();
        LastSpawnCheckLog = output + error;
        return (p.ExitCode, LastSpawnCheckLog);
    }

    private static string ResolveDotnetForSelf()
    {
        var self = Environment.ProcessPath ?? string.Empty;
        return IsDotnetHost(self) ? self : "dotnet";
    }

    private static string Tail(string text, int max)
        => text.Length <= max ? text : text[^max..];

    // ── 结构性自测（Linux 可跑；不启动任何进程） ────────────────────────────

    private static int RunSelfTest(string[] args)
    {
        var board = new Board();
        board.Probe = "WorkerLifecycleProbe/v1/selftest";
        board.StartedUtc = DateTimeOffset.UtcNow;
        board.Host = RuntimeInformation.OSDescription;
        board.IsWindows = OperatingSystem.IsWindows();
        board.Cwd = Environment.CurrentDirectory;
        board.ToolDir = AppContext.BaseDirectory;
        board.ProcessPath = Environment.ProcessPath ?? "(null)";
        board.ProbePid = Environment.ProcessId;

        try
        {
            board.Check("selftest.run-async-is-public-api",
                typeof(Worker).GetMethod("RunAsync", BindingFlags.Public | BindingFlags.Static) is { IsPublic: true, IsStatic: true },
                "副 Worker 子模式依赖公共 API Worker.RunAsync(string[]?)");

            var workerRunner = typeof(Worker).Assembly.GetType("GalleriesManager.Wdb.JobRunnerGateway");
            board.Check("selftest.runner-factory-registered",
                workerRunner?.GetProperty("WindowsProcessFactory", BindingFlags.Public | BindingFlags.Static)?.GetValue(null) is not null,
                "WindowsJobRunner 的模块初始化器必须已把工厂接上（否则副 Worker 会以 ERR_JOB_RUNNER_UNAVAILABLE 熔断）");

            board.Check("selftest.echoapp-assets-present",
                ProbeAssets.EchoAppCsproj.Contains("<TargetFramework>net10.0</TargetFramework>", StringComparison.Ordinal)
                && ProbeAssets.EchoAppProgram.Contains("WDB_ECHO_PID", StringComparison.Ordinal)
                && ProbeAssets.EchoAppProgram.Contains("--pidfile", StringComparison.Ordinal)
                && !ProbeAssets.EchoAppCsproj.StartsWith('\uFEFF'),
                $"csproj={ProbeAssets.EchoAppCsproj.Length}B program={ProbeAssets.EchoAppProgram.Length}B（自有备份源，不读桥的 fixtures/EchoApp）");

            var root = args.Length > 0 && args[0] is { Length: > 0 } p ? p : Path.Combine(Path.GetTempPath(), "wdb-lifecycle-selftest-" + Guid.NewGuid().ToString("N")[..8]);
            var queue = Path.Combine(root, "queue");
            Directory.CreateDirectory(BridgeLayout.JobsDir(queue));

            var id = ProbeAssets.NewJobId("selftest");
            var request = ProbeAssets.SubmitRunJob(queue, id, ["--pidfile", Path.Combine(root, "pids.txt"), "--repeat", "--spawn", "1", "--levels", "3"], timeoutSeconds: 0);

            var jobDir = BridgeLayout.JobDir(queue, id);
            var payloadDir = Path.Combine(jobDir, BridgeLayout.PayloadDirName);
            var files = Directory.EnumerateFiles(payloadDir, "*", SearchOption.AllDirectories)
                .Select(f => Path.GetRelativePath(payloadDir, f).Replace('\\', '/'))
                .OrderBy(f => f, StringComparer.Ordinal)
                .ToArray();
            board.Check("selftest.payload-layout",
                files.SequenceEqual(new[] { "EchoApp/EchoApp.csproj", "EchoApp/Program.cs" }),
                $"payload 文件={string.Join(',', files)}");

            var reread = BridgeJson.Deserialize<Request>(File.ReadAllText(BridgeLayout.RequestPath(queue, id), Encoding.UTF8));
            board.Check("selftest.request-roundtrip",
                reread is { ProtocolVersion: BridgeProtocol.Version, Mode: JobModes.Run, Project: "EchoApp/EchoApp.csproj" }
                && reread.Id == request.Id
                && reread.Arguments is { Length: > 0 },
                $"round-trip id={reread?.Id} mode={reread?.Mode} project={reread?.Project} argv={string.Join(' ', reread?.Arguments ?? [])}");

            var json = File.ReadAllText(BridgeLayout.RequestPath(queue, id), Encoding.UTF8);
            board.Check("selftest.request-camelCase-field-names",
                new[] { "protocolVersion", "mode", "project", "arguments", "payload", "createdUtc", "timeoutSeconds" }.All(k => json.Contains($"\"{k}\"", StringComparison.Ordinal)),
                json.Length > 400 ? json[..400] + "..." : json);

            // 三层结构校验的阳性/阴性对照（纯函数，Linux 可跑）：
            // 关键阴性样本 = root + 两个直子（恰好 3 个 PID，但不成三层）——"≥3 个 PID"式断言会在此假绿。
            var good = new List<ProbeAssets.PidEntry> { new(100, 0, -1), new(200, 1, 100), new(300, 2, 200) };
            var threeButFlat = new List<ProbeAssets.PidEntry> { new(100, 0, -1), new(200, 1, 100), new(300, 1, 100) };
            var onlyTwo = new List<ProbeAssets.PidEntry> { new(100, 0, -1), new(200, 1, 100) };
            var wrongPpid = new List<ProbeAssets.PidEntry> { new(100, 0, -1), new(200, 1, 999), new(300, 2, 200) };
            var goodOk = ChainComplete(good, out _);
            var flatOk = ChainComplete(threeButFlat, out var whyFlat);
            var twoOk = ChainComplete(onlyTwo, out var whyTwo);
            var ppidOk = ChainComplete(wrongPpid, out var whyPpid);
            board.Check("selftest.chain-complete-positive-and-negative",
                goodOk && !flatOk && !twoOk && !ppidOk,
                $"good=ok | threeButFlat=({whyFlat}) | onlyTwo=({whyTwo}) | wrongPpid=({whyPpid})");

            var pidProbePath = Path.Combine(root, "pidfile-probe.txt");
            File.WriteAllText(pidProbePath, "pid=11 depth=0 ppid=-1\npid=22 depth=1 ppid=11\npid=33 depth=2 ppid=22\n", ProbeAssets.Utf8NoBom);
            var parsedEntries = ProbeAssets.ReadPidEntries(pidProbePath);
            board.Check("selftest.pidfile-parsing",
                parsedEntries.Count == 3
                && parsedEntries[0] == new ProbeAssets.PidEntry(11, 0, -1)
                && parsedEntries[1] == new ProbeAssets.PidEntry(22, 1, 11)
                && parsedEntries[2] == new ProbeAssets.PidEntry(33, 2, 22),
                Describe(parsedEntries));

            // 缺/非法 --main-pid 必须被拒（负样本）；给了现场值必须通过（正样本）。
            var missOk = Throws<ArgumentException>(() => ScenarioOptions.Parse(["--unc-queue", @"\\wsl.localhost\ai-dev\x"]));
            var zeroOk = Throws<ArgumentException>(() => ScenarioOptions.Parse(["--unc-queue", @"\\wsl.localhost\ai-dev\x", "--main-pid", "0"]));
            var negOk = Throws<ArgumentException>(() => ScenarioOptions.Parse(["--unc-queue", @"\\wsl.localhost\ai-dev\x", "--main-pid", "-5"]));
            var posOk = ScenarioOptions.Parse(["--unc-queue", @"\\wsl.localhost\ai-dev\x", "--main-pid", "17732"]).MainPid == 17732;
            board.Check("selftest.main-pid-required-negative-and-positive", missOk && zeroOk && negOk && posOk,
                $"missing={missOk} zero={zeroOk} negative={negOk} positive={posOk}（缺 PID 一律 usage 拒绝，不再让 safety 占位串比相等）");

            board.Check("selftest.placeholder-must-not-pass-same-process",
                !SameProcess("(未提供 --main-pid)", "(未提供 --main-pid)")
                && !SameProcess("pid=1 不可读：x", "pid=1 不可读：x")
                && SameProcess("pid=1 name=wdb startUtc=T hasExited=False", "pid=1 name=wdb startUtc=T hasExited=False"),
                "占位串/不可读串相等时必须判否；只有真实的 pid/name/startUtc 三元组相同才判「未变」");

            // 冻结证据回放：用**收紧后**的 SameProcess 复核 .wdb/windows-lifecycle-validation.json 里记录的真实串。
            // 复核仍成立 ⇒ 那份 Windows PASS 不因判据收紧而作废。
            const string frozenMainWorker = "pid=17732 name=wdb startUtc=2026-10-12T03:42:03.4311971Z hasExited=False";
            const string frozenRunsRoot = "C:\\Users\\ranzon\\AppData\\Local\\WslDebugBridge\\runs | 存在 | 顶层条目=48 | marker mtime=2026-10-11T15:30:40.1499325Z len=84";
            board.Check("selftest.frozen-evidence-replay",
                SameProcess(frozenMainWorker, frozenMainWorker) && SameRunsRoot(frozenRunsRoot, frozenRunsRoot),
                "冻结证据（.wdb/windows-lifecycle-validation.json 的 mainWorkerBefore/After 与 defaultRunsRootBefore/After）在收紧后的判据下仍成立");

            board.Check("selftest.unc-detection",
                LooksLikeUnc(@"\\wsl.localhost\<distro>\<user>\galleries-manager\.wdb")
                && !LooksLikeUnc(@"C:\Users\x\.wdb")
                && !LooksLikeUnc(@"\\?\C:\temp"),
                "只有 \\\\ 开头才算 UNC（\\\\?\\ 前缀属设备路径，探针按非 UNC 处理并会被 runs-root 门禁拒绝）");

            var opRoot = Path.Combine(Path.GetTempPath(), "wdb-overlap-probe");
            board.Check("selftest.overlap-detection",
                Overlaps(Path.Combine(opRoot, "a", "b"), Path.Combine(opRoot, "a"))
                && !Overlaps(Path.Combine(opRoot, "a"), Path.Combine(opRoot, "b"))
                && Overlaps(Path.Combine(opRoot, "runs", "job"), Path.Combine(opRoot, "runs", "job", "src")),
                "queue 与 runs-root 必须双向不重叠；runs-root 不得包住工具目录");

            TryDeleteDir(root);
        }
        catch (Exception ex)
        {
            board.Fail("selftest.unhandled", $"{ex.GetType().Name}: {ex.Message}");
        }

        board.FinishedUtc = DateTimeOffset.UtcNow;
        Console.WriteLine(SelfTestPrefix + BridgeJson.Serialize(board.ToWire()));
        Console.WriteLine($"selftest: pass={board.Checks.Count(c => c.Status == "pass")} fail={board.Checks.Count(c => c.Status == "fail")}");
        return board.AllPassed ? 0 : 1;
    }

    // ── 参数与板子 ──────────────────────────────────────────────────────────

    /// <summary>进度打点：写 stdout 并立刻 flush —— 主任务的 stdout.log 里可直接看出卡在哪一步。</summary>
    private static void Progress(string step, string detail = "")
    {
        try
        {
            Console.WriteLine($"{ProgressPrefix}{DateTime.UtcNow:O} {step} {detail}".TrimEnd());
            Console.Out.Flush();
        }
        catch (IOException)
        {
            // 读端被拆掉属正常收尾。
        }
    }

    private sealed record PhaseSpec(string Name, string QueueRoot, string RunsRoot, string ProbeDir, bool IsUnc);

    private sealed record TreeUp(bool Ok, List<ProbeAssets.PidEntry> Entries, int Alive, string Detail);

    private sealed record GoneResult(bool AllGone, List<int> Gone, int QueryErrors, string Detail);

    private sealed class ScenarioOptions
    {
        public string UncQueue { get; private set; } = string.Empty;
        public int MainPid { get; private set; } = -1;
        public string Dotnet { get; private set; } = string.Empty;
        public string Stamp { get; private set; } = string.Empty;
        public int StartTimeoutSeconds { get; private set; } = 60;
        public int RunTimeoutSeconds { get; private set; } = 120;
        public int StateTimeoutSeconds { get; private set; } = 90;
        public int GoneTimeoutSeconds { get; private set; } = 60;
        public int OverallTimeoutSeconds { get; private set; } = 480;

        public static ScenarioOptions Parse(string[] args)
        {
            var o = new ScenarioOptions();
            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--unc-queue":
                        o.UncQueue = Next(args, ref i);
                        break;
                    case "--main-pid":
                        o.MainPid = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--dotnet":
                        o.Dotnet = Next(args, ref i);
                        break;
                    case "--stamp":
                        o.Stamp = Next(args, ref i);
                        break;
                    case "--start-timeout":
                        o.StartTimeoutSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--run-timeout":
                        o.RunTimeoutSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--state-timeout":
                        o.StateTimeoutSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--gone-timeout":
                        o.GoneTimeoutSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--overall-timeout":
                        o.OverallTimeoutSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    default:
                        break;
                }
            }

            if (o.MainPid <= 0)
            {
                throw new ArgumentException(
                    "缺少或非法的 --main-pid：本探针必须核对「主 Worker 未被停」，没有现场 PID 就无从核对，"
                    + "拒绝执行（驱动脚本应从 <queue>/worker.json 读取当前 pid 传进来；不要写死陈旧 PID）。");
            }

            return o;
        }

        private static string Next(string[] args, ref int i)
            => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"参数 {args[i]} 缺少取值");
    }

    private sealed class Board
    {
        public string Probe = string.Empty;
        public DateTimeOffset StartedUtc;
        public DateTimeOffset? FinishedUtc;
        public string Host = string.Empty;
        public bool IsWindows;
        public string Cwd = string.Empty;
        public string ToolDir = string.Empty;
        public string ProcessPath = string.Empty;
        public int ProbePid;
        public DateTime ProbeUpTime;
        public string[] Args = [];
        public string UncQueue = string.Empty;
        public int MainPid = -1;
        public string Dotnet = string.Empty;
        public string DefaultRunsRoot = string.Empty;
        public string MainWorkerBefore = string.Empty;
        public string MainWorkerAfter = string.Empty;
        public string DefaultRunsRootBefore = string.Empty;
        public string DefaultRunsRootAfter = string.Empty;
        public string CleanupMode = string.Empty;
        public List<int> CleanupKilledPids = [];
        public bool ShutdownMarkersWritten;
        public List<Check> Checks = [];
        public Dictionary<string, object?> PhaseFacts = new();

        public bool AllPassed => Checks.Count > 0 && Checks.All(c => c.Status != "fail");

        public void Check(string name, bool ok, string? detail = null)
            => Checks.Add(new Check(name, ok ? "pass" : "fail", detail));

        public void Fail(string name, string detail) => Checks.Add(new Check(name, "fail", detail));

        public Dictionary<string, object?> ToWire() => new()
        {
            ["probe"] = Probe,
            ["startedUtc"] = StartedUtc.ToString("O", CultureInfo.InvariantCulture),
            ["finishedUtc"] = FinishedUtc?.ToString("O", CultureInfo.InvariantCulture),
            ["verdict"] = AllPassed ? "PASS" : "FAIL",
            ["host"] = Host,
            ["isWindows"] = IsWindows,
            ["probePid"] = ProbePid,
            ["probeStartedUtc"] = ProbeUpTime.ToString("O", CultureInfo.InvariantCulture),
            ["cwd"] = Cwd,
            ["toolDir"] = ToolDir,
            ["processPath"] = ProcessPath,
            ["uncQueue"] = UncQueue,
            ["mainPid"] = MainPid,
            ["dotnet"] = Dotnet,
            ["mainWorkerBefore"] = MainWorkerBefore,
            ["mainWorkerAfter"] = MainWorkerAfter,
            ["defaultRunsRoot"] = DefaultRunsRoot,
            ["defaultRunsRootBefore"] = DefaultRunsRootBefore,
            ["defaultRunsRootAfter"] = DefaultRunsRootAfter,
            ["shutdownMarkersWritten"] = ShutdownMarkersWritten,
            ["cleanupMode"] = CleanupMode,
            ["cleanupKilledPids"] = CleanupKilledPids,
            ["phaseFacts"] = PhaseFacts,
            ["checks"] = Checks,
            ["args"] = Args,
        };

        public int EmitSpawnCheck(int exitCode)
        {
            FinishedUtc ??= DateTimeOffset.UtcNow;
            Console.WriteLine(SpawnCheckPrefix + BridgeJson.Serialize(ToWire()));
            Console.Out.Flush();
            var pass = Checks.Count(c => c.Status == "pass");
            var fail = Checks.Count(c => c.Status == "fail");
            Console.WriteLine($"spawncheck: pass={pass} fail={fail} verdict={(AllPassed ? "PASS" : "FAIL")} exit={exitCode}");
            return exitCode;
        }

        public int Emit(int exitCode)
        {
            FinishedUtc ??= DateTimeOffset.UtcNow;
            Console.WriteLine(BoardPrefix + BridgeJson.Serialize(ToWire()));
            Console.Out.Flush();
            var pass = Checks.Count(c => c.Status == "pass");
            var fail = Checks.Count(c => c.Status == "fail");
            Console.WriteLine($"lifecycle: pass={pass} fail={fail} verdict={(AllPassed ? "PASS" : "FAIL")} exit={exitCode}");
            return exitCode;
        }
    }

    private sealed record Check(string Name, string Status, string? Detail);

    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, [MarshalAs(UnmanagedType.Bool)] bool bInheritHandle, int dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsProcessInJob(IntPtr processHandle, IntPtr jobHandle, [MarshalAs(UnmanagedType.Bool)] out bool result);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();
}
