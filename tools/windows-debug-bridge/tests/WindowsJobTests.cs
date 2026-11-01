using System.Diagnostics;
using System.Reflection;
using System.Text;
using GalleriesManager.Wdb;

namespace Wdb.Tests;

/// <summary>
/// WindowsJobRunner 的**纯托管**回归（Linux/WSL 可全跑，零 Win32 调用）。
///
/// 覆盖：CRT 命令行转义与 argv 拼装、CREATE_UNICODE_ENVIRONMENT 环境块、Start 的参数校验分支、
/// 公开接口面冻结、以及与 Worker 的接缝注册（JobRunnerGateway.WindowsProcessFactory）。
///
/// 明确不覆盖（= Windows 未实测，断言它们等于假绿）：
///   CreateProcessW / STARTUPINFOEX 句柄白名单 / Job Object(KILL_ON_JOB_CLOSE) / AssignProcessToJobObject /
///   ResumeThread / 管道读写 / 整树终止 / Process.GetProcessById 元数据包装。
///
/// runner 的纯函数是 internal：测试用反射调用，**不为了测试而扩大正式 API**。
/// </summary>
public static class WindowsJobTests
{
    private const string RunnerTypeName = "GalleriesManager.Wdb.WindowsJobRunner";
    private const string GatewayTypeName = "GalleriesManager.Wdb.JobRunnerGateway";
    private const string LaunchTypeName = "GalleriesManager.Wdb.JobLaunch";
    private const string JobProcessInterfaceName = "GalleriesManager.Wdb.IJobProcess";
    private const string ProcessJobTypeName = "GalleriesManager.Wdb.WindowsProcessJob";

    // ── 反射桥（internal 纯函数） ────────────────────────────────────────────

    private static object? InvokeRunnerPure(string name, params object?[] args)
    {
        MethodInfo? method = typeof(WindowsJobRunner).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static);
        if (method is null)
        {
            throw new WdbAssertException(
                $"{RunnerTypeName}.{name} 未找到：runner 的 internal 纯函数被改名/改可见性时本测试必须翻红"
                + "（测试走反射，不要求把这些函数提为 public）。");
        }

