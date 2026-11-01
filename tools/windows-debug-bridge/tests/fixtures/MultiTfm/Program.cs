// Windows 调试桥 · 多目标框架夹具（fixture，零 NuGet、纯 BCL）
//
// 关键点：运行的**究竟是哪个 TFM** 由编译期符号决定，不由调用方声称 ——
//   net10.0-windows 定义 NET10_0_WINDOWS，net10.0 不定义；因此输出里的 tfm 字段是构建事实。
// 输出单行 JSON：WDB_MTFM {...}，含 os / cwd / pid / tfm / framework / argv / argvHex。
// 仅供 tests/windows-smoke.py 的 day2 多 TFM 用例使用；未知参数原样保留在 argv 里不报错。

using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace MultiTfm;

internal static class Program
{
    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: false);

#if NET10_0_WINDOWS
    private const string Tfm = "net10.0-windows";
    private const string Platform = "windows";
#else
    private const string Tfm = "net10.0";
    private const string Platform = "any";
#endif

    private static int Main(string[] args)
    {
        var marker = ArgValue(args, "--marker");
        if (marker is { Length: > 0 })
        {
            var full = Path.GetFullPath(marker);
            var dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            File.AppendAllText(full, $"{Environment.ProcessId}\n", Utf8NoBom);
        }

        var exitCode = 0;
        var exitText = ArgValue(args, "--exit");
        if (exitText is not null && int.TryParse(exitText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
        {
            exitCode = parsed;
        }

        var payload = new Dictionary<string, object?>
        {
            ["argv"] = args,
            ["argvHex"] = args.Select(Hex).ToArray(),
            ["argc"] = args.Length,
            ["cwd"] = Environment.CurrentDirectory,
            ["pid"] = Environment.ProcessId,
            ["tfm"] = Tfm,
            ["platform"] = Platform,
            ["framework"] = RuntimeInformation.FrameworkDescription,
            ["os"] = RuntimeInformation.OSDescription,
            ["runtime"] = Environment.Version.ToString(),
            ["compiledFor"] = CompiledFor(),
            // 下面这组是**权威**证据：不依赖任何 #if 猜测，直接说明「实际跑起来的是哪个 TFM 的产物」。
            // baseDirectory 末段即被启动程序所在的目标框架目录（多 TFM 构建下 = 真正运行的 TFM）。
            ["baseDirectory"] = AppContext.BaseDirectory,
            ["processPath"] = Environment.ProcessPath,
            ["tfmAttribute"] = typeof(Program).Assembly
                .GetCustomAttribute<System.Runtime.Versioning.TargetFrameworkAttribute>()?.FrameworkName,
            // TargetPlatformAttribute 的成员名随 SDK 版本变化，故用反射按属性名取，避免编译期绑死。
            ["targetPlatformAttribute"] = TargetPlatformOf(typeof(Program).Assembly),
            ["definedSymbols"] = DefinedSymbols(),
        };

        WriteRaw(Console.OpenStandardOutput(), Utf8NoBom.GetBytes("WDB_MTFM " + JsonSerializer.Serialize(payload) + "\n"));
        return exitCode;
    }

    private static string CompiledFor() =>
#if NET10_0_WINDOWS
        "NET10_0_WINDOWS";
#else
        "NET10_0";
#endif

    /// <summary>报告候选编译期符号里哪些真的被定义了（避免用「猜符号」当证据）。</summary>
    private static string DefinedSymbols()
    {
        var parts = new List<string>();
#if NET10_0
        parts.Add("NET10_0");
#endif
#if NET10_0_WINDOWS
        parts.Add("NET10_0_WINDOWS");
#endif
#if NET10_0_WINDOWS7_0
        parts.Add("NET10_0_WINDOWS7_0");
#endif
#if WINDOWS
        parts.Add("WINDOWS");
#endif
#if WINDOWS7_0
        parts.Add("WINDOWS7_0");
#endif
        return string.Join(",", parts);
    }

    /// <summary>反射读取 TargetPlatformAttribute（成员名随 SDK 变化，故不编译期绑死）。</summary>
    private static string? TargetPlatformOf(Assembly assembly)
    {
        foreach (var attr in assembly.GetCustomAttributes(inherit: false))
        {
            var type = attr.GetType();
            if (type.Name != "TargetPlatformAttribute")
            {
                continue;
            }

            var prop = type.GetProperty("TargetPlatform") ?? type.GetProperty("PlatformName");
            return prop?.GetValue(attr)?.ToString();
        }

        return null;
    }

    private static string? ArgValue(string[] args, string flag)
    {
        for (var i = 0; i + 1 < args.Length; i++)
        {
            if (args[i] == flag)
            {
                return args[i + 1];
            }
        }

        return null;
    }

    private static string Hex(string s)
    {
        var bytes = Utf8NoBom.GetBytes(s);
        var sb = new StringBuilder(bytes.Length * 2);
        foreach (var b in bytes)
        {
            sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
        }

        return sb.ToString();
    }

    private static void WriteRaw(Stream stream, ReadOnlySpan<byte> bytes)
    {
        try
        {
            stream.Write(bytes);
            stream.Flush();
        }
        catch (IOException)
        {
            // 读端已关闭：静默。
        }
    }
}
