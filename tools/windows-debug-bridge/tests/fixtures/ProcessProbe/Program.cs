// Windows 调试桥 · 进程存活探针（fixture，零 NuGet、纯 BCL）
//
// 用途：给 WSL 侧 tests/windows-smoke.py 提供「Windows 进程树是否真的全没了」的**只读**判据。
//   WSL 侧永远不能用 os.kill(pid,0) 检查 Windows PID —— 那是 Linux PID 命名空间里的数字，
//   拿它去查 Linux 进程是彻头彻尾的假证据。判据只能在 Windows 侧取，故有本探针。
//
// 硬性语义（与 windows-smoke.py 的验收口径一字对应）：
//   * 只查询，绝不终止：全部写操作只有 GetProcessById / HasExited / ProcessName / StartTime；
//     本文件不含 Kill / CloseMainWindow / taskkill / TerminateProcess 的任何调用。
//   * 「错误/权限异常绝不当作 gone」：只有 *可证明不存在* 才记 gone
//       - GetProcessById 抛 ArgumentException（该 PID 没有运行中的进程）→ gone
//       - GetProcessById 成功但 HasExited 为真 → gone
//     其余一切异常（Win32Exception 权限不足、InvalidOperationException 竞态、IO 等）→ query_error，
//     query_error **不算 gone**，allGone 必为 false。
//   * PID 复用可见：alive 时附带进程名与启动时间，便于人判断该 PID 是否已被无关进程复用。
//
// 用法：
//   ProcessProbe --pidfile <绝对路径> [--min-pids N] [--wait-until-gone-seconds S]
//                [--interval-ms M] [--expect-name NAME] [--pidfile-wait-seconds S]
//   ProcessProbe --selftest          # 纯逻辑自检（不需要 pidfile，Linux 也能跑）
//
// stdout 恒为一行，前缀化便于从桥的 stdout.log（混有构建输出）里精确定位：
//   WDB_PROBE {json}          正常快照
//   WDB_PROBE_SELFTEST {json} --selftest 结果
// 退出码：0 = 全部 gone 且 pid 数达标；1 = 未达标（有 alive/query_error/数量不足）；2 = 用法或读取错误。

