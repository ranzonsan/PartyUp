using System.Text.Json;
using GalleriesManager.Wdb;

namespace Wdb.Tests;

/// <summary>
/// 合同回归：把 Contracts.cs 的**字段名、常量取值与路径布局**钉死。
/// 这些断言的作用是"改动必须是有意的"——跨语言手写 JSON 的每一次改名都要先在这里翻红。
/// 手写 JSON 一律按 exact camelCase 字段，任何 DTO 变更都必须同步改这里。
/// </summary>
public static class ContractTests
{
    private static readonly string[] RequestKeys =
    [
        "arguments", "configuration", "createdUtc", "framework", "id", "mode",
        "payload", "project", "protocolVersion", "sourceRoot", "timeoutSeconds",
    ];

    private static readonly string[] PayloadRefKeys = ["fileCount", "manifestHash", "relativePath", "totalBytes"];

    private static readonly string[] StatusKeys =
    [
        "artifact", "error", "exitCode", "finishedUtc", "id", "mode", "phase",
        "pid", "protocolVersion", "startedUtc", "state", "updatedUtc",
    ];

    private static readonly string[] WorkerStateKeys =
    [
        "currentJobId", "heartbeatUtc", "hostName", "pid", "protocolVersion", "queueRoot",
        "runsRoot", "startedUtc", "version", "workerId",
    ];

    private static readonly string[] PayloadReportKeys =
    [
        "createdUtc", "excluded", "fileCount", "id", "manifestHash", "protocolVersion",
        "skipped", "skippedCount", "sourceRoot", "totalBytes",
    ];

    [WdbFact("contract.protocol-version-pinned")]
    public static void ProtocolVersionPinned()
    {
        // 改这个数字=破坏 CLI/Worker 兼容性，必须是一次有意识的改动。
        WdbAssert.Equal(1, BridgeProtocol.Version);
    }

    [WdbFact("contract.layout-exact-paths")]
    public static void LayoutExactPaths()
    {
        const string queue = "/tmp/q";
        WdbAssert.Equal(Path.Combine(queue, "jobs"), BridgeLayout.JobsDir(queue));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc"), BridgeLayout.JobDir(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "request.json"), BridgeLayout.RequestPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "status.json"), BridgeLayout.StatusPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "payload"), BridgeLayout.PayloadPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "payload-report.json"), BridgeLayout.PayloadReportPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "stdout.log"), BridgeLayout.StdoutPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "stderr.log"), BridgeLayout.StderrPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "jobs", "abc", "cancel"), BridgeLayout.CancelPath(queue, "abc"));
        WdbAssert.Equal(Path.Combine(queue, "worker.json"), BridgeLayout.WorkerPath(queue));
        WdbAssert.Equal(Path.Combine(queue, "shutdown"), BridgeLayout.ShutdownPath(queue));

        // 字面量同样钉死：跨端手写文件名的唯一事实来源。
        WdbAssert.Equal("jobs", BridgeLayout.JobsDirName);
        WdbAssert.Equal("request.json", BridgeLayout.RequestFileName);
        WdbAssert.Equal("status.json", BridgeLayout.StatusFileName);
        WdbAssert.Equal("payload", BridgeLayout.PayloadDirName);
        WdbAssert.Equal("payload-report.json", BridgeLayout.PayloadReportFileName);
        WdbAssert.Equal("stdout.log", BridgeLayout.StdoutFileName);
        WdbAssert.Equal("stderr.log", BridgeLayout.StderrFileName);
        WdbAssert.Equal("cancel", BridgeLayout.CancelFileName);
        WdbAssert.Equal("worker.json", BridgeLayout.WorkerFileName);
        WdbAssert.Equal("shutdown", BridgeLayout.ShutdownFileName);
    }

