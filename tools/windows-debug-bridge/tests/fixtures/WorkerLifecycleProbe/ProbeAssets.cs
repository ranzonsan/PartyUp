using System.Globalization;
using System.Text;
using GalleriesManager.Wdb;

namespace Wdb.LifecycleProbe;

/// <summary>
/// 探针自带的资产：**自己的 EchoApp 源备份**（不读桥的 tests/fixtures/EchoApp，避免与主桥
/// payload 快照互相依赖）与「往队列里投一个任务」的最小写作逻辑。
///
/// EchoApp 备份被刻意做成可长驻 + 可派生多层的形态，用来制造真实的 Windows 进程树：
///   --pidfile &lt;file&gt;   每一层都往同一文件追加自己的 PID（探针据此断言存活/消失）
///   --spawn N --depth D  派生 N 个子进程，子进程在 depth&gt;0 时继续派生
///   --repeat             长驻（上限 9 分钟）直到被外部停止
/// </summary>
internal static class ProbeAssets
{
    /// <summary>写 payload：全部文件写完后调用方才允许写 request.json。</summary>
    public static (int FileCount, long TotalBytes) WriteEchoAppPayload(string jobDir)
    {
        var payloadRoot = Path.Combine(jobDir, BridgeLayout.PayloadDirName, "EchoApp");
        Directory.CreateDirectory(payloadRoot);

        var csproj = Path.Combine(payloadRoot, "EchoApp.csproj");
        var program = Path.Combine(payloadRoot, "Program.cs");
        File.WriteAllText(csproj, EchoAppCsproj, Utf8NoBom);
        File.WriteAllText(program, EchoAppProgram, Utf8NoBom);

        return (2, new FileInfo(csproj).Length + new FileInfo(program).Length);
    }

    /// <summary>
    /// 在队列里投一个任务：先写全 payload，最后才原子落 request.json（合同硬性要求）。
    /// </summary>
    public static Request SubmitRunJob(string queueRoot, string id, string[] arguments, int timeoutSeconds)
    {
        var jobDir = BridgeLayout.JobDir(queueRoot, id);
        Directory.CreateDirectory(jobDir);

        var (fileCount, totalBytes) = WriteEchoAppPayload(jobDir);

        var request = new Request(
            BridgeProtocol.Version,
            id,
            JobModes.Run,
            Project: "EchoApp/EchoApp.csproj",
            Configuration: "Debug",
            Framework: null,
            Arguments: arguments,
            TimeoutSeconds: timeoutSeconds,
            Payload: new PayloadRef(BridgeLayout.PayloadDirName, fileCount, totalBytes, ManifestHash: null),
            CreatedUtc: DateTimeOffset.UtcNow,
            SourceRoot: "worker-lifecycle-probe");

        WriteLast(jobDir, BridgeLayout.RequestFileName, BridgeJson.Serialize(request));
        return request;
    }

    /// <summary>请求/状态文件一律写临时文件后原子改名，避免 Worker 读到半截 JSON。</summary>
    public static void WriteLast(string jobDir, string fileName, string content)
    {
        Directory.CreateDirectory(jobDir);
        var target = Path.Combine(jobDir, fileName);
        var temp = target + ".tmp";
        File.WriteAllText(temp, content, Utf8NoBom);
        File.Move(temp, target, overwrite: true);
    }

    public static string NewJobId(string prefix)
        => string.Create(
            CultureInfo.InvariantCulture,
            $"{prefix}-{DateTime.UtcNow:yyyyMMdd'T'HHmmssfff'Z'}-{Guid.NewGuid().ToString("N")[..6]}");

