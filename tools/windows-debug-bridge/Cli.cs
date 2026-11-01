// Windows 调试桥 · WSL 侧 CLI
//
// 设计要点：
//   * argv 全程不经 shell：任何进程启动都走 ProcessStartInfo.ArgumentList；
//   * CLI 只写 workspace 内的队列目录（绝不写 Windows 宿主盘）；
//   * run/build-run/build 提交后立即返回任务 id，不等任务结束；
//   * stdout 只承载机器可读内容（任务 id / --json 的单行 JSON / logs 的日志行），人读文本一律走 stderr。
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace GalleriesManager.Wdb;

/// <summary>CLI 进程退出码。被启动程序的退出码只出现在 status 的 exitCode 字段，不参与 CLI 退出码。</summary>
public static class CliExit
{
    public const int Ok = 0;
    public const int Usage = 2;
    public const int NotFound = 3;
    public const int Timeout = 4;
    public const int Failed = 5;
    public const int Canceled = 6;
    public const int Interrupted = 7;
    public const int IoError = 8;
    public const int WorkerUnavailable = 9;
}

public static class Cli
{
    public const string Version = "0.1.0";

    /// <summary>Worker 心跳超过该秒数即视为失联（只告警，不做 PID 恢复、不杀进程）。</summary>
    public const double WorkerHeartbeatStaleSeconds = 15.0;

    /// <summary>
    /// 默认排除的**相对路径**（相对源根）。仓库里 spikes/llamacpp 与 spikes/models 是 GB 级下载物，
    /// 必须按路径排除而不是按目录名（仓库里存在合法的 src/**/Models 目录）。
    /// </summary>
    private static readonly string[] DefaultExcludedRelativePaths =
    {
        "spikes/llamacpp",
        "spikes/models",
        "eval",
    };

    private static string[] SnapshotExcludes(CliArgs parsed) =>
        DefaultExcludedRelativePaths.Concat(parsed.GetAll("exclude")).ToArray();

    public static async Task<int> RunAsync(string[] args)
    {
        CliArgs parsed;
        try
        {
            parsed = CliArgs.Parse(args);
        }
        catch (CliUsageException ex)
        {
            Console.Error.WriteLine("wdb: " + ex.Message);
            return CliExit.Usage;
        }

        try
        {
            if (parsed.Has("version"))
            {
                Console.Out.WriteLine(Version);
                return CliExit.Ok;
            }

            var command = parsed.Positional.Count > 0 ? parsed.Positional[0] : string.Empty;
            if (parsed.Has("help"))
            {
                PrintHelp(command);
                return CliExit.Ok;
            }
            if (string.IsNullOrEmpty(command))
            {
                PrintHelp(null);
                return CliExit.Usage;
            }

            switch (command.ToLowerInvariant())
            {
                case "run":
                    return await SubmitAsync(JobModes.Run, parsed).ConfigureAwait(false);
                case "build":
                    return await SubmitAsync(JobModes.Build, parsed).ConfigureAwait(false);
                case "build-run":
                case "buildrun":
                    return await SubmitAsync(JobModes.BuildRun, parsed).ConfigureAwait(false);
                case "status":
                    return Status(parsed);
                case "logs":
                    return await LogsAsync(parsed).ConfigureAwait(false);
                case "wait":
                    return await WaitCommandAsync(parsed).ConfigureAwait(false);
                case "stop":
                    return Stop(parsed);
                case "shutdown":
                    return Shutdown(parsed);
                case "doctor":
                    return await DoctorAsync(parsed).ConfigureAwait(false);
                case "help":
                    PrintHelp(parsed.Positional.Count > 1 ? parsed.Positional[1] : null);
                    return CliExit.Ok;
                default:
                    Console.Error.WriteLine("wdb: 未知命令：" + command);
                    PrintHelp(null);
                    return CliExit.Usage;
            }
        }
        catch (CliUsageException ex)
        {
            Console.Error.WriteLine("wdb: " + ex.Message);
            return CliExit.Usage;
        }
        catch (WdbException ex)
        {
            Console.Error.WriteLine("wdb: " + ex.Message);
            return ex.Code == "ERR_SECURITY_VIOLATION" ? CliExit.IoError : CliExit.Usage;
        }
        catch (SnapshotException ex)
        {
            Console.Error.WriteLine("wdb: 源快照失败：" + ex.Message);
            return CliExit.IoError;
        }
        catch (IOException ex)
        {
            Console.Error.WriteLine("wdb: IO 失败：" + ex.Message);
            return CliExit.IoError;
        }
        catch (UnauthorizedAccessException ex)
        {
            Console.Error.WriteLine("wdb: 权限不足：" + ex.Message);
            return CliExit.IoError;
        }
    }

    // ---------------------------------------------------------------- 提交任务

    private static async Task<int> SubmitAsync(string mode, CliArgs parsed)
    {
        RequireKnown(parsed, "project", "configuration", "framework", "timeout", "wait-timeout",
            "exclude", "json", "quiet", "wait", "follow", "require-worker", "poll");

        var project = NormalizeProjectPath(parsed.Get("project"));
        if (mode != JobModes.Run && mode != JobModes.BuildRun && parsed.Passthrough.Count > 0)
            throw new CliUsageException("只有 run / build-run 支持 '-- <应用参数>' 透传。");

        var sourceRoot = ResolveSourceRoot(parsed.Get("source"));
        var projectOnDisk = Path.Combine(sourceRoot, project.Replace('/', Path.DirectorySeparatorChar));
        if (!File.Exists(projectOnDisk))
            throw new CliUsageException($"项目文件不存在（相对源目录 {sourceRoot}）：{project}");

        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: true);

        var configuration = parsed.Get("configuration");
        var framework = parsed.Get("framework");
        var timeoutSeconds = parsed.GetInt("timeout") ?? (mode == JobModes.Build ? 600 : 0);
        if (timeoutSeconds < 0) throw new CliUsageException("--timeout 不能为负。");

        var health = WorkerHealth.Check(queueRoot);
        if (parsed.Has("require-worker") && !health.Healthy)
            throw new CliUsageException("--require-worker 已指定但 Worker 未运行（" + health.Reason + "）。" +
                                        Environment.NewLine + WorkerHealth.StartHint(queueRoot));

        var id = Store.NewId(DateTimeOffset.UtcNow);
        var jobDir = BridgeLayout.JobDir(queueRoot, id);
        AssertJobPathSafe(queueRoot, id, "任务目录");
        Store.EnsureDir(jobDir);