        try
        {
            return method.Invoke(null, args);
        }
        catch (TargetInvocationException ex) when (ex.InnerException is not null)
        {
            throw ex.InnerException;
        }
    }

    // 注意：逐个包成 object?[] 显式数组，避免 params 把 string[]/KV[] 展平成多个实参。
    private static string BuildArguments(params string[] argumentList)
        => (string)InvokeRunnerPure("BuildArguments", new object?[] { argumentList })!;

    private static string BuildCommandLine(string fileName, string arguments)
        => (string)InvokeRunnerPure("BuildCommandLine", new object?[] { fileName, arguments })!;

    private static string? BuildEnvironmentBlock(params KeyValuePair<string, string?>[] environment)
        => (string?)InvokeRunnerPure("BuildEnvironmentBlock", new object?[] { environment });

    private static Type BridgeType(string name)
        => typeof(WindowsJobRunner).Assembly.GetType(name)
           ?? throw new WdbAssertException($"桥程序集里找不到类型 {name}：与 Worker 的接缝被改名时本测试必须翻红。");

    // ── 1. CRT 转义：精确期望值 ─────────────────────────────────────────────

    [WdbFact("runner.crt-quoting-exact")]
    public static void CrtQuotingExact()
    {
        // 空参数必须变成 ""，否则会被 argv 直接吞掉。
        WdbAssert.Equal("\"\"", BuildArguments(""));

        // 不含分隔符/引号：原样输出，不加引号。
        WdbAssert.Equal("-c", BuildArguments("-c"));
        WdbAssert.Equal("中文", BuildArguments("中文"));
        WdbAssert.Equal(@"\\server\share\file.cs", BuildArguments(@"\\server\share\file.cs"));
        WdbAssert.Equal(@"C:\dir\", BuildArguments(@"C:\dir\"));

        // 含空白/引号：整串加引号。
        WdbAssert.Equal("\"a b\"", BuildArguments("a b"));
        WdbAssert.Equal("\"a\tb\"", BuildArguments("a\tb"));
        WdbAssert.Equal("\"a\nb\"", BuildArguments("a\nb"));
        WdbAssert.Equal("\"中 文\"", BuildArguments("中 文"));

        // 串中引号：1 个反斜杠 + 引号（2N+1 规则，N=0）。
        WdbAssert.Equal("\"a\\\"b\"", BuildArguments("a\"b"));

        // 收尾反斜杠在引号内必须翻倍（2N 规则），否则会吃掉收尾引号。
        WdbAssert.Equal("\"C:\\dir with space\\\\\"", BuildArguments(@"C:\dir with space\"));

        // 2 个反斜杠 + 引号（N=1 的 2N+1 形态）：翻倍成 5 个反斜杠 + 引号。
        WdbAssert.Equal("\"a\\\\\\\\\\\"b\"", BuildArguments("a\\\\\"b"));

        // 引号判定必须与 .NET 运行时 System.PasteArguments 一致：**任意** Unicode 空白码点都要加引号，
        // 而不是只有空格/制表/LF/VT（曾窄 22 个码点：CR/FF/NEL/NBSP/U+2000-200A/U+2028/U+2029/U+202F/U+205F/U+3000）。
        WdbAssert.Equal("\"a\rb\"", BuildArguments("a\rb"));
        WdbAssert.Equal("\"a\fb\"", BuildArguments("a\fb"));
        WdbAssert.Equal("\"a\u0085b\"", BuildArguments("a\u0085b"));
        WdbAssert.Equal("\"a\u00a0b\"", BuildArguments("a\u00a0b"));
        WdbAssert.Equal("\"a\u2003b\"", BuildArguments("a\u2003b"));
        WdbAssert.Equal("\"a\u2028b\"", BuildArguments("a\u2028b"));
        WdbAssert.Equal("\"a\u2029b\"", BuildArguments("a\u2029b"));
        WdbAssert.Equal("\"a\u3000b\"", BuildArguments("a\u3000b"));

        // 多参数连接：逐项引用，中间单空格。
        WdbAssert.Equal("-c \"a b\" c", BuildArguments("-c", "a b", "c"));
        WdbAssert.Equal("\"\" x", BuildArguments("", "x"));
    }

    // ── 2. argv[0] 引号规则 ─────────────────────────────────────────────────

    [WdbFact("runner.commandline-exe-quoting")]
    public static void CommandLineExeQuoting()
    {
        WdbAssert.Equal("\"C:\\dotnet\\dotnet.exe\" run", BuildCommandLine(@"C:\dotnet\dotnet.exe", "run"));
        WdbAssert.Equal("\"C:\\Program Files\\dotnet\\dotnet.exe\" --info", BuildCommandLine(@"C:\Program Files\dotnet\dotnet.exe", "--info"));
        WdbAssert.Equal("\"dotnet.exe\"", BuildCommandLine("dotnet.exe", ""));
        WdbAssert.Equal("\"dotnet.exe\" build", BuildCommandLine("  dotnet.exe  ", "build"));

        // 已带引号的 FileName 不得重复加引号（否则 CreateProcessW 会当成空参数）。
        WdbAssert.Equal("\"C:\\x\\a.exe\" --info", BuildCommandLine("\"C:\\x\\a.exe\"", "--info"));
    }

    // ── 3. 与独立解析器往返：固定边界集 ─────────────────────────────────────

    [WdbFact("runner.crt-roundtrip-fixed-set")]
    public static void CrtRoundtripFixedSet()
    {
        // 32 个边界参数 + 9 个针对性语料，逐个往返：写(转义) → 独立读(CRT 解析) 必须还原原文。
        string[] edge =
        [
            "", " ", "  ", "\t", "a", "a b", "a\tb", "a\nb", "a\vb", "a\"b", "\"", "\"\"", "\\", "\\\\",
            "C:\\dir\\", @"C:\dir with space\", "a\\\\\"b", @"\\server\share", "中 文", "中文\"引号\"",
            "--project", "src/GalleriesManager.Desktop", "-p:UseSharedCompilation=false", "a\"\"b", "\\\"", "\"a\"",
            @"C:\p\", @"C:\p q\", "\\\\\"", "\\\"", "a b c", "  lead and trail  ",
            "a\rb", "a\fb", "a\u00a0b", "a\u2003b", "a\u2028b", "a\u3000b", "\r\f\u00a0",
        ];

        string[] corpus = ["", " ", @"C:\p\", @"C:\p q\", "\"", "\\", "\\\\", "\\\\\"", "\\\""];

        foreach (string argument in edge.Concat(corpus))
        {
            string commandLine = BuildCommandLine(@"C:\dotnet\dotnet.exe", BuildArguments(argument));
            var parsed = CrtParse(commandLine);
            WdbAssert.Equal(2, parsed.Count, $"往返失败：argv 数不对。argument={WdbAssert.Show(argument)} cmd={WdbAssert.Show(commandLine)}");
            WdbAssert.Equal(@"C:\dotnet\dotnet.exe", parsed[0], $"argv[0] 还原失败 cmd={WdbAssert.Show(commandLine)}");
            WdbAssert.Equal(argument, parsed[1], $"往返失败 cmd={WdbAssert.Show(commandLine)}");
        }
    }

    // ── 4. 与独立解析器往返：确定性 fuzz ────────────────────────────────────

    [WdbFact("runner.crt-roundtrip-fuzz")]
    public static void CrtRoundtripFuzz()
    {
        // 固定种子：可复现。字母表覆盖 空白(空格/制表/CR/LF/VT/FF/NBSP/U+2028) + 引号 + 反斜杠 + 中文
        // —— 即 CRT 转义的全部决策点（F1 修正后含非 ASCII 空白码点）。
        const int iterations = 4000;
        const string alphabet = "ab \\\t\n\r\f\"\u00a0\u2028中";
        var rng = new Random(20261011);

        for (var i = 0; i < iterations; i++)
        {
            var sb = new StringBuilder();
            int length = rng.Next(0, 9);
            for (var j = 0; j < length; j++)
            {
                sb.Append(alphabet[rng.Next(alphabet.Length)]);
            }

            string argument = sb.ToString();

            // 同一参数放两次：顺带证明「相邻参数不互相污染」。
            string commandLine = BuildCommandLine("prog.exe", BuildArguments(argument, argument));
            var parsed = CrtParse(commandLine);

            WdbAssert.Equal(3, parsed.Count, $"fuzz 第 {i} 次 argv 数不对。argument={WdbAssert.Show(argument)} cmd={WdbAssert.Show(commandLine)}");
            WdbAssert.Equal("prog.exe", parsed[0], $"fuzz 第 {i} 次 argv[0] 不对 cmd={WdbAssert.Show(commandLine)}");
            WdbAssert.Equal(argument, parsed[1], $"fuzz 第 {i} 次 argv[1] 往返失败。cmd={WdbAssert.Show(commandLine)}");
            WdbAssert.Equal(argument, parsed[2], $"fuzz 第 {i} 次 argv[2] 往返失败。cmd={WdbAssert.Show(commandLine)}");
        }
    }

    // ── 4b. 与运行时 System.PasteArguments 差分 ─────────────────────────────

    [WdbFact("runner.crt-matches-runtime-pastearguments")]
    public static void CrtMatchesRuntimePasteArguments()
    {
        // 差分基准 = 本进程运行时的 System.PasteArguments（internal），即 .NET 自己把
        // ProcessStartInfo.ArgumentList 拼成命令行时用的那段实现。规则一旦分叉（如"引号判定窄了 22 个
        // 空白码点"这类一致性偏差），这里必须立刻翻红 —— 靠人肉比对是发现不了的。
        var type = typeof(object).Assembly.GetType("System.PasteArguments");
        if (type is null)
        {
            throw new WdbAssertException("运行时里找不到 System.PasteArguments：差分基准失效，需人工复核转义规则。");
        }

        var paste = type.GetMethod("Paste", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
        if (paste is null)
        {
            throw new WdbAssertException("System.PasteArguments.Paste 不存在：差分基准失效，需人工复核转义规则。");
        }

        // false = 不按 argv[0] 规则（argv[0] 由 BuildCommandLine 单独处理）。
        string RuntimePaste(string[] arguments)
            => (string)paste.Invoke(null, new object?[] { arguments, false })!;

        string[][] cases =
        [
            ["ab"], ["a b"], ["a\tb"], ["a\nb"], ["a\vb"], ["a\rb"], ["a\fb"],
            ["a\u0085b"], ["a\u00a0b"], ["a\u2003b"], ["a\u2028b"], ["a\u2029b"], ["a\u3000b"],
            [""], ["  "], ["a\"b"], ["a\\\"b"], [@"C:\p\"], [@"C:\p q\"], ["中文 参数"],
            ["-c", "a b", ""], ["a b", "c"], ["", "", ""],
        ];

        foreach (string[] arguments in cases)
        {
            WdbAssert.Equal(
                RuntimePaste(arguments),
                BuildArguments(arguments),
                $"与运行时 PasteArguments 不一致 args=[{string.Join(" | ", arguments.Select(WdbAssert.Show))}]");
        }

        // 4000 次随机差分：与 fuzz 同种子、同字母表（含 CR/FF/NBSP/U+2028 等非 ASCII 空白码点）。
        const string alphabet = "ab \\\t\n\r\f\"\u00a0\u2028中";
        var rng = new Random(20261011);
        for (var i = 0; i < 4000; i++)
        {
            var sb = new StringBuilder();
            int length = rng.Next(0, 9);
            for (var j = 0; j < length; j++)
            {
                sb.Append(alphabet[rng.Next(alphabet.Length)]);
            }

            string argument = sb.ToString();
            string[] arguments = [argument, argument];
            WdbAssert.Equal(
                RuntimePaste(arguments),
                BuildArguments(arguments),
                $"fuzz 第 {i} 次与运行时 PasteArguments 不一致 argument={WdbAssert.Show(argument)}");
        }
    }

    // ── 5. 环境块 ───────────────────────────────────────────────────────────

    [WdbFact("runner.environment-block")]
    public static void EnvironmentBlock()
    {
        // 条目按名排序 + 每条 NUL 结尾 + 块尾再补一个 NUL（双 NUL 终止）。
        WdbAssert.Equal(
            "A=1\0B=2\0\0",
            BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("B", "2"), KeyValuePair.Create<string, string?>("A", "1")));
        WdbAssert.Equal("K=\0\0", BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("K", null)));
        WdbAssert.Equal("PATH=C:\\中文 目录\0\0", BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("PATH", @"C:\中文 目录")));

        // 驱动器当前目录这类隐藏变量在块里的形态是 "=C:=C:\dir"；.NET 字典里该 Value 带不带前导 '=' 两种都兜住。
        WdbAssert.Equal("=C:=C:\\dir\0\0", BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("=C:", "=C:\\dir")));
        WdbAssert.Equal("=C:=C:\\dir\0\0", BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("=C:", "C:\\dir")));
        WdbAssert.Equal(
            "=C:=C:\\dir\0A=1\0\0",
            BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("A", "1"), KeyValuePair.Create<string, string?>("=C:", "C:\\dir")));

        // 空集合/空键 → null 表示「继承父环境」，绝不构造出一个空环境块把子进程变成裸进程。
        WdbAssert.Null(BuildEnvironmentBlock());
        WdbAssert.Null(BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("", "v")));

        // 值里出现 NUL 会截断环境块：必须整条丢弃，不得写出去。
        WdbAssert.Null(BuildEnvironmentBlock(KeyValuePair.Create<string, string?>("A", "x\0y")));

        string? two = BuildEnvironmentBlock(
            KeyValuePair.Create<string, string?>("A", "1"),
            KeyValuePair.Create<string, string?>("B", "2"));
        WdbAssert.NotNull(two);
        WdbAssert.StartsWith("A=1\0B=2\0", two);
        WdbAssert.True(two!.EndsWith("\0\0", StringComparison.Ordinal), $"环境块未以双 NUL 结束：{WdbAssert.Show(two)}");
        WdbAssert.Equal(4, two.Split('\0').Length, $"环境块条目数不对：{WdbAssert.Show(two)}");
    }

    // ── 6. Start 参数校验（原生调用之前的纯托管分支） ────────────────────────

    [WdbFact("runner.start-validation")]
    public static void StartValidation()
    {
        // 这几个分支与平台无关：必须在触碰任何 Win32 之前抛出。
        WdbAssert.Throws<ArgumentNullException>(() => WindowsJobRunner.Start(null!), "info 为 null");

        WdbAssert.Throws<InvalidOperationException>(
            () => WindowsJobRunner.Start(new ProcessStartInfo("dotnet") { UseShellExecute = true }),
            "UseShellExecute=true 必须拒绝（本 runner 不经 shell）");

        WdbAssert.Throws<InvalidOperationException>(
            () => WindowsJobRunner.Start(new ProcessStartInfo("dotnet") { UseShellExecute = false, Verb = "runas" }),
            "Verb 需要 UseShellExecute=true，必须拒绝");

        WdbAssert.Throws<InvalidOperationException>(
            () => WindowsJobRunner.Start(new ProcessStartInfo("  ") { UseShellExecute = false }),
            "FileName 为空必须拒绝");

        // 凭据分支：非 Windows 上 Password/Domain 的 getter 自己就会抛 PlatformNotSupportedException，
        // 因此平台守卫必须排在凭据校验之前（否则 Linux 上会抛成与业务无关的密码异常）。
        if (OperatingSystem.IsWindows())
        {
            WdbAssert.Throws<NotSupportedException>(
                () => WindowsJobRunner.Start(new ProcessStartInfo("dotnet") { UseShellExecute = false, UserName = "u" }),
                "Windows 上凭据必须显式拒绝");
        }
        else
        {
            var ex = WdbAssert.Throws<PlatformNotSupportedException>(
                () => WindowsJobRunner.Start(new ProcessStartInfo("dotnet") { UseShellExecute = false, UserName = "u" }),
                "非 Windows 上平台守卫优先");
            WdbAssert.Contains("WindowsJobRunner 只能在 Windows 上运行", ex.Message);

            // 合法参数在非 Windows 上也必须走同一条守卫，且绝不在守卫之前读 Password（否则消息会变成密码本体的异常）。
            var valid = WdbAssert.Throws<PlatformNotSupportedException>(
                () => WindowsJobRunner.Start(new ProcessStartInfo("dotnet") { UseShellExecute = false }),
                "非 Windows 上合法参数也必须拒绝启动");
            WdbAssert.StartsWith("WindowsJobRunner 只能在 Windows 上运行", valid.Message);
        }
    }

    // ── 7. 公开接口面冻结 ───────────────────────────────────────────────────

    [WdbFact("runner.public-surface-frozen")]
    public static void PublicSurfaceFrozen()
    {
        var type = typeof(WindowsJobRunner);

        WdbAssert.True(type.IsSealed, "runner 必须是 sealed");
        WdbAssert.True(typeof(IDisposable).IsAssignableFrom(type), "runner 必须实现 IDisposable");

        string[] expected =
        [
            "GalleriesManager.Wdb.WindowsJobRunner Start(System.Diagnostics.ProcessStartInfo)",
            "System.Boolean get_HasExited()",
            "System.Diagnostics.Process get_Process()",
            "System.Int32 get_ExitCode()",
            "System.Int32 get_Id()",
            "System.IO.StreamReader get_StandardError()",
            "System.IO.StreamReader get_StandardOutput()",
            "System.IO.StreamWriter get_StandardInput()",
            "System.Threading.Tasks.Task`1[System.Int32] WaitForExitAsync(System.Threading.CancellationToken)",
            "System.Void Dispose()",
            "System.Void Kill()",
        ];

        var actual = type.GetMembers(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
            .Where(m => m.MemberType is MemberTypes.Method or MemberTypes.Property)
            .Select(m => m switch
            {
                PropertyInfo p => $"{p.PropertyType} get_{p.Name}()",
                MethodInfo mi => mi.IsSpecialName
                    ? null
                    : $"{mi.ReturnType} {mi.Name}({string.Join(", ", mi.GetParameters().Select(pa => pa.ParameterType.ToString()))})",
                _ => null,
            })
            .Where(s => s is not null)
            .Select(s => s!)
            .OrderBy(s => s, StringComparer.Ordinal)
            .ToArray();

        // 公开面就是契约：Worker/CLI 依赖它，任何增删都必须是有意为之。
        WdbAssert.SequenceEqual(expected.OrderBy(s => s, StringComparer.Ordinal), actual);

        var start = type.GetMethod("Start");
        WdbAssert.NotNull(start, "Start 必须存在");
        WdbAssert.True(start!.IsStatic, "Start 必须是 static");
        WdbAssert.Equal(type, start.ReturnType, "Start 必须返回 WindowsJobRunner");
        WdbAssert.Equal(typeof(ProcessStartInfo), start.GetParameters().Single().ParameterType, "Start 的唯一参数必须是 ProcessStartInfo");

        var wait = type.GetMethod("WaitForExitAsync");
        WdbAssert.NotNull(wait, "WaitForExitAsync 必须存在");
        WdbAssert.True(wait!.GetParameters()[0].HasDefaultValue, "WaitForExitAsync 的 CancellationToken 必须有默认值");

        // 子进程不是 System.Diagnostics.Process 启的：其重定向流拿不到数据，只能由 runner 自己暴露读者。
        WdbAssert.Equal(typeof(StreamReader), type.GetProperty("StandardOutput")!.PropertyType);
        WdbAssert.Equal(typeof(StreamReader), type.GetProperty("StandardError")!.PropertyType);
        WdbAssert.Equal(typeof(StreamWriter), type.GetProperty("StandardInput")!.PropertyType);
    }

    // ── 8. 与 Worker 的接缝注册 ─────────────────────────────────────────────

    [WdbFact("runner.worker-seam-registered")]
    public static void WorkerSeamRegistered()
    {
        var gateway = BridgeType(GatewayTypeName);
        var launch = BridgeType(LaunchTypeName);
        var jobProcess = BridgeType(JobProcessInterfaceName);
        var processJob = BridgeType(ProcessJobTypeName);

        var factoryProperty = gateway.GetProperty("WindowsProcessFactory", BindingFlags.Public | BindingFlags.Static);
        WdbAssert.NotNull(factoryProperty, "JobRunnerGateway.WindowsProcessFactory 必须存在（Worker 的唯一接缝）");
        WdbAssert.Equal(
            typeof(Func<,>).MakeGenericType(launch, jobProcess),
            factoryProperty!.PropertyType,
            "工厂签名必须是 Func<JobLaunch, IJobProcess>");

        // [ModuleInitializer] 必须已把工厂装上：为 null 时 Worker 在 Windows 上会以
        // ERR_JOB_RUNNER_UNAVAILABLE 拒绝启动（这是有意的硬失败，不是降级）。
        object? factory = factoryProperty.GetValue(null);
        WdbAssert.NotNull(factory, "模块初始化未注册工厂：Windows 上任务将全部启动失败");
        WdbAssert.True(jobProcess.IsAssignableFrom(processJob), "WindowsProcessJob 必须实现 IJobProcess");

        if (OperatingSystem.IsWindows())
        {
            // Windows 上真的会建进程：此处只做注册态断言，不实际启动（实机项属 Windows 实测范围）。
            return;
        }

        // 非 Windows：工厂必须直达 WindowsJobRunner.Start 并抛平台异常 —— 证明接缝已接线（而不是被空实现吞掉）。
        object instance = launch.GetConstructors().Single()
            .Invoke(["/bin/echo", new[] { "hi" }, "/tmp"]);
        var ex = WdbAssert.Throws<TargetInvocationException>(
            () => ((Delegate)factory!).DynamicInvoke(instance),
            "工厂不得在非 Windows 上返回进程");
        WdbAssert.NotNull(ex.InnerException, "DynamicInvoke 必须包裹内层异常");
        WdbAssert.Equal(typeof(PlatformNotSupportedException), ex.InnerException!.GetType(), "工厂必须直达 runner 的平台守卫");
        WdbAssert.StartsWith("WindowsJobRunner 只能在 Windows 上运行", ex.InnerException.Message);
    }

    // ── 独立实现的 Windows CRT 解析器 ───────────────────────────────────────
    //
    // 依据 MSDN "Parsing C++ Command-Line Arguments" 独立实现：转义器负责「写」，这里负责「读」，
    // 二者只有真正互补才能往返一致 —— 用同一份代码自证是没有意义的。

    private static List<string> CrtParse(string commandLine)
    {
        var result = new List<string>();
        var i = 0;
        var n = commandLine.Length;

        // argv[0]：以引号开头 → 到下一个引号为止；否则到第一个空白为止。
        var first = new StringBuilder();
        if (n > 0 && commandLine[0] == '"')
        {
            i = 1;
            while (i < n && commandLine[i] != '"')
            {
                first.Append(commandLine[i++]);
            }

            if (i < n)
            {
                i++;
            }
        }
        else
        {
            while (i < n && commandLine[i] != ' ' && commandLine[i] != '\t')
            {
                first.Append(commandLine[i++]);
            }
        }

        if (first.Length > 0)
        {
            result.Add(first.ToString());
        }

        while (i < n)
        {
            while (i < n && (commandLine[i] == ' ' || commandLine[i] == '\t'))
            {
                i++;
            }

            if (i >= n)
            {
                break;
            }

            var argument = new StringBuilder();
            var inQuotes = false;
            while (i < n)
            {
                char c = commandLine[i];
                if (!inQuotes && (c == ' ' || c == '\t'))
                {
                    break;
                }

                if (c == '\\')
                {
                    var backslashes = 0;
                    while (i < n && commandLine[i] == '\\')
                    {
                        backslashes++;
                        i++;
                    }

                    if (i < n && commandLine[i] == '"')
                    {
                        argument.Append('\\', backslashes / 2);
                        if (backslashes % 2 == 0)
                        {
                            inQuotes = !inQuotes;
                        }
                        else
                        {
                            argument.Append('"');
                        }

                        i++;
                    }
                    else
                    {
                        argument.Append('\\', backslashes);
                    }
                }
                else if (c == '"')
                {
                    inQuotes = !inQuotes;
                    i++;
                }
                else
                {
                    argument.Append(c);
                    i++;
                }
            }

            result.Add(argument.ToString());
        }

        return result;
    }
}
