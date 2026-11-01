using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace EchoApp;

/// <summary>
/// 被测宿主程序：把真实的进程事实（argv 逐字节、cwd、PID、父子关系、运行时版本）
/// 以单行 JSON 输出，供桥的集成测试做逐字节与进程树断言。
///
/// 硬性约束：
///   * 只输出到 stdout/stderr，不写任何业务目录（除调用方显式给出的 marker/cwd-file）；
///   * argv 同时给出 UTF-8 十六进制，避免控制台代码页把比较结果污染成"看起来对"；
///   * 启动哨兵（--marker）在任何其它副作用之前写入，用于证明"进程是否真的被启动"。
/// </summary>
internal static class Program
{
    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: false);

    private static int Main(string[] args)
    {
        Options opts;
        try
        {
            opts = Options.Parse(args);
        }
        catch (Exception ex)
        {
            WriteRawStdout(Encoding.UTF8.GetBytes($"WDB_ECHO_BADARGS {ex.Message}\n"));
            return 64;
        }

        // 1) 启动哨兵：进程真实存在的最早证据（在派生、输出、sleep 之前）。
        if (!opts.IsChild && opts.Marker is { Length: > 0 } markerPath)
        {
            var full = Path.GetFullPath(markerPath);
            var dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.AppendAllText(full, $"{Environment.ProcessId}\n", Utf8NoBom);
        }

        // 2) 记录 cwd（用于断言工作目录是否被尊重）。
        if (!opts.IsChild && opts.CwdFile is { Length: > 0 } cwdFile)
        {
            var full = Path.GetFullPath(cwdFile);
            var dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.WriteAllText(full, Environment.CurrentDirectory, Utf8NoBom);
        }

        // 3) 全层级登记 PID（进程树停止测试用）：每一层都往同一个 pidfile 追加自己的 PID。
        if (opts.PidFile is { Length: > 0 } pidFile)
        {
            var full = Path.GetFullPath(pidFile);
            var dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.AppendAllText(full, $"{Environment.ProcessId}\n", Utf8NoBom);
        }

        // 4) 派生后代进程（进程树停止测试用），孙进程由子进程自行派生。
        var childPids = new List<int>();
        if (opts.Spawn > 0)
        {
            for (var i = 0; i < opts.Spawn; i++)
            {
                var pid = SpawnSelf(opts, depth: opts.Depth - 1);
                if (pid > 0)
                {
                    childPids.Add(pid);
                }
            }
        }

        // 5) 结构化输出（子进程只安静地活着，不污染父进程的证据通道）。
        //    链证明 sidecar 在派生之后写，便于把它自己认到的子 PID 一并落盘。
        AppendLevelRecord(opts, childPids);

        if (!opts.IsChild)
        {
            var payload = new Dictionary<string, object?>
            {
                ["argv"] = args,
                ["argvHex"] = args.Select(Hex).ToArray(),
                ["argc"] = args.Length,
                ["cwd"] = Environment.CurrentDirectory,
                ["pid"] = Environment.ProcessId,
                ["ppid"] = ParentPid(),
                ["runtime"] = Environment.Version.ToString(),
                ["framework"] = System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription,
                ["os"] = System.Runtime.InteropServices.RuntimeInformation.OSDescription,
                ["childPids"] = childPids,
                ["isChild"] = opts.IsChild,
                ["level"] = opts.Level,
                ["parentPidKernel"] = ParentPid(),
            };

            var json = JsonSerializer.Serialize(payload);
            WriteRawStdout(Utf8NoBom.GetBytes("WDB_ECHO " + json + "\n"));
        }

        if (opts.StderrText is { } stderrText)
        {
            WriteRawStderr(Utf8NoBom.GetBytes("WDB_STDERR " + stderrText + "\n"));
        }

        if (opts.Bytes > 0)
        {
            var chunk = new byte[64 * 1024];
            Array.Fill(chunk, (byte)'x');
            var remaining = opts.Bytes;
            WriteRawStdout(Encoding.ASCII.GetBytes("<<BULK>>"));
            while (remaining > 0)
            {
                var n = Math.Min(remaining, chunk.Length);
                WriteRawStdout(chunk.AsSpan(0, n));
                remaining -= n;
            }

            WriteRawStdout(Encoding.ASCII.GetBytes("<<ENDBULK>>\n"));
        }

        if (opts.BadUtf8)
        {
            // 非 UTF-8 的裸字节：桥必须定义好如何处理，且不能让结果 JSON 变成不可解析。
            ReadOnlySpan<byte> junk = [0x80, 0xFF, 0xC3, 0x28, 0xFE, 0x0A];
            WriteRawStdout(Encoding.ASCII.GetBytes("<<BAD>>"));
            WriteRawStdout(junk);
            WriteRawStdout(Encoding.ASCII.GetBytes("<<END>>\n"));

            WriteRawStderr(Encoding.ASCII.GetBytes("<<BAD>>"));
            WriteRawStderr(junk);
            WriteRawStderr(Encoding.ASCII.GetBytes("<<END>>\n"));
        }

        // 6) 生命周期：--repeat 表示长驻直到被停止（stop/cancel 测试用）。
        if (opts.Repeat)
        {
            var deadline = DateTime.UtcNow.AddMinutes(10);
            while (DateTime.UtcNow < deadline)
            {
                Thread.Sleep(50);
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
        var exe = Environment.ProcessPath;
        var argList = new List<string> { "--child" };
        if (parent.PidFile is { Length: > 0 } pidFile)
        {
            argList.Add("--pidfile");
            argList.Add(pidFile);
        }

        if (parent.LevelFile is { Length: > 0 } levelFile)
        {
            argList.Add("--level-file");
            argList.Add(levelFile);
        }

        // 层级与自报父 PID：链证明需要每层各自的 level，且能交叉校验内核真值。
        argList.Add("--level");
        argList.Add((parent.Level + 1).ToString(CultureInfo.InvariantCulture));
        argList.Add("--parent-pid");
        argList.Add(Environment.ProcessId.ToString(CultureInfo.InvariantCulture));

        if (depth > 0)
        {
            argList.Add("--depth");
            argList.Add(depth.ToString(CultureInfo.InvariantCulture));
            argList.Add("--spawn");
            argList.Add("1");
        }

        argList.Add("--sleep");
        argList.Add("300000");

        var psi = new ProcessStartInfo
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        if (!string.IsNullOrEmpty(exe) && !exe.EndsWith("dotnet", StringComparison.Ordinal))
        {
            psi.FileName = exe;
            foreach (var a in argList)
            {
                psi.ArgumentList.Add(a);
            }
        }
        else
        {
            // 回退：dotnet <assembly>（仅在 apphost 不可用时发生）
            psi.FileName = "dotnet";
            psi.ArgumentList.Add(Environment.ProcessPath ?? "EchoApp.dll");
            foreach (var a in argList)
            {
                psi.ArgumentList.Add(a);
            }
        }

        try
        {
            using var p = Process.Start(psi);
            return p?.Id ?? 0;
        }
        catch (Exception ex)
        {
            WriteRawStderr(Utf8NoBom.GetBytes($"WDB_SPAWN_FAILED {ex.GetType().Name}: {ex.Message}\n"));
            return 0;
        }
    }

    private static int ParentPid()
    {
        // Windows：内核真值（Toolhelp32 快照的 th32ParentProcessID），拿不到则回退 -1。
        var (kernelPpid, _) = ParentInfo();
        if (kernelPpid > 0)
        {
            return kernelPpid;
        }

        // Linux：/proc/self/stat 第 4 个字段即 PPID（comm 可能含空格与括号，按最后一个 ')' 切分）。
        try
        {
            var stat = File.ReadAllText("/proc/self/stat");
            var close = stat.LastIndexOf(')');
            if (close > 0 && close + 1 < stat.Length)
            {
                var rest = stat[(close + 1)..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (rest.Length >= 2 && int.TryParse(rest[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out var ppid))
                {
                    return ppid;
                }
            }
        }
        catch
        {
            // 非 Linux 或读取失败：明确返回 -1，不编造。
        }

        return -1;
    }

    // ---- 内核真值父进程（Windows Toolhelp32 快照；只读，不改任何状态）----
    private const uint TH32CS_SNAPPROCESS = 0x00000002;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct PROCESSENTRY32W
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szExeFile;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32FirstW(IntPtr hSnapshot, ref PROCESSENTRY32W lppe);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32NextW(IntPtr hSnapshot, ref PROCESSENTRY32W lppe);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    /// <summary>返回 (自己的内核父 PID, 父进程可执行文件名)；非 Windows 或失败返回 (-1, "")。</summary>
    private static (int ParentPid, string ParentExe) ParentInfo()
    {
        if (!OperatingSystem.IsWindows())
        {
            return (-1, string.Empty);
        }

        var snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == IntPtr.Zero || snapshot == new IntPtr(-1))
        {
            return (-1, string.Empty);
        }

        try
        {
            var selfPid = Environment.ProcessId;
            var entry = new PROCESSENTRY32W { dwSize = (uint)Marshal.SizeOf<PROCESSENTRY32W>() };
            var parentOfSelf = -1;
            var byPid = new Dictionary<int, string>();

            if (Process32FirstW(snapshot, ref entry))
            {
                do
                {
                    var pid = (int)entry.th32ProcessID;
                    byPid[pid] = entry.szExeFile ?? string.Empty;
                    if (pid == selfPid)
                    {
                        parentOfSelf = (int)entry.th32ParentProcessID;
                    }

                    entry.dwSize = (uint)Marshal.SizeOf<PROCESSENTRY32W>();
                }
                while (Process32NextW(snapshot, ref entry));
            }

            var exe = parentOfSelf > 0 && byPid.TryGetValue(parentOfSelf, out var name) ? name : string.Empty;
            return (parentOfSelf, exe);
        }
        catch
        {
            return (-1, string.Empty);
        }
        finally
        {
            CloseHandle(snapshot);
        }
    }

    /// <summary>可选 sidecar：每层各追加一行 JSON，用于证明「3 级链」而不是「root + 两个直系兄弟」。</summary>
    private static void AppendLevelRecord(Options opts, IReadOnlyList<int> childPids)
    {
        if (opts.LevelFile is not { Length: > 0 } levelFile)
        {
            return;
        }

        var full = Path.GetFullPath(levelFile);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        var (_, parentExe) = ParentInfo();
        var record = new Dictionary<string, object?>
        {
            ["pid"] = Environment.ProcessId,
            ["parentPidKernel"] = ParentPid(),      // 内核真值：Windows=Toolhelp32，Linux=/proc/self/stat
            ["parentExeKernel"] = parentExe,        // 父进程可执行文件名（自我复制链 ⇒ 应为 EchoApp*）
            ["parentPidArg"] = opts.ParentPidArg,   // 派生方显式传入的自报父 PID（交叉校验用）
            ["level"] = opts.Level,
            ["depth"] = opts.Depth,
            ["spawn"] = opts.Spawn,
            ["isChild"] = opts.IsChild,
            ["childPids"] = childPids.ToArray(),
            ["os"] = RuntimeInformation.OSDescription,
        };

        var line = JsonSerializer.Serialize(record) + "\n";
        var bytes = Utf8NoBom.GetBytes(line);

        // 多进程并发追加：FileMode.Append + FileShare.ReadWrite，单次写入后立即关闭（短行 < 4KB，追加写原子）。
        for (var attempt = 0; attempt < 20; attempt++)
        {
            try
            {
                using var fs = new FileStream(full, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                fs.Write(bytes, 0, bytes.Length);
                fs.Flush();
                return;
            }
            catch (IOException)
            {
                Thread.Sleep(25);
            }
        }
    }


    private static string Hex(string s)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        var sb = new StringBuilder(bytes.Length * 2);
        foreach (var b in bytes)
        {
            sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
        }

        return sb.ToString();
    }

    private static void WriteRawStdout(ReadOnlySpan<byte> bytes) => WriteRaw(Console.OpenStandardOutput(), bytes);

    private static void WriteRawStderr(ReadOnlySpan<byte> bytes) => WriteRaw(Console.OpenStandardError(), bytes);

    private static void WriteRaw(Stream stream, ReadOnlySpan<byte> bytes)
    {
        try
        {
            stream.Write(bytes);
            stream.Flush();
        }
        catch (IOException)
        {
            // 读端已关闭（例如 stop 正在拆管道）：静默退出，不把它变成崩溃噪音。
        }
    }

    private sealed class Options
    {
        public bool IsChild { get; private set; }
        public string? Marker { get; private set; }
        public string? PidFile { get; private set; }
        public string? CwdFile { get; private set; }
        public string? StderrText { get; private set; }
        public int ExitCode { get; private set; }
        public int SleepMs { get; private set; }
        public int Spawn { get; private set; }
        public int Depth { get; private set; } = 1;
        public int Bytes { get; private set; }
        public bool Repeat { get; private set; }
        public bool BadUtf8 { get; private set; }
        public int Level { get; private set; }

        /// <summary>可选的「层级 sidecar」路径：每层各追加一行 JSON（裸 PIDfile 格式保持不变）。</summary>
        public string? LevelFile { get; private set; }

        /// <summary>派生方显式传入的自报父 PID（用于与内核真值交叉校验）。</summary>
        public int ParentPidArg { get; private set; } = -1;

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (var i = 0; i < args.Length; i++)
            {
                var a = args[i];
                switch (a)
                {
                    case "--child":
                        o.IsChild = true;
                        break;
                    case "--marker":
                        o.Marker = Next(args, ref i, a);
                        break;
                    case "--pidfile":
                        o.PidFile = Next(args, ref i, a);
                        break;
                    case "--cwd-file":
                        o.CwdFile = Next(args, ref i, a);
                        break;
                    case "--stderr":
                        o.StderrText = Next(args, ref i, a);
                        break;
                    case "--exit":
                        o.ExitCode = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--sleep":
                        o.SleepMs = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--spawn":
                        o.Spawn = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--depth":
                        o.Depth = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--bytes":
                        o.Bytes = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--repeat":
                        o.Repeat = true;
                        break;
                    case "--level":
                        o.Level = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--level-file":
                        o.LevelFile = Next(args, ref i, a);
                        break;
                    case "--parent-pid":
                        o.ParentPidArg = int.Parse(Next(args, ref i, a), CultureInfo.InvariantCulture);
                        break;
                    case "--bad-utf8":
                        o.BadUtf8 = true;
                        break;
                    default:
                        // 未知参数一律原样保留在 argv 里（证明桥没有吞参数），不报错。
                        break;
                }
            }

            return o;
        }

        private static string Next(string[] args, ref int i, string flag)
        {
            if (i + 1 >= args.Length)
            {
                throw new ArgumentException($"{flag} 缺少取值");
            }

            return args[++i];
        }
    }
}