    [WdbFact("contract.layout-stays-under-queue-root")]
    public static void LayoutStaysUnderQueueRoot()
    {
        const string queue = "/tmp/q";
        var prefix = Path.Combine(queue, "jobs") + Path.DirectorySeparatorChar;
        foreach (var p in new[]
                 {
                     BridgeLayout.JobDir(queue, "id"), BridgeLayout.RequestPath(queue, "id"),
                     BridgeLayout.StatusPath(queue, "id"), BridgeLayout.PayloadPath(queue, "id"),
                     BridgeLayout.PayloadReportPath(queue, "id"), BridgeLayout.StdoutPath(queue, "id"),
                     BridgeLayout.StderrPath(queue, "id"), BridgeLayout.CancelPath(queue, "id"),
                 })
        {
            WdbAssert.StartsWith(prefix, Path.GetFullPath(p), $"任务内路径越出 jobs/ ：{p}");
        }

        WdbAssert.StartsWith(Path.GetFullPath(queue) + Path.DirectorySeparatorChar, Path.GetFullPath(BridgeLayout.WorkerPath(queue)));
        WdbAssert.StartsWith(Path.GetFullPath(queue) + Path.DirectorySeparatorChar, Path.GetFullPath(BridgeLayout.ShutdownPath(queue)));
    }

    [WdbFact("contract.modes-and-states-exact")]
    public static void ModesAndStatesExact()
    {
        WdbAssert.Equal("build", JobModes.Build);
        WdbAssert.Equal("run", JobModes.Run);
        WdbAssert.Equal("buildRun", JobModes.BuildRun);

        WdbAssert.True(JobModes.IsKnown(JobModes.Build));
        WdbAssert.True(JobModes.IsKnown(JobModes.Run));
        WdbAssert.True(JobModes.IsKnown(JobModes.BuildRun));

        // 阴性对照：大小写/别名不是合法模式（跨端拼错必须被拒，而不是被宽容成 run）。
        WdbAssert.False(JobModes.IsKnown("Build"));
        WdbAssert.False(JobModes.IsKnown("buildrun"));
        WdbAssert.False(JobModes.IsKnown("build-run"));
        WdbAssert.False(JobModes.IsKnown(""));
        WdbAssert.False(JobModes.IsKnown(null));

        WdbAssert.Equal("queued", JobStates.Queued);
        WdbAssert.Equal("running", JobStates.Running);
        WdbAssert.Equal("succeeded", JobStates.Succeeded);
        WdbAssert.Equal("failed", JobStates.Failed);
        WdbAssert.Equal("canceled", JobStates.Canceled);
        WdbAssert.Equal("timeout", JobStates.Timeout);
        WdbAssert.Equal("rejected", JobStates.Rejected);
        WdbAssert.Equal("interrupted", JobStates.Interrupted);

        WdbAssert.Equal("submitted", JobPhases.Submitted);
        WdbAssert.Equal("done", JobPhases.Done);
    }

    [WdbFact("contract.terminal-states-exact")]
    public static void TerminalStatesExact()
    {
        foreach (var terminal in new[] { "succeeded", "failed", "canceled", "timeout", "rejected", "interrupted" })
        {
            WdbAssert.True(JobStates.IsTerminal(terminal), $"终态被漏判：{terminal}");
        }

        // 阴性对照：非终态绝不能被当成终态，否则 wait 会立刻假返回。
        foreach (var nonTerminal in new[] { "queued", "running", "", null, "Succeeded", "done" })
        {
            WdbAssert.False(JobStates.IsTerminal(nonTerminal), $"非终态被误判为终态：{nonTerminal ?? "<null>"}");
        }
    }

    [WdbFact("contract.request-json-field-names-exact")]
    public static void RequestFieldNamesExact()
    {
        var request = SampleRequest();
        var json = BridgeJson.Serialize(request);
        WdbAssert.SequenceEqual(RequestKeys, TopLevelKeys(json));

        // 阴性对照：禁止出现别名或状态字段（父代理约定：Request.Id 而非 jobId）。
        WdbAssert.NotContains("\"jobId\"", json);
        WdbAssert.NotContains("\"job_id\"", json);
        WdbAssert.NotContains("Id\"", json.Replace("\"id\"", string.Empty));
        WdbAssert.NotContains("\"state\"", json);
        WdbAssert.NotContains("\"status\"", json);
    }