        var payloadRoot = BridgeLayout.PayloadPath(queueRoot, id);
        AssertNoReparseAncestors(payloadRoot, "payload 目录");
        var snapshot = Snapshot.Copy(sourceRoot, payloadRoot, SnapshotExcludes(parsed), queueRoot);

        // 快照里必须真的存在该项目文件：符号链接被跳过、目录被排除、文件超限都会让它缺席
        var projectInPayload = Path.Combine(payloadRoot, project.Replace('/', Path.DirectorySeparatorChar));
        if (!File.Exists(projectInPayload))
        {
            Console.Error.WriteLine(
                $"wdb: 项目文件未进入快照（被排除 / 是符号链接 / 超过 {Snapshot.MaxFileBytes} 字节）：{project}");
            return CliExit.IoError;
        }

        var now = DateTimeOffset.UtcNow;
        var report = new PayloadReport(BridgeProtocol.Version, id, snapshot.SourceRoot, snapshot.FileCount,
            snapshot.TotalBytes, snapshot.SkippedCount, snapshot.Skipped, snapshot.Excluded, snapshot.ManifestHash, now);
        Store.WriteJsonAtomic(BridgeLayout.PayloadReportPath(queueRoot, id), report);

        var request = new Request(BridgeProtocol.Version, id, mode, project, configuration, framework,
            parsed.Passthrough.Count > 0 ? parsed.Passthrough.ToArray() : null, timeoutSeconds,
            new PayloadRef(BridgeLayout.PayloadDirName, snapshot.FileCount, snapshot.TotalBytes, snapshot.ManifestHash),
            now, snapshot.SourceRoot);

        // request.json 是本任务目录里最后一个被写入的文件：它一出现就代表快照完整
        Store.WriteJsonAtomic(BridgeLayout.RequestPath(queueRoot, id), request);

        if (parsed.Has("json"))
        {
            Console.Out.WriteLine(BridgeJson.Serialize(new SubmitResult(id, mode, project, queueRoot, jobDir,
                snapshot.FileCount, snapshot.TotalBytes, snapshot.SkippedCount, health.Healthy, now)));
        }
        else
        {
            Console.Out.WriteLine(id);
        }

        if (snapshot.SkippedCount > 0 && !parsed.Has("quiet"))
        {
            Console.Error.WriteLine($"wdb: 快照跳过 {snapshot.SkippedCount} 个条目（明细见 payload-report.json）：" +
                                    string.Join(", ", snapshot.Skipped.Take(3)));
        }

        if (!health.Healthy && !parsed.Has("quiet"))
            Console.Error.WriteLine("wdb: " + health.Explain(queueRoot));

        if (parsed.Has("wait") || parsed.Has("follow"))
        {
            return await WaitLoopAsync(queueRoot, id, parsed.GetInt("wait-timeout") ?? 900,
                parsed.GetInt("poll") ?? 500, parsed.Has("json"), parsed.Has("follow")).ConfigureAwait(false);
        }

