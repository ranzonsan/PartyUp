using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace Wdb.Tests;

/// <summary>标记一个测试方法。方法必须是 public static、无参、返回 void 或 Task。</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class WdbFactAttribute : Attribute
{
    public WdbFactAttribute(string? id = null) => Id = id;

    /// <summary>稳定 id（默认取方法名），用于 --filter 与证据输出。</summary>
    public string? Id { get; }

    /// <summary>非空则跳过，值即跳过原因。</summary>
    public string? Skip { get; set; }
}

public sealed class WdbAssertException : Exception
{
    public WdbAssertException(string message) : base(message) { }
}

/// <summary>
/// 断言集。所有失败信息都带 expected/actual，并对字符串附 UTF-8 十六进制，
/// 避免"看起来一样"（不可见字符、NBSP、CRLF、路径分隔符）骗过比较。
/// </summary>
public static class WdbAssert
{
    private const int MaxText = 400;

    public static void True(bool condition, string? because = null)
    {
        if (!condition)
        {
            throw new WdbAssertException($"期望 true，实际 false。{Suffix(because)}");
        }
    }

    public static void False(bool condition, string? because = null)
    {
        if (condition)
        {
            throw new WdbAssertException($"期望 false，实际 true。{Suffix(because)}");
        }
    }

    public static void Equal<T>(T expected, T actual, string? because = null)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new WdbAssertException(
                $"值不相等。expected={Show(expected)} actual={Show(actual)}。{Suffix(because)}");
        }
    }

    public static void NotEqual<T>(T unexpected, T actual, string? because = null)
    {
        if (EqualityComparer<T>.Default.Equals(unexpected, actual))
        {
            throw new WdbAssertException(
                $"值不应相等，但相等。value={Show(actual)}。{Suffix(because)}");
        }
    }

    public static void SequenceEqual<T>(IEnumerable<T> expected, IEnumerable<T> actual, string? because = null)
    {
        var e = expected.ToArray();
        var a = actual.ToArray();
        if (e.Length != a.Length || !e.SequenceEqual(a))
        {
            throw new WdbAssertException(
                $"序列不相等。expected=[{string.Join(", ", e.Select(x => Show(x)))}] actual=[{string.Join(", ", a.Select(x => Show(x)))}]。{Suffix(because)}");
        }
    }

    public static void Contains(string expectedSubstring, string? actual, string? because = null)
    {
        if (actual is null || !actual.Contains(expectedSubstring, StringComparison.Ordinal))
        {
            throw new WdbAssertException(
                $"未找到子串。expectedSubstring={Show(expectedSubstring)} actual={Show(actual)}。{Suffix(because)}");
        }
    }

    public static void NotContains(string unexpectedSubstring, string? actual, string? because = null)
    {
        if (actual is not null && actual.Contains(unexpectedSubstring, StringComparison.Ordinal))
        {
            throw new WdbAssertException(
                $"不应出现的子串出现了。unexpectedSubstring={Show(unexpectedSubstring)} actual={Show(actual)}。{Suffix(because)}");
        }
    }

    public static void NotNull<T>(T? value, string? because = null) where T : class
    {
        if (value is null)
        {
            throw new WdbAssertException($"期望非 null，实际 null。{Suffix(because)}");
        }
    }

    public static void Null(object? value, string? because = null)
    {
        if (value is not null)
        {
            throw new WdbAssertException($"期望 null，实际 {Show(value)}。{Suffix(because)}");
        }
    }

    public static void StartsWith(string expectedPrefix, string? actual, string? because = null)
    {
        if (actual is null || !actual.StartsWith(expectedPrefix, StringComparison.Ordinal))
        {
            throw new WdbAssertException(
                $"前缀不符。expectedPrefix={Show(expectedPrefix)} actual={Show(actual)}。{Suffix(because)}");
        }
    }

    public static TException Throws<TException>(Action action, string? because = null)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException ex)
        {
            return ex;
        }
        catch (Exception ex)
        {
            throw new WdbAssertException(
                $"期望抛出 {typeof(TException).Name}，实际抛出 {ex.GetType().Name}: {ex.Message}。{Suffix(because)}");
        }

        throw new WdbAssertException($"期望抛出 {typeof(TException).Name}，实际没有异常。{Suffix(because)}");
    }

    /// <summary>把字符串渲染成 `<literal> (utf8hex=...)`，便于发现不可见差异。</summary>
    public static string Show(object? value)
    {
        switch (value)
        {
            case null:
                return "<null>";
            case string s:
                return $"\"{Truncate(s)}\" (utf8={Hex(s)})";
            default:
                return Truncate(Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture) ?? "<null>");
        }
    }

    public static string Hex(string s)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        var sb = new StringBuilder(bytes.Length * 2 + 8);
        sb.Append("0x");
        foreach (var b in bytes)
        {
            sb.Append(b.ToString("x2", System.Globalization.CultureInfo.InvariantCulture));
        }

        return sb.ToString();
    }

    private static string Truncate(string s) =>
        s.Length <= MaxText ? s : s[..MaxText] + $"...(len={s.Length})";

    private static string Suffix(string? because) => string.IsNullOrEmpty(because) ? string.Empty : $"原因：{because}";
}