    [WdbFact("contract.payload-ref-field-names-and-defaults-exact")]
    public static void PayloadRefFieldNamesAndDefaultsExact()
    {
        var payload = new PayloadRef();
        WdbAssert.Equal("payload", payload.RelativePath);
        WdbAssert.Equal(0, payload.FileCount);
        WdbAssert.Equal(0L, payload.TotalBytes);
        WdbAssert.Null(payload.ManifestHash);

        // IgnoreCondition.Never：默认值也必须出现在 JSON 里（Worker 依赖字段存在）。
        var json = BridgeJson.Serialize(payload);
        WdbAssert.SequenceEqual(PayloadRefKeys, TopLevelKeys(json));
        WdbAssert.Equal("payload", GetString(json, "relativePath"));
    }

    [WdbFact("contract.status-json-field-names-exact")]
    public static void StatusFieldNamesExact()
    {
        var status = new Status(
            BridgeProtocol.Version, "id-1", JobModes.BuildRun, JobStates.Running, JobPhases.Building,
            ExitCode: null, Pid: 4242, Error: null, Artifact: null,
            UpdatedUtc: DateTimeOffset.Parse("2026-10-11T09:30:00+08:00"),
            StartedUtc: DateTimeOffset.Parse("2026-10-11T09:29:59+08:00"), FinishedUtc: null);

        var json = BridgeJson.Serialize(status);
        WdbAssert.SequenceEqual(StatusKeys, TopLevelKeys(json));

        // null 字段也必须存在（DefaultIgnoreCondition.Never），CLI 才能区分"没写"与"写了 null"。
        WdbAssert.Contains("\"exitCode\":null", json);
        WdbAssert.Contains("\"finishedUtc\":null", json);
        WdbAssert.Equal(4242, GetInt(json, "pid"));
        WdbAssert.Equal("running", GetString(json, "state"));
    }

    [WdbFact("contract.worker-state-field-names-exact")]
    public static void WorkerStateFieldNamesExact()
    {
        var state = new WorkerState(
            BridgeProtocol.Version, "w-1", 777, "HOST", "//wsl.localhost/ai-dev/q", @"C:\runs",
            "0.0.0-test", DateTimeOffset.Parse("2026-10-11T09:00:00+08:00"),
            DateTimeOffset.Parse("2026-10-11T09:00:01+08:00"), CurrentJobId: null);

        var json = BridgeJson.Serialize(state);
        WdbAssert.SequenceEqual(WorkerStateKeys, TopLevelKeys(json));
        WdbAssert.Contains("\"currentJobId\":null", json);
    }

    [WdbFact("contract.payload-report-field-names-exact")]
    public static void PayloadReportFieldNamesExact()
    {
        var report = new PayloadReport(
            BridgeProtocol.Version, "id-1", "/tmp/x", 3, 1234, 1, ["bin/x.dll"],
            ["bin", "obj"], "hash", DateTimeOffset.Parse("2026-10-11T09:30:00+08:00"));

        var json = BridgeJson.Serialize(report);
        WdbAssert.SequenceEqual(PayloadReportKeys, TopLevelKeys(json));
        WdbAssert.Contains("\"skipped\":[\"bin/x.dll\"]", json);
    }

    [WdbFact("contract.json-non-ascii-not-escaped")]
    public static void NonAsciiNotEscaped()
    {
        var request = SampleRequest() with { SourceRoot = "/tmp/画廊 目录" };
        var json = BridgeJson.Serialize(request);

        // 中文必须原样可读（UnsafeRelaxedJsonEscaping）；转义成 \uXXXX 会让人工核对与跨端 diff 变成盲区。
        WdbAssert.Contains("画廊 目录", json);
        WdbAssert.NotContains("\\u753B", json);
    }