        return CliExit.Ok;
    }

    // ---------------------------------------------------------------- wait

    private static async Task<int> WaitCommandAsync(CliArgs parsed)
    {
        RequireKnown(parsed, "id", "json", "wait-timeout", "poll", "follow");
        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: false);
        var id = ResolveJobId(parsed, "wait");
        AssertJobPathSafe(queueRoot, id, "任务目录");
        return await WaitLoopAsync(queueRoot, id, parsed.GetInt("wait-timeout") ?? 900,
            parsed.GetInt("poll") ?? 500, parsed.Has("json"), parsed.Has("follow")).ConfigureAwait(false);
    }

    private static async Task<int> WaitLoopAsync(string queueRoot, string id, int timeoutSeconds, int pollMs, bool json, bool follow)
    {
        if (pollMs < 100) pollMs = 100;
        var started = DateTimeOffset.UtcNow;
        var stdout = new LogCursor();
        var stderr = new LogCursor();
        var warnedWorker = false;

        while (true)
        {
            if (follow) PumpLogs(queueRoot, id, json, stdout, stderr);

            var status = Store.ReadJson<Status>(BridgeLayout.StatusPath(queueRoot, id));
            if (status is not null && JobStates.IsTerminal(status.State))
            {
                // 终态后必须把剩余日志排干（可能远超单次 8MiB 读取上限），否则输出会截断
                if (follow) DrainLogs(queueRoot, id, "all", stdout, stderr, emitJson: false, prefix: false, textToStderr: json);
                EmitStatus(status, json);
                return ExitForState(status.State);
            }

            if (!warnedWorker && status is null or { State: JobStates.Queued })
            {
                var health = WorkerHealth.Check(queueRoot);
                if (!health.Healthy)
                {
                    Console.Error.WriteLine("wdb: " + health.Explain(queueRoot));
                    warnedWorker = true;
                }
            }

            if (timeoutSeconds > 0 && (DateTimeOffset.UtcNow - started).TotalSeconds > timeoutSeconds)
            {
                var stateText = status is null ? "尚无 status.json（Worker 未接手）" : status.State;
                Console.Error.WriteLine($"wdb: 等待超时（{timeoutSeconds}s），任务 {id} 未达终态（{stateText}）；" +
                                        "CLI 不杀进程（进程树由 Worker 的 Job Object 负责）。");
                return CliExit.Timeout;
            }

            await Task.Delay(pollMs).ConfigureAwait(false);
        }
    }

    /// <summary>wait --follow 的单轮增量读取（不排干）。--json 时宿主输出一律转 stderr，保持 stdout 只有一行 JSON。</summary>
    private static void PumpLogs(string queueRoot, string id, bool jsonMode, LogCursor stdout, LogCursor stderr)
    {
        var (chunks, _, _) = ReadLogChunks(queueRoot, id, "all", tailLines: null, stdout, stderr);
        EmitLogChunks(id, chunks, emitJson: false, prefix: false, announceEmpty: false, textToStderr: jsonMode);
    }

    /// <summary>增量读取一段日志（tailLines 仅在首轮 --tail 时非空）。</summary>
    private static (List<LogChunk> Chunks, bool Progressed, bool HasMore) ReadLogChunks(
        string queueRoot, string id, string stream, int? tailLines, LogCursor stdout, LogCursor stderr)
    {
        var chunks = new List<LogChunk>();
        var progressed = false;
        var hasMore = false;

        if (stream is "stdout" or "all")
        {
            var before = stdout.Offset;
            var slice = Store.ReadLog(BridgeLayout.StdoutPath(queueRoot, id), stdout.Offset, tailLines ?? 0);
            stdout.Offset = slice.NextOffset;
            progressed |= slice.NextOffset > before;
            hasMore |= slice.NextOffset < slice.Length;
            chunks.Add(new LogChunk("stdout", slice, stdout));
        }

        if (stream is "stderr" or "all")
        {
            var before = stderr.Offset;
            var slice = Store.ReadLog(BridgeLayout.StderrPath(queueRoot, id), stderr.Offset, tailLines ?? 0);
            stderr.Offset = slice.NextOffset;
            progressed |= slice.NextOffset > before;
            hasMore |= slice.NextOffset < slice.Length;
            chunks.Add(new LogChunk("stderr", slice, stderr));
        }

        return (chunks, progressed, hasMore);
    }

    /// <summary>
    /// 终态排干：反复读到追平文件末尾。若某一轮毫无进展（尾部是不完整/非法 UTF-8 字节），立即停止，
    /// 既不会截断超过单次上限的日志，也不会死循环。
    /// </summary>
    private static void DrainLogs(string queueRoot, string id, string stream, LogCursor stdout, LogCursor stderr,
        bool emitJson, bool prefix, bool textToStderr)
    {
        while (true)
        {
            var (chunks, progressed, hasMore) = ReadLogChunks(queueRoot, id, stream, tailLines: null, stdout, stderr);
            EmitLogChunks(id, chunks, emitJson, prefix, announceEmpty: false, textToStderr);
            if (!hasMore || !progressed) break;
        }

        // 终态收尾：尾部若还剩下不完整/非法字节（任务已结束、不会再增长），明确以替换字符吐一次，避免静默丢字
        var tail = ReadRemainderChunks(queueRoot, id, stream, stdout, stderr);
        if (tail.Count > 0)
            EmitLogChunks(id, tail, emitJson, prefix, announceEmpty: false, textToStderr);
    }

    /// <summary>终态收尾读：不再扣留尾部不完整序列（仅此一次）。</summary>
    private static List<LogChunk> ReadRemainderChunks(string queueRoot, string id, string stream, LogCursor stdout, LogCursor stderr)
    {
        var chunks = new List<LogChunk>();

        if (stream is "stdout" or "all")
        {
            var slice = Store.ReadLogRemainder(BridgeLayout.StdoutPath(queueRoot, id), stdout.Offset);
            stdout.Offset = slice.NextOffset;
            if (slice.RawText.Length > 0) chunks.Add(new LogChunk("stdout", slice, stdout));
        }

        if (stream is "stderr" or "all")
        {
            var slice = Store.ReadLogRemainder(BridgeLayout.StderrPath(queueRoot, id), stderr.Offset);
            stderr.Offset = slice.NextOffset;
            if (slice.RawText.Length > 0) chunks.Add(new LogChunk("stderr", slice, stderr));
        }

        return chunks;
    }

    // ---------------------------------------------------------------- status

    private static int Status(CliArgs parsed)
    {
        RequireKnown(parsed, "id", "json", "max-jobs", "all");
        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: false);
        var id = parsed.Get("id") ?? (parsed.Positional.Count > 1 ? parsed.Positional[1] : null);
        var maxJobs = parsed.GetInt("max-jobs") ?? 10;
        if (maxJobs <= 0) maxJobs = 10;

        var worker = WorkerHealth.Check(queueRoot);

        if (!string.IsNullOrWhiteSpace(id))
        {
            id = ValidateJobId(id.Trim());
            AssertJobPathSafe(queueRoot, id, "任务目录");
            var status = Store.ReadJson<Status>(BridgeLayout.StatusPath(queueRoot, id));
            if (status is null)
            {
                var request = Store.ReadJson<Request>(BridgeLayout.RequestPath(queueRoot, id));
                if (request is null)
                {
                    Console.Error.WriteLine("wdb: 未找到任务：" + id);
                    return CliExit.NotFound;
                }

                var view = new JobView(request.Id, JobStates.Queued, request.Mode, JobPhases.Submitted,
                    null, null, null, null, false);
                if (parsed.Has("json"))
                    Console.Out.WriteLine(BridgeJson.Serialize(view));
                else
                    Console.Out.WriteLine(FormatJobView(view));
                return CliExit.Ok;
            }

            if (parsed.Has("json"))
                Console.Out.WriteLine(BridgeJson.Serialize(status));
            else
                Console.Out.WriteLine(FormatStatus(status));
            return CliExit.Ok;
        }

        var jobs = ListJobs(queueRoot, maxJobs);
        if (parsed.Has("json"))
        {
            Console.Out.WriteLine(BridgeJson.Serialize(new QueueView(queueRoot,
                worker.State is null ? null : new WorkerHealthView(worker.Healthy, worker.AgeSeconds, worker.State),
                jobs)));
        }
        else
        {
            Console.Out.WriteLine("queue: " + queueRoot);
            Console.Out.WriteLine(worker.Healthy
                ? $"worker: 运行中 pid={worker.State!.Pid} host={worker.State.HostName} 心跳 {worker.AgeSeconds:F1}s 前 当前任务={worker.State.CurrentJobId ?? "-"}"
                : "worker: 未运行（" + worker.Reason + "）");
            foreach (var job in jobs) Console.Out.WriteLine(FormatJobView(job));
        }

        return CliExit.Ok;
    }

    private static JobView[] ListJobs(string queueRoot, int maxJobs)
    {
        var jobsDir = BridgeLayout.JobsDir(queueRoot);
        if (!Directory.Exists(jobsDir)) return Array.Empty<JobView>();

        var ids = Directory.GetDirectories(jobsDir)
            .Select(Path.GetFileName)
            .Where(x => !string.IsNullOrEmpty(x))
            .Cast<string>()
            .OrderByDescending(x => x, StringComparer.Ordinal)
            .Take(maxJobs);

        var result = new List<JobView>();
        foreach (var id in ids)
        {
            try
            {
                AssertJobPathSafe(queueRoot, id, "任务目录");
            }
            catch (WdbException ex)
            {
                result.Add(new JobView(id, JobStates.Rejected, null, null, null,
                    "任务目录含重解析点/符号链接，已跳过：" + ex.Message, null, null, false));
                continue;
            }

            var status = Store.ReadJson<Status>(BridgeLayout.StatusPath(queueRoot, id));
            if (status is not null)
            {
                result.Add(new JobView(status.Id, status.State, status.Mode, status.Phase, status.ExitCode,
                    status.Error, status.UpdatedUtc, status.FinishedUtc, true));
                continue;
            }

            var request = Store.ReadJson<Request>(BridgeLayout.RequestPath(queueRoot, id));
            result.Add(new JobView(id, request is null ? "unknown" : JobStates.Queued, request?.Mode,
                request is null ? null : JobPhases.Submitted, null, null, null, null, false));
        }

        return result.ToArray();
    }

    // ---------------------------------------------------------------- logs

    private static async Task<int> LogsAsync(CliArgs parsed)
    {
        RequireKnown(parsed, "id", "json", "stream", "offset", "tail", "follow", "poll", "wait-timeout");
        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: false);
        var id = ResolveJobId(parsed, "logs");

        AssertJobPathSafe(queueRoot, id, "任务目录");
        if (!Directory.Exists(BridgeLayout.JobDir(queueRoot, id)))
        {
            Console.Error.WriteLine("wdb: 未找到任务目录：" + BridgeLayout.JobDir(queueRoot, id));
            return CliExit.NotFound;
        }

        var stream = (parsed.Get("stream") ?? "stdout").ToLowerInvariant();
        if (stream is not ("stdout" or "stderr" or "all"))
            throw new CliUsageException("--stream 只能是 stdout、stderr 或 all。");

        var tail = parsed.GetInt("tail");
        var offset = parsed.GetLong("offset");
        if (tail is null && offset is null) tail = 200;
        if (tail is < 0 || offset is < 0) throw new CliUsageException("--tail / --offset 不能为负。");

        var pollMs = parsed.GetInt("poll") ?? 500;
        if (pollMs < 100) pollMs = 100;
        var follow = parsed.Has("follow");
        var json = parsed.Has("json");
        var deadline = DateTimeOffset.UtcNow.AddSeconds(parsed.GetInt("wait-timeout") ?? 900);

        var stdout = new LogCursor { Offset = offset ?? 0 };
        var stderr = new LogCursor { Offset = offset ?? 0 };
        var first = true;

        while (true)
        {
            // 首轮若给了 --tail 就按行尾读，其后一律按 offset 增量读
            var tailLines = first && tail is > 0 ? tail : null;
            var (chunks, _, _) = ReadLogChunks(queueRoot, id, stream, tailLines, stdout, stderr);
            first = false;
            EmitLogChunks(id, chunks, emitJson: json, prefix: stream == "all", announceEmpty: !follow, textToStderr: false);

            if (!follow) return CliExit.Ok;

            var status = Store.ReadJson<Status>(BridgeLayout.StatusPath(queueRoot, id));
            if (status is not null && JobStates.IsTerminal(status.State))
            {
                // 终态：把剩余日志全部排干（可能远超单次 8MiB 读取上限），避免尾部被截断
                DrainLogs(queueRoot, id, stream, stdout, stderr, emitJson: json, prefix: stream == "all", textToStderr: false);
                return ExitForState(status.State);
            }

            if (DateTimeOffset.UtcNow > deadline)
            {
                Console.Error.WriteLine("wdb: logs --follow 等待超时。");
                return CliExit.Timeout;
            }

            await Task.Delay(pollMs).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// 输出日志片段。文本模式一律用 <c>Write(RawText)</c> 原样写回：不补换行、不改写尚未结束的片段；
    /// prefix 模式按行首插入前缀，跨片段的续行不会重复插入前缀。
    /// </summary>
    private static void EmitLogChunks(string id, List<LogChunk> chunks, bool emitJson, bool prefix, bool announceEmpty, bool textToStderr)
    {
        if (emitJson)
        {
            StreamView? stdout = null;
            StreamView? stderr = null;
            foreach (var chunk in chunks)
            {
                var view = new StreamView(chunk.Slice.Offset, chunk.Slice.NextOffset, chunk.Slice.Length,
                    chunk.Slice.Truncated, chunk.Slice.Lines);
                if (chunk.Name == "stdout") stdout = view; else stderr = view;
            }

            Console.Out.WriteLine(BridgeJson.Serialize(new LogChunkView(id, stdout, stderr)));
            return;
        }

        var emitted = 0;
        foreach (var chunk in chunks)
        {
            var target = chunk.Name == "stdout" && !textToStderr ? Console.Out : Console.Error;
            var text = chunk.Slice.RawText;

            if (text.Length > 0)
            {
                WriteFragment(target, text, prefix ? "[" + chunk.Name + "] " : null, chunk.Cursor);
                emitted += chunk.Slice.Lines.Length;
            }

            if (chunk.Slice.Truncated)
                Console.Error.WriteLine($"wdb: {chunk.Name} 日志比请求的 offset 短，已从头重读。");
        }

        if (emitted == 0 && announceEmpty)
            Console.Error.WriteLine("wdb: 暂无日志输出（Worker 尚未写入 stdout.log / stderr.log）。");
    }

    /// <summary>原样写回文本片段；prefix 模式只在行首插入前缀（续片段不重复插入）。</summary>
    private static void WriteFragment(TextWriter target, string text, string? prefix, LogCursor cursor)
    {
        if (prefix is null)
        {
            target.Write(text);
            cursor.AtLineStart = text[^1] == '\n';
            return;
        }

        var start = 0;
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] != '\n') continue;
            if (cursor.AtLineStart) target.Write(prefix);
            target.Write(text.AsSpan(start, i + 1 - start));
            cursor.AtLineStart = true;
            start = i + 1;
        }

        if (start < text.Length)
        {
            if (cursor.AtLineStart) target.Write(prefix);
            target.Write(text.AsSpan(start));
            cursor.AtLineStart = false;
        }
    }

    // ---------------------------------------------------------------- stop / shutdown

    private static int Stop(CliArgs parsed)
    {
        RequireKnown(parsed, "id", "reason", "json");
        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: false);
        var id = ResolveJobId(parsed, "stop");

        AssertJobPathSafe(queueRoot, id, "任务目录");
        if (!Directory.Exists(BridgeLayout.JobDir(queueRoot, id)))
        {
            Console.Error.WriteLine("wdb: 未找到任务：" + id);
            return CliExit.NotFound;
        }

        var cancelPath = BridgeLayout.CancelPath(queueRoot, id);
        Store.WriteMarker(cancelPath);

        if (parsed.Has("json"))
            Console.Out.WriteLine(BridgeJson.Serialize(new MarkerResult(id, cancelPath, DateTimeOffset.UtcNow)));
        else
            Console.Out.WriteLine("已写入取消标记：" + cancelPath);
        return CliExit.Ok;
    }

    private static int Shutdown(CliArgs parsed)
    {
        RequireKnown(parsed, "reason", "json");
        var queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: true);
        var path = BridgeLayout.ShutdownPath(queueRoot);
        Store.WriteMarker(path);

        if (parsed.Has("json"))
            Console.Out.WriteLine(BridgeJson.Serialize(new MarkerResult(null, path, DateTimeOffset.UtcNow)));
        else
            Console.Out.WriteLine("已写入停机标记：" + path + "（Worker 结束当前任务后退出）");
        return CliExit.Ok;
    }

    // ---------------------------------------------------------------- doctor

    private static async Task<int> DoctorAsync(CliArgs parsed)
    {
        RequireKnown(parsed, "json", "max-jobs");
        var notes = new List<string>();

        string? repoRoot = FindRepoRoot(Directory.GetCurrentDirectory()) ?? FindRepoRoot(AppContext.BaseDirectory);
        string queueRoot;
        string? queueError = null;
        var queueWritable = false;

        try
        {
            queueRoot = ResolveQueueRoot(parsed.Get("queue"), create: true);
        }
        catch (Exception ex) when (ex is CliUsageException or IOException or UnauthorizedAccessException)
        {
            queueRoot = parsed.Get("queue") ?? Path.Combine(repoRoot ?? Directory.GetCurrentDirectory(), ".wdb", "queue");
            queueError = ex.Message;
        }

        if (queueError is null)
        {
            var probe = Path.Combine(queueRoot, ".wdb-write-probe-" + Store.NewToken(8));
            try
            {
                Store.WriteTextAtomic(probe, "ok");
                Store.TryDelete(probe);
                queueWritable = true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                queueError = ex.Message;
            }
        }

        string sourceRoot;
        SnapshotResult? scan = null;
        try
        {
            sourceRoot = ResolveSourceRoot(parsed.Get("source"));
            scan = Snapshot.Inspect(sourceRoot, SnapshotExcludes(parsed), queueRoot);
        }
        catch (Exception ex) when (ex is CliUsageException or SnapshotException or IOException)
        {
            sourceRoot = "(unknown)";
            notes.Add("源目录清点失败：" + ex.Message);
        }

        var worker = WorkerHealth.Check(queueRoot);
        var jobs = queueWritable || Directory.Exists(queueRoot) ? ListJobs(queueRoot, parsed.GetInt("max-jobs") ?? 5) : Array.Empty<JobView>();

        var handlers = Array.Empty<string>();
        string? mntC = null;
        var interop = false;
        if (!OperatingSystem.IsWindows())
        {
            (interop, handlers, mntC) = ProbeLinuxHost();
            if (!interop)
                notes.Add("WSL 互操作未启用（binfmt_misc 无 WSLInterop）：本会话无法直接执行 Windows 程序，" +
                          "Worker 必须在 Windows 前台手动启动。");
        }

        if (!worker.Healthy) notes.Add(WorkerHealth.StartHint(queueRoot));

        var report = new DoctorReport(
            CliVersion: Version,
            OperatingSystem: RuntimeInformation2.OsDescription,
            RepoRoot: repoRoot ?? "(not-found)",
            SourceRoot: sourceRoot,
            QueueRoot: queueRoot,
            QueueWritable: queueWritable,
            QueueError: queueError,
            WorkerHealthy: worker.Healthy,
            Worker: worker.State is null ? null : new WorkerHealthView(worker.Healthy, worker.AgeSeconds, worker.State),
            JobCount: jobs.Length,
            LatestJob: jobs.Length > 0 ? jobs[0] : null,
            SourceFileCount: scan?.FileCount ?? 0,
            SourceBytes: scan?.TotalBytes ?? 0,
            SourceSkippedCount: scan?.SkippedCount ?? 0,
            SourceExcluded: scan?.Excluded ?? Array.Empty<string>(),
            DotnetVersion: await ProbeDotnetVersionAsync().ConfigureAwait(false),
            WindowsInterop: interop,
            BinfmtHandlers: handlers,
            MntCMount: mntC,
            Notes: notes.ToArray());

        if (parsed.Has("json"))
        {
            Console.Out.WriteLine(BridgeJson.Serialize(report));
        }
        else
        {
            Console.Out.WriteLine("wdb doctor");
            Console.Out.WriteLine("  cli          : " + report.CliVersion + "  (" + report.OperatingSystem + ")");
            Console.Out.WriteLine("  repo         : " + report.RepoRoot);
            Console.Out.WriteLine("  source       : " + report.SourceRoot +
                                  $"  files={report.SourceFileCount} bytes={report.SourceBytes} skipped={report.SourceSkippedCount}");
            Console.Out.WriteLine("  excluded     : " + (report.SourceExcluded.Length == 0 ? "(none)" : string.Join(", ", report.SourceExcluded)));
            Console.Out.WriteLine("  queue        : " + report.QueueRoot + (report.QueueWritable ? "  [writable]" : "  [NOT writable]"));
            if (report.QueueError is not null) Console.Out.WriteLine("  queue error  : " + report.QueueError);
            Console.Out.WriteLine("  worker       : " + (report.WorkerHealthy
                ? $"运行中 pid={report.Worker!.State.Pid} 心跳 {report.Worker.AgeSeconds:F1}s 前"
                : "未运行"));
            Console.Out.WriteLine("  dotnet       : " + report.DotnetVersion);
            if (!OperatingSystem.IsWindows())
            {
                Console.Out.WriteLine("  win interop  : " + (report.WindowsInterop ? "enabled" : "DISABLED") +
                                      "  binfmt=" + (report.BinfmtHandlers.Length == 0 ? "(none)" : string.Join(",", report.BinfmtHandlers)));
                if (report.MntCMount is not null) Console.Out.WriteLine("  /mnt/c       : " + report.MntCMount);
            }
            foreach (var note in report.Notes) Console.Out.WriteLine("  ! " + note);
        }

        if (!queueWritable) return CliExit.IoError;
        return CliExit.Ok;
    }

    private static (bool Interop, string[] Handlers, string? MntC) ProbeLinuxHost()
    {
        var handlers = Array.Empty<string>();
        try
        {
            if (Directory.Exists("/proc/sys/fs/binfmt_misc"))
            {
                handlers = Directory.GetFiles("/proc/sys/fs/binfmt_misc")
                    .Select(Path.GetFileName)
                    .Where(n => !string.IsNullOrEmpty(n))
                    .Cast<string>()
                    .OrderBy(n => n, StringComparer.Ordinal)
                    .ToArray();
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }

        var interop = handlers.Any(h => h.StartsWith("WSLInterop", StringComparison.OrdinalIgnoreCase));

        string? mntC = null;
        try
        {
            foreach (var line in File.ReadLines("/proc/mounts"))
            {
                if (line.Contains(" /mnt/c ", StringComparison.Ordinal))
                {
                    mntC = line;
                    break;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }

        return (interop, handlers, mntC);
    }

    private static async Task<string> ProbeDotnetVersionAsync()
    {
        try
        {
            var psi = new ProcessStartInfo("dotnet")
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            psi.ArgumentList.Add("--version");

            using var process = Process.Start(psi);
            if (process is null) return "unavailable";

            var output = await process.StandardOutput.ReadToEndAsync().ConfigureAwait(false);
            await process.WaitForExitAsync().ConfigureAwait(false);
            var text = output.Trim();
            return string.IsNullOrEmpty(text) ? "unknown" : text;
        }
        catch (Exception ex) when (ex is IOException or System.ComponentModel.Win32Exception)
        {
            return "unavailable (" + ex.GetType().Name + ")";
        }
    }

    // ---------------------------------------------------------------- 通用辅助

    private static readonly string[] GlobalOptions = { "queue", "source", "json", "quiet", "help", "h", "version", "exclude" };

    private static void RequireKnown(CliArgs parsed, params string[] allowed)
    {
        var set = new HashSet<string>(GlobalOptions, StringComparer.OrdinalIgnoreCase);
        foreach (var item in allowed) set.Add(item);

        var unknown = parsed.Names.Where(n => !set.Contains(n)).ToArray();
        if (unknown.Length > 0)
            throw new CliUsageException("不支持的选项：" + string.Join(", ", unknown.Select(n => "--" + n)));
    }

    private static string ResolveQueueRoot(string? explicitQueue, bool create)
    {
        var value = !string.IsNullOrWhiteSpace(explicitQueue)
            ? explicitQueue
            : Environment.GetEnvironmentVariable("WDB_QUEUE");

        var repo = FindRepoRoot(Directory.GetCurrentDirectory()) ?? FindRepoRoot(AppContext.BaseDirectory);

        string full;
        if (!string.IsNullOrWhiteSpace(value))
        {
            full = Path.GetFullPath(value!);
        }
        else
        {
            var repoRoot = repo
                ?? throw new CliUsageException("无法定位仓库根（未找到 GalleriesManager.slnx 或 .git）；请用 --queue 指定队列目录。");
            full = Path.GetFullPath(Path.Combine(repoRoot, ".wdb", "queue"));
        }

        // 顺序很重要：**先**做包含性与重解析点校验，**再**创建目录。
        // 否则一个越界（或指向宿主盘/外部的符号链接）路径会先被创建出来，产生越界副作用。
        if (repo is not null && !IsUnderOrEqual(full, repo))
            throw new CliUsageException($"队列必须位于 workspace 内（{repo}）：{full}；CLI 不具备写 Windows 宿主盘的能力。");

        AssertNoReparseAncestors(full, "队列目录");

        if (create)
        {
            Store.EnsureDir(full);
            AssertNoReparseAncestors(full, "队列目录");
        }
        else if (!Directory.Exists(full))
        {
            throw new CliUsageException("队列目录不存在：" + full + "（尚未运行过 Worker？）");
        }

        return full;
    }

    /// <summary>拒绝跨重解析点/符号链接读写：目标自身与其所有已存在祖先都必须不是链接。</summary>
    private static void AssertNoReparseAncestors(string path, string what)
    {
        WdbPath.AssertNoReparseAncestors(path, what);
    }

    /// <summary>任务目录及其祖先（含队列根）必须不含重解析点，避免 markers/日志被重定向到队列之外。</summary>
    private static void AssertJobPathSafe(string queueRoot, string id, string what)
    {
        AssertNoReparseAncestors(BridgeLayout.JobDir(queueRoot, id), what);
    }

    private static string ResolveSourceRoot(string? explicitSource)
    {
        var value = !string.IsNullOrWhiteSpace(explicitSource)
            ? explicitSource
            : Environment.GetEnvironmentVariable("WDB_SOURCE");

        if (!string.IsNullOrWhiteSpace(value))
        {
            var full = Path.GetFullPath(value!);
            if (!Directory.Exists(full)) throw new CliUsageException("源目录不存在：" + full);
            return full;
        }

        return FindRepoRoot(Directory.GetCurrentDirectory()) ?? FindRepoRoot(AppContext.BaseDirectory)
            ?? throw new CliUsageException("无法定位仓库根；请用 --source 指定要快照的源码目录。");
    }

    private static string? FindRepoRoot(string start)
    {
        try
        {
            var dir = new DirectoryInfo(Path.GetFullPath(start));
            while (dir is not null)
            {
                if (File.Exists(Path.Combine(dir.FullName, "GalleriesManager.slnx"))) return dir.FullName;
                if (Directory.Exists(Path.Combine(dir.FullName, ".git"))) return dir.FullName;
                dir = dir.Parent;
            }
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or UnauthorizedAccessException) { }

        return null;
    }

    private static string NormalizeProjectPath(string? project)
    {
        if (string.IsNullOrWhiteSpace(project))
            throw new CliUsageException("缺少 --project <仓库相对 .csproj 路径>。");

        var normalized = project.Replace('\\', '/').Trim();
        if (normalized.StartsWith('/') || Path.IsPathRooted(normalized))
            throw new CliUsageException("--project 必须是仓库相对路径（不接受绝对路径）：" + project);
        if (normalized.Split('/').Any(segment => segment is ".." or "." or ""))
            throw new CliUsageException("--project 不允许包含 . / .. 或空路径段：" + project);
        if (!normalized.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase))
            throw new CliUsageException("--project 必须是 .csproj 路径：" + project);

        return normalized;
    }

    private static string ResolveJobId(CliArgs parsed, string command)
    {
        var id = parsed.Get("id") ?? (parsed.Positional.Count > 1 ? parsed.Positional[1] : null);
        if (string.IsNullOrWhiteSpace(id))
            throw new CliUsageException($"{command} 需要任务 id：wdb {command} <id> [选项]");
        return ValidateJobId(id.Trim());
    }

    private static string ValidateJobId(string id)
    {
        if (id.Length == 0 || id.Length > 128 ||
            id.Contains('/') || id.Contains('\\') || id.Contains("..") ||
            id.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new CliUsageException("任务 id 非法：" + id);
        return id;
    }

    private static bool IsUnderOrEqual(string candidate, string root)
    {
        var c = candidate.TrimEnd(Path.DirectorySeparatorChar);
        var r = root.TrimEnd(Path.DirectorySeparatorChar);
        var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        return string.Equals(c, r, comparison) || c.StartsWith(r + Path.DirectorySeparatorChar, comparison);
    }

    private static int ExitForState(string state) => state switch
    {
        JobStates.Succeeded => CliExit.Ok,
        JobStates.Canceled => CliExit.Canceled,
        JobStates.Timeout => CliExit.Timeout,
        JobStates.Interrupted => CliExit.Interrupted,
        _ => CliExit.Failed,
    };

    private static void EmitStatus(Status status, bool json)
    {
        if (json) Console.Out.WriteLine(BridgeJson.Serialize(status));
        else Console.Out.WriteLine(FormatStatus(status));
    }

    private static string FormatStatus(Status status)
    {
        var sb = new StringBuilder();
        sb.Append(status.Id).Append("  ").Append(status.Mode).Append("  ").Append(status.State);
        if (!string.IsNullOrEmpty(status.Phase)) sb.Append(" / ").Append(status.Phase);
        if (status.ExitCode is not null) sb.Append("  exit=").Append(status.ExitCode);
        if (status.Pid is not null) sb.Append("  pid=").Append(status.Pid);
        sb.Append("  updated=").Append(status.UpdatedUtc.ToLocalTime().ToString("MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        if (status.FinishedUtc is not null)
            sb.Append("  finished=").Append(status.FinishedUtc.Value.ToLocalTime().ToString("MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        if (!string.IsNullOrEmpty(status.Error)) sb.Append(Environment.NewLine).Append("  error: ").Append(status.Error);
        if (!string.IsNullOrEmpty(status.Artifact)) sb.Append(Environment.NewLine).Append("  artifact: ").Append(status.Artifact);
        return sb.ToString();
    }

    private static string FormatJobView(JobView job)
    {
        var sb = new StringBuilder();
        sb.Append(job.Id).Append("  ").Append(job.State);
        if (!string.IsNullOrEmpty(job.Mode)) sb.Append("  ").Append(job.Mode);
        if (!string.IsNullOrEmpty(job.Phase)) sb.Append("  ").Append(job.Phase);
        if (job.ExitCode is not null) sb.Append("  exit=").Append(job.ExitCode);
        if (!job.StatusPresent) sb.Append("  (无 status.json：Worker 尚未接手)");
        if (!string.IsNullOrEmpty(job.Error)) sb.Append("  error: ").Append(job.Error);
        return sb.ToString();
    }

    private static void PrintHelp(string? command)
    {
        Console.Error.WriteLine("""
            wdb（Windows 调试桥 CLI）— 用法：wdb <命令> [选项]

            命令（WSL 侧）
              run        --project <仓库相对 .csproj> [--configuration Debug] [--framework net10.0]
                         [--timeout 0] [--wait] [--follow] [--json] [-- <应用参数>...]
              build      --project <...> [--configuration Debug] [--timeout 600] [--wait] [--json]
              build-run  同上（先构建，再运行构建产物）
              status     [<id>] [--max-jobs 10] [--json]
              logs       <id> [--stream stdout|stderr|all] [--tail 200 | --offset 字节] [--follow] [--json]
              wait       <id> [--wait-timeout 900] [--poll 500] [--follow] [--json]
              stop       <id> [--reason 说明]
              shutdown   [--reason 说明]
              doctor     [--json]

            全局选项
              --queue <目录>   队列根（默认 <repo>/.wdb/queue，必须位于 workspace 内）
              --source <目录>  参与快照的源码目录（默认仓库根）
              --exclude <名|相对路径>  额外排除（可重复）
              --json           stdout 只输出机器可读 JSON
              --help           帮助

            约定
              * run / build-run / build 提交后立即在 stdout 输出任务 id，不等任务结束；
              * 被启动程序的输出在 <queue>/jobs/<id>/stdout.log 与 stderr.log，用 wdb logs 读；
              * Worker 未运行时任务保持 queued；在 Windows 前台启动 Worker：
                  powershell.exe -NoProfile -ExecutionPolicy Bypass -File <UNC>\\tools\\windows-debug-bridge\\start-worker.ps1 -Queue '<UNC 队列路径>'

            Windows 侧
              wdb worker --queue <队列> --runs-root <本机运行根>
            """);

        if (!string.IsNullOrEmpty(command)) Console.Error.WriteLine(Environment.NewLine + "（已忽略未知子命令的帮助请求：" + command + "）");
    }
}

/// <summary>用法错误：直接以 <see cref="CliExit.Usage"/> 退出。</summary>
internal sealed class CliUsageException : Exception
{
    public CliUsageException(string message) : base(message) { }
}

/// <summary>极简 argv 解析：支持 --key value、--key=value、flag，以及 '--' 之后的原样透传。</summary>
internal sealed class CliArgs
{
    /// <summary>需要取值的选项名；其余 --xxx 视为 flag。</summary>
    private static readonly HashSet<string> ValueOptions = new(StringComparer.OrdinalIgnoreCase)
    {
        "queue", "source", "project", "configuration", "framework", "timeout", "wait-timeout",
        "id", "offset", "tail", "stream", "poll", "reason", "exclude", "runs-root", "max-jobs",
    };

    private readonly Dictionary<string, List<string>> _values = new(StringComparer.OrdinalIgnoreCase);

    public List<string> Positional { get; } = new();

    public List<string> Passthrough { get; } = new();

    public IEnumerable<string> Names => _values.Keys;

    public static CliArgs Parse(string[] args)
    {
        var result = new CliArgs();

        for (var i = 0; i < args.Length; i++)
        {
            var token = args[i];

            if (token == "--")
            {
                for (var j = i + 1; j < args.Length; j++) result.Passthrough.Add(args[j]);
                break;
            }

            if (token.Length > 1 && token[0] == '-' && token != "-")
            {
                var name = token.TrimStart('-');
                string? value = null;

                var equals = name.IndexOf('=');
                if (equals >= 0)
                {
                    value = name[(equals + 1)..];
                    name = name[..equals];
                }
                else if (ValueOptions.Contains(name))
                {
                    if (i + 1 >= args.Length) throw new CliUsageException($"选项 --{name} 缺少取值。");
                    value = args[++i];
                }

                if (name.Length == 0) throw new CliUsageException("无法解析的选项：" + token);
                result.Add(name, value);
                continue;
            }

            result.Positional.Add(token);
        }

        return result;
    }

    private void Add(string name, string? value)
    {
        if (!_values.TryGetValue(name, out var list))
        {
            list = new List<string>();
            _values[name] = list;
        }

        list.Add(value ?? string.Empty);
    }

    public bool Has(string name) => _values.ContainsKey(name);

    public string? Get(string name)
    {
        if (!_values.TryGetValue(name, out var list)) return null;
        var value = list[^1];
        return value.Length == 0 ? null : value;
    }

    public string[] GetAll(string name) =>
        _values.TryGetValue(name, out var list)
            ? list.Where(v => v.Length > 0).ToArray()
            : Array.Empty<string>();

    public int? GetInt(string name)
    {
        var raw = Get(name);
        if (raw is null) return null;
        if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value))
            throw new CliUsageException($"选项 --{name} 需要整数，实际为：{raw}");
        return value;
    }

    /// <summary>字节 offset 用：日志可超过 2GiB，LogSlice/Cursor 都是 long。</summary>
    public long? GetLong(string name)
    {
        var raw = Get(name);
        if (raw is null) return null;
        if (!long.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value))
            throw new CliUsageException($"选项 --{name} 需要整数，实际为：{raw}");
        return value;
    }
}

// ---------------------------------------------------------------- CLI 专属输出 DTO（不属于 Worker 合同）

/// <summary>日志读取游标：字节 offset + 是否处于行首（prefix 模式跨片段续行时用）。</summary>
internal sealed class LogCursor
{
    public long Offset { get; set; }

    public bool AtLineStart { get; set; } = true;
}

/// <summary>一条日志片段：流名 + 读取结果 + 该流的游标（写回时更新行首状态）。</summary>
internal readonly record struct LogChunk(string Name, LogSlice Slice, LogCursor Cursor);

internal sealed record SubmitResult(
    string Id,
    string Mode,
    string Project,
    string QueueRoot,
    string JobDir,
    int PayloadFiles,
    long PayloadBytes,
    int PayloadSkipped,
    bool WorkerHealthy,
    DateTimeOffset CreatedUtc);

internal sealed record MarkerResult(string? Id, string Path, DateTimeOffset CreatedUtc);

internal sealed record JobView(
    string Id,
    string State,
    string? Mode,
    string? Phase,
    int? ExitCode,
    string? Error,
    DateTimeOffset? UpdatedUtc,
    DateTimeOffset? FinishedUtc,
    bool StatusPresent);

internal sealed record WorkerHealthView(bool Healthy, double AgeSeconds, WorkerState State);

internal sealed record QueueView(string QueueRoot, WorkerHealthView? Worker, JobView[] Jobs);

internal sealed record StreamView(long Offset, long NextOffset, long Length, bool Truncated, string[] Lines);

internal sealed record LogChunkView(string Id, StreamView? Stdout, StreamView? Stderr);

internal sealed record DoctorReport(
    string CliVersion,
    string OperatingSystem,
    string RepoRoot,
    string SourceRoot,
    string QueueRoot,
    bool QueueWritable,
    string? QueueError,
    bool WorkerHealthy,
    WorkerHealthView? Worker,
    int JobCount,
    JobView? LatestJob,
    int SourceFileCount,
    long SourceBytes,
    int SourceSkippedCount,
    string[] SourceExcluded,
    string DotnetVersion,
    bool WindowsInterop,
    string[] BinfmtHandlers,
    string? MntCMount,
    string[] Notes);

internal static class RuntimeInformation2
{
    public static string OsDescription =>
        System.Runtime.InteropServices.RuntimeInformation.OSDescription;
}

/// <summary>Worker 存活判定：只读 worker.json + 心跳年龄；不做 PID 恢复、不杀进程。</summary>
internal sealed record WorkerHealth(bool Present, bool Healthy, double AgeSeconds, WorkerState? State, string Reason)
{
    public static WorkerHealth Check(string queueRoot)
    {
        // 队列根已在 ResolveQueueRoot 校验过；这里再挡一次 worker.json 自身是链接的情况（防读穿）
        try
        {
            WdbPath.AssertNoReparseAncestors(BridgeLayout.WorkerPath(queueRoot), "worker.json");
        }
        catch (WdbException ex)
        {
            return new WorkerHealth(false, false, double.PositiveInfinity, null, ex.Message);
        }

        var state = Store.ReadJson<WorkerState>(BridgeLayout.WorkerPath(queueRoot));
        if (state is null)
            return new WorkerHealth(false, false, double.PositiveInfinity, null, "worker.json 不存在或不可读");

        var age = (DateTimeOffset.UtcNow - state.HeartbeatUtc).TotalSeconds;
        if (age < 0) age = 0;

        var healthy = age <= Cli.WorkerHeartbeatStaleSeconds;
        return new WorkerHealth(true, healthy, age, state,
            healthy ? "ok" : $"心跳已过期 {age:F1}s（阈值 {Cli.WorkerHeartbeatStaleSeconds:F0}s）");
    }

    public string Explain(string queueRoot) =>
        $"Worker 未在运行（{Reason}）。任务保持 queued，直到 Windows 侧启动 Worker；本次不自动启动宿主。" +
        Environment.NewLine + StartHint(queueRoot);

    public static string StartHint(string queueRoot)
    {
        var queueUnc = ToWindowsUnc(queueRoot) ?? @"<队列的 Windows 路径>";
        const string fallbackScript = @"<UNC>\tools\windows-debug-bridge\start-worker.ps1";
        var startScript = FindStartScript();
        var scriptUnc = startScript is null ? fallbackScript : ToWindowsUnc(startScript) ?? fallbackScript;

        return "启动方法（在 Windows 前台执行一次）：" + Environment.NewLine +
               "  powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + scriptUnc + "\" -Queue \"" + queueUnc + "\"";
    }

    private static string? FindStartScript()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, "tools", "windows-debug-bridge", "start-worker.ps1");
            if (File.Exists(candidate)) return candidate;
            dir = dir.Parent;
        }

        return null;
    }

    private static string? ToWindowsUnc(string linuxPath)
    {
        var distro = Environment.GetEnvironmentVariable("WSL_DISTRO_NAME");
        if (string.IsNullOrWhiteSpace(distro)) return null;

        var path = Path.GetFullPath(linuxPath).Replace('/', '\\');
        return @"\\wsl.localhost\" + distro + path;
    }
}