using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace ProcessProbe;

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
            return EmitError("badargs", ex.Message);
        }

        if (opts.SelfTest)
        {
            return SelfTest();
        }

        if (string.IsNullOrWhiteSpace(opts.PidFile))
        {
            return EmitError("badargs", "缺少 --pidfile <绝对路径>。");
        }

        var notes = new List<string>();
        var pids = new List<int>();
        var firstCount = -1;
        var waitForFile = TimeSpan.FromSeconds(Math.Max(0, opts.PidFileWaitSeconds));
        var fileDeadline = DateTime.UtcNow + waitForFile;

        // 首次读取允许一个短重试窗口：pidfile 由被取消的进程树写出，可能刚落地。
        while (true)
        {
            var read = TryReadPids(opts.PidFile!, notes);
            if (read.Error is not null)
            {
                return EmitError("pidfile", read.Error);
            }

            pids = read.Pids;
            if (pids.Count > 0 || DateTime.UtcNow >= fileDeadline)
            {
                break;
            }

            Thread.Sleep(100);
        }

        firstCount = pids.Count;
        if (IsEmptyPidfileFatal(pids.Count))
        {
            // 空 pidfile 绝不等于「全没了」——那是假绿。明确报错。
            return EmitError("pidfile", $"pidfile 里没有任何 PID：{opts.PidFile}"
                + $"（等待 {waitForFile.TotalSeconds:F0}s 后仍为空）。空文件不构成「全部 gone」的证据。");
        }

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(Math.Max(0, opts.WaitUntilGoneSeconds));
        var interval = TimeSpan.FromMilliseconds(Math.Clamp(opts.IntervalMs, 50, 5000));
        var attempts = 0;
        var startedUtc = DateTime.UtcNow;
        List<PidResult> results;

        while (true)
        {
            attempts++;

            // 每轮重读 pidfile：三层树可能在首读之后又落了新 PID（并集，绝不丢）。
            var again = TryReadPids(opts.PidFile!, notes);
            if (again.Error is null)
            {
                foreach (var pid in again.Pids)
                {
                    if (!pids.Contains(pid))
                    {
                        pids.Add(pid);
                    }
                }
            }

            results = pids.Select(pid => ProbeOne(pid, opts.ExpectName)).ToList();
            var allGoneNow = results.All(r => r.State == StateGone);
            if (allGoneNow || DateTime.UtcNow >= deadline)
            {
                break;
            }

            Thread.Sleep(interval);
        }

        var gone = results.Count(r => r.State == StateGone);
        var alive = results.Count(r => r.State == "alive");
        var queryErrors = results.Count(r => r.State == "query_error");
        var countOk = results.Count >= opts.MinPids;
        var allGone = gone == results.Count;

        var payload = new Dictionary<string, object?>
        {
            ["tool"] = "process-probe",
            ["schema"] = 1,
            ["ok"] = allGone && countOk,
            ["status"] = allGone && countOk ? "all-gone" : "not-all-gone",
            ["pidfile"] = opts.PidFile,
            ["expectedName"] = opts.ExpectName,
            ["minPids"] = opts.MinPids,
            ["pidCount"] = results.Count,
            ["pidCountFirstRead"] = firstCount,
            ["countOk"] = countOk,
            ["pids"] = results.Select(r => r.Pid).ToArray(),
            ["gone"] = gone,
            ["alive"] = alive,
            ["queryErrors"] = queryErrors,
            ["allGone"] = allGone,
            ["results"] = results.Select(r => r.ToJson()).ToArray(),
            ["attempts"] = attempts,
            ["waitedSeconds"] = Math.Round((DateTime.UtcNow - startedUtc).TotalSeconds, 3),
            ["deadlineSeconds"] = opts.WaitUntilGoneSeconds,
            ["notes"] = notes.ToArray(),
            ["operations"] = new[] { "Process.GetProcessById", "Process.HasExited", "Process.ProcessName", "Process.StartTime" },
            ["kills"] = false,
            ["os"] = RuntimeInformation.OSDescription,
            ["framework"] = RuntimeInformation.FrameworkDescription,
            ["cwd"] = Environment.CurrentDirectory,
            ["pid"] = Environment.ProcessId,
        };

        WriteLine("WDB_PROBE " + JsonSerializer.Serialize(payload));
        return allGone && countOk ? 0 : 1;
    }

    // ---------------------------------------------------------------- 单 PID 判定

    private const string StateGone = "gone";
    private const string StateAlive = "alive";
    private const string StateQueryError = "query_error";

    /// <summary>只读判定：gone 必须有证据，异常一律 query_error（绝不当作 gone）。</summary>
    private static PidResult ProbeOne(int pid, string? expectName)
    {
        try
        {
            using var proc = Process.GetProcessById(pid);

            // GetProcessById 成功但进程已退出（Unix 上尤其常见）→ 有证据的 gone。
            if (proc.HasExited)
            {
                return new PidResult(pid, StateGone, null, null, null, null);
            }

            return new PidResult(pid, StateAlive, SafeName(proc), SafeStartTime(proc),
                expectName is null ? null : SafeNameMatches(proc, expectName), null);
        }
        catch (ArgumentException)
        {
            // 该 PID 没有运行中的进程（Windows 上即 “Process with an Id of N is not running.”）。
            return new PidResult(pid, StateGone, null, null, null, null);
        }
        catch (Exception ex)
        {
            // 权限不足 / 竞态 / 其它：不可证明不存在 ⇒ query_error，不得计入 gone。
            return new PidResult(pid, StateQueryError, null, null, null,
                ex.GetType().Name + ": " + ex.Message);
        }
    }

    private static string? SafeName(Process p)
    {
        try
        {
            return p.ProcessName;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static string? SafeStartTime(Process p)
    {
        try
        {
            return p.StartTime.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture);
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static bool? SafeNameMatches(Process p, string expectName)
    {
        var name = SafeName(p);
        return name is null ? null : string.Equals(name, expectName, StringComparison.OrdinalIgnoreCase);
    }

    private sealed record PidResult(int Pid, string State, string? Name, string? StartTimeUtc, bool? NameMatches, string? Error)
    {
        public Dictionary<string, object?> ToJson()
        {
            var d = new Dictionary<string, object?> { ["pid"] = Pid, ["state"] = State };
            if (Name is not null) d["name"] = Name;
            if (StartTimeUtc is not null) d["startTimeUtc"] = StartTimeUtc;
            if (NameMatches is not null) d["nameMatches"] = NameMatches;
            if (Error is not null) d["error"] = Error;
            return d;
        }
    }

    // ---------------------------------------------------------------- pidfile 解析

    /// <summary>
    /// 空 pidfile 绝不等于「全部 gone」—— 这是本探针唯一可能产生假绿的入口，故单独抽成纯函数并被自检覆盖。
    /// </summary>
    internal static bool IsEmptyPidfileFatal(int pidCount) => pidCount == 0;

    private static (List<int> Pids, string? Error) TryReadPids(string path, List<string> notes)
    {
        string text;
        try
        {
            if (!File.Exists(path))
            {
                return (new List<int>(), null);
            }

            text = File.ReadAllText(path, Utf8NoBom);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return (new List<int>(), $"读取 pidfile 失败：{ex.GetType().Name}: {ex.Message}（{path}）");
        }

        var pids = new List<int>();
        var junk = 0;
        foreach (var token in text.Split(['\r', '\n', ' ', '\t'], StringSplitOptions.RemoveEmptyEntries))
        {
            if (int.TryParse(token.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var pid) && pid > 0)
            {
                if (!pids.Contains(pid))
                {
                    pids.Add(pid);
                }
            }
            else
            {
                junk++;
            }
        }

        if (junk > 0)
        {
            notes.Add($"pidfile 里有 {junk} 个非 PID 片段被忽略。");
        }

        return (pids, null);
    }

    // ---------------------------------------------------------------- 自检（Linux 可跑）

    /// <summary>
    /// 纯逻辑自检：证明「哪些异常算 gone、哪些必须算 query_error」。
    /// 这是 windows-smoke.py --static 在 WSL 上唯一能真正验证「权限/错误不当作 gone」的方式。
    /// </summary>
    private static int SelfTest()
    {
        var cases = new List<Dictionary<string, object?>>();
        var ok = true;

        void Case(string name, string expect, Func<string> actual)
        {
            string got;
            try
            {
                got = actual();
            }
            catch (Exception ex)
            {
                got = "THREW:" + ex.GetType().Name;
            }

            var pass = got == expect;
            ok &= pass;
            cases.Add(new Dictionary<string, object?> { ["name"] = name, ["expect"] = expect, ["actual"] = got, ["pass"] = pass });
        }

        // 1) ArgumentException（Windows 上“该 PID 未运行”）→ gone
        Case("ArgumentException->gone", StateGone, () => ClassifyException(new ArgumentException("Process with an Id of 12345 is not running.")));
        // 2) 权限不足（Win32Exception）→ query_error
        Case("Win32Exception->query_error", StateQueryError,
            () => ClassifyException(new System.ComponentModel.Win32Exception(5, "Access is denied.")));
        // 3) InvalidOperationException（竞态）→ query_error
        Case("InvalidOperationException->query_error", StateQueryError, () => ClassifyException(new InvalidOperationException("no process")));
        // 4) IOException → query_error
        Case("IOException->query_error", StateQueryError, () => ClassifyException(new IOException("io")));
        // 5) 真实进程：本进程自己必须 alive
        Case("self->alive", StateAlive, () =>
        {
            using var self = Process.GetProcessById(Environment.ProcessId);
            return self.HasExited ? StateGone : StateAlive;
        });
        // 6) 真实不存在的 PID：必须 gone（Linux 与 Windows 都由 GetProcessById/ HasExited 给出可证证据）
        Case("bogus-pid->gone", StateGone, () =>
        {
            var probe = ProbeOne(FindBogusPid(), null);
            return probe.State;
        });
        // 7) 空 pidfile 不得被判成 all gone（判据是纯函数，Main 与自检共用同一条规则）
        Case("empty-pidfile->fatal", "fatal", () => IsEmptyPidfileFatal(0) ? "fatal" : "not-fatal");
        Case("nonempty-pidfile->ok", "not-fatal", () => IsEmptyPidfileFatal(3) ? "fatal" : "not-fatal");
        Case("empty-file->no-pids", "0/", () =>
        {
            var tmp = Path.Combine(Path.GetTempPath(), "pp-selftest-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
            File.WriteAllText(tmp, "\n  \n");
            try
            {
                var (pids, err) = TryReadPids(tmp, new List<string>());
                return $"{pids.Count}/{err}";
            }
            finally
            {
                try { File.Delete(tmp); } catch (IOException) { }
            }
        });
        // 8) pidfile 解析：容忍垃圾、去重、忽略非正数
        Case("parse->dedup", "3,7", () =>
        {
            var tmp = Path.Combine(Path.GetTempPath(), "pp-selftest-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
            File.WriteAllText(tmp, "3\n7 3\nabc 0 -5\n7\n");
            try
            {
                var (pids, err) = TryReadPids(tmp, new List<string>());
                return err is not null ? "error" : string.Join(",", pids);
            }
            finally
            {
                try { File.Delete(tmp); } catch (IOException) { }
            }
        });

        var payload = new Dictionary<string, object?>
        {
            ["tool"] = "process-probe-selftest",
            ["ok"] = ok,
            ["cases"] = cases,
            ["os"] = RuntimeInformation.OSDescription,
            ["framework"] = RuntimeInformation.FrameworkDescription,
            ["version"] = Assembly.GetExecutingAssembly().GetName().Version?.ToString() ?? "?",
        };
        WriteLine("WDB_PROBE_SELFTEST " + JsonSerializer.Serialize(payload));
        return ok ? 0 : 1;
    }

    /// <summary>把异常映射成状态 —— 与 ProbeOne 的 catch 链一一对应（唯一事实来源）。</summary>
    private static string ClassifyException(Exception ex) => ex switch
    {
        ArgumentException => StateGone,
        _ => StateQueryError,
    };

    /// <summary>找一个确定不存在的 PID：从一个已退出的子进程拿号，再 +2^22 偏移（远超常用 pid_max）。</summary>
    private static int FindBogusPid()
    {
        var candidate = 1 << 22;
        while (candidate < (1 << 30))
        {
            try
            {
                using var p = Process.GetProcessById(candidate);
                candidate += 1 << 20;
            }
            catch (ArgumentException)
            {
                return candidate;
            }
            catch (Exception)
            {
                candidate += 1 << 20;
            }
        }

        return candidate;
    }

    // ---------------------------------------------------------------- 输出与参数

    private static int EmitError(string kind, string message)
    {
        var payload = new Dictionary<string, object?>
        {
            ["tool"] = "process-probe",
            ["schema"] = 1,
            ["ok"] = false,
            ["status"] = "error",
            ["errorKind"] = kind,
            ["error"] = message,
            ["os"] = RuntimeInformation.OSDescription,
            ["pid"] = Environment.ProcessId,
        };
        WriteLine("WDB_PROBE " + JsonSerializer.Serialize(payload));
        return 2;
    }

    private static void WriteLine(string line)
    {
        try
        {
            using var stdout = Console.OpenStandardOutput();
            var bytes = Utf8NoBom.GetBytes(line + "\n");
            stdout.Write(bytes, 0, bytes.Length);
            stdout.Flush();
        }
        catch (IOException)
        {
            // 读端已关闭：静默。
        }
    }

    private sealed class Options
    {
        public string? PidFile { get; private set; }
        public int MinPids { get; private set; } = 1;
        public double WaitUntilGoneSeconds { get; private set; }
        public int IntervalMs { get; private set; } = 250;
        public string? ExpectName { get; private set; }
        public double PidFileWaitSeconds { get; private set; } = 5;
        public bool SelfTest { get; private set; }

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--pidfile":
                        o.PidFile = Next(args, ref i);
                        break;
                    case "--min-pids":
                        o.MinPids = ParseInt(Next(args, ref i), "--min-pids");
                        break;
                    case "--wait-until-gone-seconds":
                        o.WaitUntilGoneSeconds = ParseDouble(Next(args, ref i), "--wait-until-gone-seconds");
                        break;
                    case "--interval-ms":
                        o.IntervalMs = ParseInt(Next(args, ref i), "--interval-ms");
                        break;
                    case "--expect-name":
                        o.ExpectName = Next(args, ref i);
                        break;
                    case "--pidfile-wait-seconds":
                        o.PidFileWaitSeconds = ParseDouble(Next(args, ref i), "--pidfile-wait-seconds");
                        break;
                    case "--selftest":
                        o.SelfTest = true;
                        break;
                    default:
                        throw new ArgumentException("未知参数：" + args[i]);
                }
            }

            if (o.MinPids < 0)
            {
                throw new ArgumentException("--min-pids 不能为负。");
            }

            if (o.WaitUntilGoneSeconds < 0)
            {
                throw new ArgumentException("--wait-until-gone-seconds 不能为负。");
            }

            return o;
        }

        private static string Next(string[] args, ref int i)
        {
            if (i + 1 >= args.Length)
            {
                throw new ArgumentException($"{args[i]} 缺少取值");
            }

            return args[++i];
        }

        private static int ParseInt(string text, string flag) =>
            int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v)
                ? v
                : throw new ArgumentException($"{flag} 需要整数，收到：{text}");

        private static double ParseDouble(string text, string flag) =>
            double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v)
                ? v
                : throw new ArgumentException($"{flag} 需要数值，收到：{text}");
    }
}