    [WdbFact("contract.json-tolerant-read-and-exact-write")]
    public static void TolerantReadAndExactWrite()
    {
        // 手写 JSON：字段名一律 exact camelCase。
        const string raw = """
        {
          // 注释与尾逗号都必须被容忍（人工排查时会编辑队列文件）
          "protocolVersion": 1,
          "id": "20261011-abcdef",
          "mode": "buildRun",
          "project": "tests/fixtures/EchoApp/EchoApp.csproj",
          "configuration": "Release",
          "framework": "net10.0",
          "arguments": ["a b", "中文 参数", "quote\"inside", "", "--exit", "7"],
          "timeoutSeconds": 0,
          "payload": { "relativePath": "payload", "fileCount": 3, "totalBytes": 42, "manifestHash": "ab12" },
          "createdUtc": "2026-10-11T09:30:00+08:00",
          "sourceRoot": "/tmp/project",
        }
        """;

        var request = BridgeJson.Deserialize<Request>(raw);
        WdbAssert.NotNull(request, "手写 JSON 反序列化失败");
        WdbAssert.Equal(1, request!.ProtocolVersion);
        WdbAssert.Equal("20261011-abcdef", request.Id);
        WdbAssert.Equal("buildRun", request.Mode);
        WdbAssert.Equal("Release", request.Configuration);
        WdbAssert.Equal("net10.0", request.Framework);
        WdbAssert.Equal(0, request.TimeoutSeconds);
        WdbAssert.Equal("payload", request.Payload.RelativePath);
        WdbAssert.Equal(3, request.Payload.FileCount);
        WdbAssert.Equal(42L, request.Payload.TotalBytes);
        WdbAssert.Equal("ab12", request.Payload.ManifestHash);

        // argv 必须逐字节无损（含空格、中文、内嵌引号、空串）——这是 A3 的合同层前置条件。
        WdbAssert.SequenceEqual(
            new[] { "a b", "中文 参数", "quote\"inside", "", "--exit", "7" },
            request.Arguments ?? []);

        // 时间戳带偏移量必须原样保留（不能悄悄转成 UTC 丢失 +08:00）。
        WdbAssert.Equal(TimeSpan.FromHours(8), request.CreatedUtc.Offset);
    }

    [WdbFact("contract.wrong-field-name-is-not-aliased")]
    public static void WrongFieldNameIsNotAliased()
    {
        // 阴性对照：写成 jobId 时 Id 必须为 null（说明合同没有偷偷做别名容错）。
        const string wrong = """
        { "protocolVersion": 1, "jobId": "x", "mode": "run", "project": "p.csproj", "payload": {} }
        """;
        var request = BridgeJson.Deserialize<Request>(wrong);
        WdbAssert.NotNull(request);
        WdbAssert.Null(request!.Id, "jobId 被当成了 Id 的别名——合同已被静默放宽");

        // 阳性对照：写对字段名时确实能拿到值。
        const string right = """
        { "protocolVersion": 1, "id": "x", "mode": "run", "project": "p.csproj", "payload": {} }
        """;
        WdbAssert.Equal("x", BridgeJson.Deserialize<Request>(right)!.Id);
    }

    // ---- helpers ------------------------------------------------------------

    private static Request SampleRequest() => new(
        BridgeProtocol.Version,
        "20261011-abcdef",
        JobModes.BuildRun,
        "tools/windows-debug-bridge/tests/fixtures/EchoApp/EchoApp.csproj",
        "Release",
        "net10.0",
        ["a b", "中文 参数", "quote\"inside", ""],
        0,
        new PayloadRef("payload", 3, 42, "ab12"),
        DateTimeOffset.Parse("2026-10-11T09:30:00+08:00"),
        "/tmp/project");

    private static string[] TopLevelKeys(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return doc.RootElement.EnumerateObject().Select(p => p.Name).OrderBy(n => n, StringComparer.Ordinal).ToArray();
    }

    private static string? GetString(string json, string name)
    {
        using var doc = JsonDocument.Parse(json);
        return doc.RootElement.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
    }

    private static int? GetInt(string json, string name)
    {
        using var doc = JsonDocument.Parse(json);
        return doc.RootElement.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetInt32() : null;
    }
}