/// <summary>测试结果记录。</summary>
public sealed record TestResult(string Id, string Status, string? Message, TimeSpan Duration);

/// <summary>反射式运行器：顺序执行，任何失败都用退出码表达。</summary>
public static class TestRunner
{
    public static int Run(string[] args)
    {
        var filter = ArgValue(args, "--filter");
        var listOnly = args.Contains("--list");
        var verbose = args.Contains("--verbose");

        var methods = Discover().ToList();

        if (listOnly)
        {
            foreach (var m in methods)
            {
                Console.WriteLine($"{IdOf(m)}  ({m.DeclaringType?.Name}.{m.Name})");
            }

            Console.WriteLine($"共 {methods.Count} 个测试。");
            return 0;
        }

        var selected = methods
            .Where(m => filter is null || IdOf(m).Contains(filter, StringComparison.OrdinalIgnoreCase))
            .ToList();

        Console.WriteLine($"wdb-tests · 发现 {methods.Count} 个，选中 {selected.Count} 个");
        Console.WriteLine($"临时根：{TestSpace.TmpRoot}（WDB_TEST_TMP 可覆盖；WDB_KEEP_TMP=1 保留现场）");
        Console.WriteLine(new string('-', 78));

        var results = new List<TestResult>();
        var sw = Stopwatch.StartNew();

        foreach (var m in selected)
        {
            var id = IdOf(m);
            var skip = m.GetCustomAttribute<WdbFactAttribute>()?.Skip;
            if (!string.IsNullOrEmpty(skip))
            {
                Console.WriteLine($"[SKIP] {id} — {skip}");
                results.Add(new TestResult(id, "skip", skip, TimeSpan.Zero));
                continue;
            }

            var started = Stopwatch.StartNew();
            try
            {
                var ret = m.Invoke(null, null);
                if (ret is Task task)
                {
                    task.GetAwaiter().GetResult();
                }

                started.Stop();
                results.Add(new TestResult(id, "pass", null, started.Elapsed));
                Console.WriteLine($"[PASS] {id} ({started.ElapsedMilliseconds} ms)");
            }
            catch (Exception ex)
            {
                started.Stop();
                var real = ex is TargetInvocationException { InnerException: { } inner } ? inner : ex;
                var msg = real is WdbAssertException
                    ? real.Message
                    : $"{real.GetType().Name}: {real.Message}\n{real.StackTrace}";
                results.Add(new TestResult(id, "fail", msg, started.Elapsed));
                Console.WriteLine($"[FAIL] {id} ({started.ElapsedMilliseconds} ms)");
                Console.WriteLine($"       {Indent(msg)}");
            }

            if (verbose)
            {
                Console.WriteLine($"       tmp-root={TestSpace.TmpRoot}");
            }
        }

        sw.Stop();

        var pass = results.Count(r => r.Status == "pass");
        var fail = results.Count(r => r.Status == "fail");
        var skipCount = results.Count(r => r.Status == "skip");

        Console.WriteLine(new string('-', 78));
        Console.WriteLine($"结果：pass={pass} fail={fail} skip={skipCount} 用时={sw.Elapsed.TotalSeconds:F2}s");

        if (fail > 0)
        {
            Console.WriteLine("失败明细：");
            foreach (var f in results.Where(r => r.Status == "fail"))
            {
                Console.WriteLine($"  * {f.Id}");
                Console.WriteLine($"    {Indent(f.Message ?? string.Empty)}");
            }
        }

        return fail > 0 ? 1 : 0;
    }

    /// <summary>发现所有带 [WdbFact] 的 public static 方法（按类型名+方法名稳定排序）。</summary>
    public static IEnumerable<MethodInfo> Discover()
    {
        var asm = Assembly.GetExecutingAssembly();
        return asm.GetTypes()
            .Where(t => t.IsClass && t.IsAbstract && t.IsSealed) // static class
            .SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.Static))
            .Where(m => m.GetCustomAttribute<WdbFactAttribute>() is not null)
            .Where(m => m.GetParameters().Length == 0)
            .OrderBy(m => m.DeclaringType?.Name, StringComparer.Ordinal)
            .ThenBy(m => m.Name, StringComparer.Ordinal);
    }

    public static string IdOf(MethodInfo m) =>
        m.GetCustomAttribute<WdbFactAttribute>()?.Id is { Length: > 0 } id ? id : m.Name;

    private static string? ArgValue(string[] args, string name)
    {
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == name && i + 1 < args.Length)
            {
                return args[i + 1];
            }
        }

        return null;
    }

    private static string Indent(string text) => text.Replace("\n", "\n       ");
}

/// <summary>
/// 每个测试一个隔离现场：root/{queue,runs,src,...} 全部落在 workspace 内（沙箱可写），
/// 默认在 Dispose 时递归删除；WDB_KEEP_TMP=1 时保留并打印路径，供人工核对与取证。
/// </summary>
public sealed class TestSpace : IDisposable
{
    private static readonly Lazy<string> TmpRootLazy = new(ResolveTmpRoot);