    /// <summary>读 status.json；不存在/半截/解析失败一律返回 null（绝不让探针因此崩）。</summary>
    public static Status? TryReadStatus(string queueRoot, string id)
    {
        var path = BridgeLayout.StatusPath(queueRoot, id);
        try
        {
            if (!File.Exists(path))
            {
                return null;
            }

            var text = File.ReadAllText(path, Encoding.UTF8);
            return string.IsNullOrWhiteSpace(text) ? null : BridgeJson.Deserialize<Status>(text);
        }
        catch (IOException)
        {
            return null;
        }
        catch (System.Text.Json.JsonException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static WorkerState? TryReadWorkerState(string queueRoot)
    {
        var path = BridgeLayout.WorkerPath(queueRoot);
        try
        {
            if (!File.Exists(path))
            {
                return null;
            }

            var text = File.ReadAllText(path, Encoding.UTF8);
            return string.IsNullOrWhiteSpace(text) ? null : BridgeJson.Deserialize<WorkerState>(text);
        }
        catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>pidfile 的一行：进程自报 PID / 层级深度 / 父进程 PID（父 PID 由父进程在 argv 里传下来）。</summary>
    public readonly record struct PidEntry(int Pid, int Depth, int ParentPid);

    /// <summary>
    /// 读 pidfile。行格式 `pid=123 depth=1 ppid=456`；同时容忍裸数字行（老格式），
    /// 解析不出的一律跳过——正在写入的半行不该让探针崩。
    /// </summary>
    public static List<PidEntry> ReadPidEntries(string path)
    {
        var entries = new List<PidEntry>();
        if (!File.Exists(path))
        {
            return entries;
        }

        try
        {
            foreach (var raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                var line = raw.Trim();
                if (line.Length == 0)
                {
                    continue;
                }

                if (!line.Contains('=', StringComparison.Ordinal))
                {
                    if (int.TryParse(line, NumberStyles.Integer, CultureInfo.InvariantCulture, out var bare) && bare > 0)
                    {
                        entries.Add(new PidEntry(bare, -1, -1));
                    }

                    continue;
                }

                int pid = -1, depth = -1, ppid = -1;
                foreach (var part in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
                {
                    var eq = part.IndexOf('=');
                    if (eq <= 0)
                    {
                        continue;
                    }

                    var key = part[..eq];
                    if (!int.TryParse(part[(eq + 1)..], NumberStyles.Integer, CultureInfo.InvariantCulture, out var value))
                    {
                        continue;
                    }

                    switch (key)
                    {
                        case "pid":
                            pid = value;
                            break;
                        case "depth":
                            depth = value;
                            break;
                        case "ppid":
                            ppid = value;
                            break;
                    }
                }

                if (pid > 0)
                {
                    entries.Add(new PidEntry(pid, depth, ppid));
                }
            }
        }
        catch (IOException)
        {
            // 与 EchoApp 的写入竞争：本轮读到的就当本轮结果，调用方会重试。
        }

        return entries;
    }

    public static List<int> ReadPids(string path) => ReadPidEntries(path).Select(e => e.Pid).ToList();

    public static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: false);

    /// <summary>自己的 EchoApp 源备份：自包含、无 NuGet、可长驻可派生。</summary>
    public const string EchoAppCsproj = """
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <AssemblyName>EchoApp</AssemblyName>
    <RootNamespace>EchoApp</RootNamespace>
    <InvariantGlobalization>true</InvariantGlobalization>
    <SatelliteResourceLanguages>en</SatelliteResourceLanguages>
    <ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>
  </PropertyGroup>
</Project>
""";

    public const string EchoAppProgram = """
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Text;

internal static class Program
{
    private static int Main(string[] args)
    {
        var opts = Options.Parse(args);

        // 每一层都登记 PID + 深度 + 父 PID：探针据此断言**完整的三层结构**（不是"至少3个"），
        // 并在停止后按同一份清单确认全部 Gone。父 PID 由父进程在 argv 里传下来（Windows 无 /proc）。
        if (opts.PidFile.Length > 0)
        {
            Append(
                opts.PidFile,
                "pid=" + Environment.ProcessId.ToString(CultureInfo.InvariantCulture)
                + " depth=" + opts.Depth.ToString(CultureInfo.InvariantCulture)
                + " ppid=" + opts.ParentPid.ToString(CultureInfo.InvariantCulture));
        }

        Console.WriteLine("WDB_ECHO_PID " + Environment.ProcessId.ToString(CultureInfo.InvariantCulture)
            + " depth=" + opts.Depth.ToString(CultureInfo.InvariantCulture));
        Console.Out.Flush();

        // 层数语义：根 self depth 恒为 0，子层 depth+1，直到 depth == levels-1 为止。
        var spawned = new List<int>();
        if (opts.Spawn > 0 && opts.Depth + 1 < opts.Levels)
        {
            for (var i = 0; i < opts.Spawn; i++)
            {
                var pid = SpawnSelf(opts, opts.Depth + 1);
                if (pid > 0)
                {
                    spawned.Add(pid);
                }
            }
        }

        if (spawned.Count > 0)
        {
            Console.WriteLine("WDB_ECHO_CHILDREN " + string.Join(',', spawned));
            Console.Out.Flush();
        }

        if (opts.Repeat)
        {
            // 睡眠切成 <=250ms 的片：既保持心跳节奏，又让 --repeat-seconds 的截止时间真正准时生效
            // （整段 Sleep(HeartbeatMs) 会把 12s 的窗口拖成 60s+）。
            var deadline = DateTime.UtcNow.AddSeconds(opts.RepeatSeconds);
            var lastBeat = DateTime.MinValue;
            while (DateTime.UtcNow < deadline)
            {
                if (opts.HeartbeatMs > 0
                    && (DateTime.UtcNow - lastBeat).TotalMilliseconds >= opts.HeartbeatMs)
                {
                    Console.WriteLine("WDB_ECHO_ALIVE " + Environment.ProcessId.ToString(CultureInfo.InvariantCulture));
                    Console.Out.Flush();
                    lastBeat = DateTime.UtcNow;
                }

                Thread.Sleep(250);
            }

            return 0;
        }

        if (opts.SleepMs > 0)
        {
            Thread.Sleep(opts.SleepMs);
        }

        return opts.ExitCode;
    }

    private static int SpawnSelf(Options parent, int depth)
    {
        var psi = new ProcessStartInfo
        {
            UseShellExecute = false,
            RedirectStandardOutput = false,
            RedirectStandardError = false,
        };

        // 被 dotnet 宿主拉起时 ProcessPath 就是 dotnet.exe，argv[0] 必须给**入口 dll 的绝对路径**：
        // 相对名 "EchoApp.dll" 在 cwd 下并不存在，子进程会立刻失败、pidfile 永远只有 1 行
        // （这正是第一轮 5 分钟空等的根因）。
        var self = Environment.ProcessPath ?? string.Empty;
        var isDotnetHost = self.EndsWith("dotnet", StringComparison.OrdinalIgnoreCase)
            || self.EndsWith("dotnet.exe", StringComparison.OrdinalIgnoreCase);
        var entryDll = Assembly.GetEntryAssembly()?.Location
            ?? Path.Combine(AppContext.BaseDirectory, "EchoApp.dll");

        if (isDotnetHost)
        {
            psi.FileName = self;            // dotnet.exe 的绝对路径，不依赖 PATH
            psi.ArgumentList.Add(entryDll);
        }
        else if (self.Length > 0)
        {
            psi.FileName = self;            // apphost：本身就是可执行文件
        }
        else
        {
            psi.FileName = "dotnet";
            psi.ArgumentList.Add(entryDll);
        }

        psi.ArgumentList.Add("--child");
        psi.ArgumentList.Add("--depth");
        psi.ArgumentList.Add(depth.ToString(CultureInfo.InvariantCulture));   // 子进程自己的深度
        psi.ArgumentList.Add("--levels");
        psi.ArgumentList.Add(parent.Levels.ToString(CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("--pidfile");
        psi.ArgumentList.Add(parent.PidFile);
        psi.ArgumentList.Add("--parent-pid");
        psi.ArgumentList.Add(Environment.ProcessId.ToString(CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("--repeat");
        psi.ArgumentList.Add("--repeat-seconds");
        psi.ArgumentList.Add(parent.RepeatSeconds.ToString(CultureInfo.InvariantCulture));
        psi.ArgumentList.Add("--heartbeat-ms");
        psi.ArgumentList.Add("60000");
        if (depth + 1 < parent.Levels)
        {
            psi.ArgumentList.Add("--spawn");
            psi.ArgumentList.Add("1");
        }

        try
        {
            using var child = Process.Start(psi);
            return child?.Id ?? 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("WDB_ECHO_SPAWN_FAILED " + ex.GetType().Name + ": " + ex.Message);
            return 0;
        }
    }

    private static void Append(string path, string line)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        for (var attempt = 0; ; attempt++)
        {
            try
            {
                File.AppendAllText(full, line + Environment.NewLine, new UTF8Encoding(false));
                return;
            }
            catch (IOException) when (attempt < 20)
            {
                Thread.Sleep(25);
            }
        }
    }

    private sealed class Options
    {
        public string PidFile { get; private set; } = string.Empty;

        public int ParentPid { get; private set; } = -1;
        public int Spawn { get; private set; }
        public int Depth { get; private set; }

        public int Levels { get; private set; } = 1;
        public bool Repeat { get; private set; }

        public int RepeatSeconds { get; private set; } = 540;
        public int SleepMs { get; private set; }
        public int ExitCode { get; private set; }
        public int HeartbeatMs { get; private set; }

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--child":
                        break;
                    case "--pidfile":
                        o.PidFile = Next(args, ref i);
                        break;
                    case "--parent-pid":
                        o.ParentPid = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--spawn":
                        o.Spawn = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--depth":
                        o.Depth = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--levels":
                        o.Levels = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--repeat":
                        o.Repeat = true;
                        break;
                    case "--repeat-seconds":
                        o.RepeatSeconds = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--sleep":
                        o.SleepMs = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--exit":
                        o.ExitCode = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    case "--heartbeat-ms":
                        o.HeartbeatMs = int.Parse(Next(args, ref i), CultureInfo.InvariantCulture);
                        break;
                    default:
                        break;
                }
            }

            return o;
        }

        private static string Next(string[] args, ref int i)
            => i + 1 < args.Length ? args[++i] : throw new ArgumentException("缺少取值");
    }
}
""";
}
