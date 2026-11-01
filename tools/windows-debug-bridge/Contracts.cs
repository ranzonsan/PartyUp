// Windows 调试桥 · 共享合同（CLI 与 Worker 共用）
//
// 纯 BCL：只依赖 System.Text.Json 与 System.IO 的路径组合。
// 本文件不含任何文件 I/O —— 落盘语义在 Store.cs，目录/文件名的唯一事实来源是 BridgeLayout。
//
// 所有权约定（硬性）：
//   * request.json 只由 CLI 写；且必须是任务目录里最后一个被写入的文件（先 payload、后 request）。
//   * status.json / worker.json 只由 Worker 写；CLI 只读。
//   * cancel / shutdown 是「空标记文件」：文件存在即表示信号，内容不参与语义。
using System.Text.Json;
using System.Text.Json.Serialization;

namespace GalleriesManager.Wdb;

/// <summary>线上协议版本。CLI 与 Worker 不一致时必须拒绝（见 Request.ProtocolVersion 校验）。</summary>
public static class BridgeProtocol
{
    public const int Version = 1;
}

/// <summary>队列目录布局（唯一事实来源：两边都不许自己拼字符串）。</summary>
public static class BridgeLayout
{
    public const string JobsDirName = "jobs";
    public const string RequestFileName = "request.json";
    public const string StatusFileName = "status.json";
    public const string PayloadDirName = "payload";
    public const string PayloadReportFileName = "payload-report.json";
    public const string StdoutFileName = "stdout.log";
    public const string StderrFileName = "stderr.log";
    public const string CancelFileName = "cancel";
    public const string WorkerFileName = "worker.json";
    public const string ShutdownFileName = "shutdown";

    public static string JobsDir(string queueRoot) => Path.Combine(queueRoot, JobsDirName);

    public static string JobDir(string queueRoot, string id) => Path.Combine(JobsDir(queueRoot), id);

    public static string RequestPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), RequestFileName);

    public static string StatusPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), StatusFileName);

    public static string PayloadPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), PayloadDirName);

    public static string PayloadReportPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), PayloadReportFileName);

    public static string StdoutPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), StdoutFileName);

    public static string StderrPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), StderrFileName);

    public static string CancelPath(string queueRoot, string id) => Path.Combine(JobDir(queueRoot, id), CancelFileName);

    public static string WorkerPath(string queueRoot) => Path.Combine(queueRoot, WorkerFileName);

    public static string ShutdownPath(string queueRoot) => Path.Combine(queueRoot, ShutdownFileName);
}

/// <summary>任务模式（Request.Mode 的取值）。</summary>
public static class JobModes
{
    public const string Build = "build";
    public const string Run = "run";
    public const string BuildRun = "buildRun";

    public static bool IsKnown(string? mode) => mode is Build or Run or BuildRun;
}

/// <summary>任务状态（Status.State 的取值）。全部小写单词，便于跨语言解析。</summary>
public static class JobStates
{
    public const string Queued = "queued";
    public const string Running = "running";
    public const string Succeeded = "succeeded";
    public const string Failed = "failed";
    public const string Canceled = "canceled";
    public const string Timeout = "timeout";
    public const string Rejected = "rejected";

    /// <summary>中断（例如 Worker 在执行中被停止/崩溃后由重启的 Worker 补写）。属终态，wait 必须返回而非永久等待。</summary>
    public const string Interrupted = "interrupted";

    /// <summary>终态：wait 见终态即返回（canceled / interrupted 都不得永久阻塞）。</summary>
    public static bool IsTerminal(string? state) =>
        state is Succeeded or Failed or Canceled or Timeout or Rejected or Interrupted;
}

/// <summary>任务阶段（Status.Phase 的取值，自由文本但建议取这些常量，便于人读）。</summary>
public static class JobPhases
{
    public const string Submitted = "submitted";
    public const string Copying = "copying";
    public const string Restoring = "restoring";
    public const string Building = "building";
    public const string Starting = "starting";
    public const string Running = "running";
    public const string Stopping = "stopping";
    public const string Done = "done";
}

/// <summary>任务请求：CLI 写 &lt;queue&gt;/jobs/&lt;id&gt;/request.json，且必须是该目录最后一个写入的文件。</summary>
/// <param name="ProtocolVersion">必须等于 <see cref="BridgeProtocol.Version"/>，否则 Worker 以 rejected 拒绝。</param>
/// <param name="Id">任务 id，同时是任务目录名（CLI 用 Store.NewId 生成，保证 Windows 合法文件名）。</param>
/// <param name="Mode">见 <see cref="JobModes"/>。</param>
/// <param name="Project">仓库相对路径、以 '/' 分隔、以 .csproj 结尾（CLI 已校验存在且未越界）。</param>
/// <param name="Configuration">可选；null 表示由 Worker/项目默认决定（建议 Debug）。</param>
/// <param name="Framework">可选；显式指定 TargetFramework（如 net10.0），null 表示不覆盖。</param>
/// <param name="Arguments">可选；仅在 mode=run/buildRun 时作为被启动程序的 argv（不做 shell 解析）。</param>
/// <param name="TimeoutSeconds">Worker 侧的硬超时（&lt;=0 表示不限）。超时后必须停掉进程树并写 timeout。</param>
/// <param name="Payload">源快照引用，见 <see cref="PayloadRef"/>。</param>
/// <param name="CreatedUtc">请求创建时间。</param>
/// <param name="SourceRoot">仅用于诊断/日志的 WSL 侧源目录绝对路径。</param>
public sealed record Request(
    int ProtocolVersion,
    string Id,
    string Mode,
    string Project,
    string? Configuration,
    string? Framework,
    string[]? Arguments,
    int TimeoutSeconds,
    PayloadRef Payload,
    DateTimeOffset CreatedUtc,
    string? SourceRoot);