    private bool _disposed;

    private TestSpace(string root)
    {
        Root = root;
        Queue = Path.Combine(root, "queue");
        Runs = Path.Combine(root, "runs");
        Src = Path.Combine(root, "src");
        Directory.CreateDirectory(Root);
    }

    public string Root { get; }

    public string Queue { get; }

    public string Runs { get; }

    public string Src { get; }

    public static string TmpRoot => TmpRootLazy.Value;

    public static bool Keep => Environment.GetEnvironmentVariable("WDB_KEEP_TMP") is "1" or "true";

    public static TestSpace Create(string name)
    {
        var safe = new string(name.Select(c => char.IsLetterOrDigit(c) || c is '-' or '_' ? c : '_').ToArray());
        var stamp = DateTime.UtcNow.ToString("HHmmss", System.Globalization.CultureInfo.InvariantCulture);
        var guid = Guid.NewGuid().ToString("N")[..8];
        return new TestSpace(Path.Combine(TmpRoot, $"{safe}-{stamp}-{guid}"));
    }

    public string PathOf(params string[] parts) => Path.Combine(new[] { Root }.Concat(parts).ToArray());

    public string WriteFile(string relativePath, string content)
    {
        var full = PathOf(relativePath.Split('/'));
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        File.WriteAllText(full, content, new UTF8Encoding(false));
        return full;
    }

    public string WriteBytes(string relativePath, byte[] content)
    {
        var full = PathOf(relativePath.Split('/'));
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        File.WriteAllBytes(full, content);
        return full;
    }

    public string ReadFile(params string[] parts) => File.ReadAllText(PathOf(parts));

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        if (Keep)
        {
            Console.WriteLine($"       [keep] {Root}");
            return;
        }

        TryDelete(Root);
    }

    public static void TryDelete(string path)
    {
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    Directory.Delete(path, recursive: true);
                }
                else if (File.Exists(path))
                {
                    File.Delete(path);
                }

                return;
            }
            catch (IOException)
            {
                Thread.Sleep(100);
            }
            catch (UnauthorizedAccessException)
            {
                Thread.Sleep(100);
            }
        }
    }

    private static string ResolveTmpRoot()
    {
        var fromEnv = Environment.GetEnvironmentVariable("WDB_TEST_TMP");
        if (!string.IsNullOrWhiteSpace(fromEnv))
        {
            Directory.CreateDirectory(fromEnv);
            return Path.GetFullPath(fromEnv);
        }

        var repo = FindRepoRoot() ?? Directory.GetCurrentDirectory();
        var tmp = Path.Combine(repo, "tools", "windows-debug-bridge", "tests", ".tmp");
        Directory.CreateDirectory(tmp);
        return tmp;
    }

    /// <summary>从测试输出目录向上找仓库根（含 GalleriesManager.slnx 的目录）。</summary>
    public static string? FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "GalleriesManager.slnx")))
            {
                return dir.FullName;
            }

            dir = dir.Parent;
        }

        return null;
    }
}

/// <summary>子进程执行器：一律走 ArgumentList（不经 shell），stdin 关闭，显式超时。</summary>
public static class Proc
{
    public sealed record Result(int ExitCode, string Stdout, string Stderr, bool TimedOut, string FileName, IReadOnlyList<string> Args)
    {
        public string CommandLine => FileName + " " + string.Join(' ', Args.Select(a => a.Contains(' ') ? $"\"{a}\"" : a));

        public override string ToString() =>
            $"exit={ExitCode} timedOut={TimedOut}\ncmd={CommandLine}\n--- stdout ---\n{Stdout}\n--- stderr ---\n{Stderr}";
    }

    public static Result Run(string fileName, IEnumerable<string> args, string? workingDir = null, int timeoutMs = 120_000, IReadOnlyDictionary<string, string>? env = null)
    {
        var argList = args.ToList();
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            WorkingDirectory = workingDir ?? Directory.GetCurrentDirectory(),
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = false,
            CreateNoWindow = true,
        };

        foreach (var a in argList)
        {
            psi.ArgumentList.Add(a);
        }

        if (env is not null)
        {
            foreach (var (k, v) in env)
            {
                psi.Environment[k] = v;
            }
        }

        using var p = new Process { StartInfo = psi };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) { lock (stdout) { stdout.AppendLine(e.Data); } } };
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) { lock (stderr) { stderr.AppendLine(e.Data); } } };

        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();

        var timedOut = !p.WaitForExit(timeoutMs);
        if (timedOut)
        {
            try
            {
                p.Kill(entireProcessTree: true);
            }
            catch
            {
                // 已被回收
            }

            p.WaitForExit(5_000);
        }

        // WaitForExit(int) 不保证异步读事件已排空，必须再等一次无参重载。
        try
        {
            p.WaitForExit();
        }
        catch
        {
            // ignore
        }

        lock (stdout)
        {
            lock (stderr)
            {
                return new Result(p.HasExited ? p.ExitCode : -1, stdout.ToString(), stderr.ToString(), timedOut, fileName, argList);
            }
        }
    }
}