/// <summary>
/// 源快照引用。<see cref="RelativePath"/> 相对任务目录（默认 "payload"），
/// Worker 负责把它复制到 Windows 本机专属副本后再构建/运行 —— Worker 绝不在队列目录（UNC）内构建。
/// </summary>
/// <param name="RelativePath">相对任务目录的快照根，固定为 "payload"。</param>
/// <param name="FileCount">CLI 快照的文件数（诊断用；0 表示未统计）。</param>
/// <param name="TotalBytes">CLI 快照的字节总数（诊断用）。</param>
/// <param name="ManifestHash">路径+大小清单的 SHA-256 十六进制（小写）；null 表示未计算。</param>
public sealed record PayloadRef(
    string RelativePath = "payload",
    int FileCount = 0,
    long TotalBytes = 0,
    string? ManifestHash = null);

/// <summary>任务状态：Worker 写 &lt;queue&gt;/jobs/&lt;id&gt;/status.json（原子覆盖写），CLI 只读。</summary>
/// <param name="ProtocolVersion">写状态时的协议版本。</param>
/// <param name="Id">任务 id。</param>
/// <param name="Mode">冗余自 Request.Mode，便于 CLI 只读 status 即可渲染。</param>
/// <param name="State">见 <see cref="JobStates"/>；只有终态才允许 <see cref="FinishedUtc"/> 非空。</param>
/// <param name="Phase">人读阶段，见 <see cref="JobPhases"/>。</param>
/// <param name="ExitCode">子进程退出码；null 表示尚未退出（或非进程类失败）。</param>
/// <param name="Pid">被启动进程（或其宿主 dotnet）的 Windows PID；null 表示尚未启动。</param>
/// <param name="Error">失败原因/异常摘要；非终态时可为 null。</param>
/// <param name="Artifact">构建产物或启动入口的 Windows 绝对路径（诊断用）。</param>
/// <param name="UpdatedUtc">本状态写入时间（Worker 每次更新都要刷新）。</param>
/// <param name="StartedUtc">任务开始执行时间。</param>
/// <param name="FinishedUtc">终态写入时间。</param>
public sealed record Status(
    int ProtocolVersion,
    string Id,
    string Mode,
    string State,
    string? Phase,
    int? ExitCode,
    int? Pid,
    string? Error,
    string? Artifact,
    DateTimeOffset UpdatedUtc,
    DateTimeOffset? StartedUtc,
    DateTimeOffset? FinishedUtc);

/// <summary>Worker 心跳：Worker 写 &lt;queue&gt;/worker.json（原子覆盖写，建议 1s 一次）。</summary>
/// <param name="ProtocolVersion">协议版本。</param>
/// <param name="WorkerId">本次 Worker 进程的随机会话 id（重启即变）。</param>
/// <param name="Pid">Worker 进程的 Windows PID。</param>
/// <param name="HostName">Windows 机器名。</param>
/// <param name="QueueRoot">Worker 使用的队列根（Windows 侧路径，通常为 UNC）。</param>
/// <param name="RunsRoot">Worker 的本地运行根（Windows 绝对路径）。</param>
/// <param name="Version">Worker 版本串。</param>
/// <param name="StartedUtc">Worker 启动时间。</param>
/// <param name="HeartbeatUtc">最近一次心跳时间（CLI 以 &gt;15s 视为失联，仅告警不杀进程）。</param>
/// <param name="CurrentJobId">当前正在执行的任务 id；空闲为 null。</param>
public sealed record WorkerState(
    int ProtocolVersion,
    string WorkerId,
    int Pid,
    string HostName,
    string QueueRoot,
    string RunsRoot,
    string Version,
    DateTimeOffset StartedUtc,
    DateTimeOffset HeartbeatUtc,
    string? CurrentJobId);

/// <summary>源快照报告：CLI 写 &lt;queue&gt;/jobs/&lt;id&gt;/payload-report.json，供 Worker/诊断查看。</summary>
/// <param name="ProtocolVersion">协议版本。</param>
/// <param name="Id">任务 id。</param>
/// <param name="SourceRoot">WSL 侧源目录绝对路径。</param>
/// <param name="FileCount">已复制文件数。</param>
/// <param name="TotalBytes">已复制字节总数。</param>
/// <param name="SkippedCount">被跳过的条目数（超大文件/符号链接），见 <see cref="Skipped"/>。</param>
/// <param name="Skipped">跳过条目的相对路径（最多保留前 200 条）。</param>
/// <param name="Excluded">命中的排除目录名（去重，便于核对"没有误排除根 props"）。</param>
/// <param name="ManifestHash">路径+大小清单的 SHA-256（小写）；与 <see cref="PayloadRef.ManifestHash"/> 一致。</param>
/// <param name="CreatedUtc">快照完成时间。</param>
public sealed record PayloadReport(
    int ProtocolVersion,
    string Id,
    string SourceRoot,
    int FileCount,
    long TotalBytes,
    int SkippedCount,
    string[] Skipped,
    string[] Excluded,
    string? ManifestHash,
    DateTimeOffset CreatedUtc);

/// <summary>JSON 约定：camelCase、枚举/状态一律字符串、允许注释与尾逗号、非 ASCII 不转义（中文可读）。</summary>
public static class BridgeJson
{
    public static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static string Serialize<T>(T value) => JsonSerializer.Serialize(value, Options);

    public static T? Deserialize<T>(string json) where T : class =>
        JsonSerializer.Deserialize<T>(json, Options);
}
