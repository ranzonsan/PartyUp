// Windows 调试桥 · 日志增量夹具（fixture，零 NuGet、纯 BCL）
//
// 用途：给 tests/windows-smoke.py 的 day2 用例提供**可复算**的增量日志源。
//   WSL 侧用同一条公式独立算出期望字节，再与 `wdb logs --follow` 的原始输出逐字节比对，
//   因此本文件里**不许出现**时间戳、随机数、PID、环境相关文本 —— 任何不确定性都会让断言失去意义。
//
// 确定性公式（N = 行数，B = 每行填充字节数）：
//   stdout 第 i 行（i 从 0 计）= "OUT{i:0000}|中文行|" + 'x' * B + "\n"
//   stderr 第 i 行            = "ERR{i:0000}|错误中文行|" + 'y' * B + "\n"
//   全部 UTF-8 无 BOM；每行写后立即 Flush（真增量，便于 --follow 观察中间态）。
//
// 用法：
//   LogSpinner --stdout-lines N --stderr-lines M [--interval-ms I] [--chunk-bytes B]
//              [--exit C] [--marker <绝对路径>] [--cwd-file <绝对路径>]
//   默认 --stdout-lines 3 --stderr-lines 3 --interval-ms 0 --chunk-bytes 0 --exit 0

using System.Globalization;
using System.Text;

namespace LogSpinner;

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
            WriteRaw(Console.OpenStandardError(), Utf8NoBom.GetBytes($"WDB_SPINNER_BADARGS {ex.Message}\n"));
            return 64;
        }

        // 启动哨兵（可选）：进程真实存在的最早证据。
        if (opts.Marker is { Length: > 0 } marker)
        {
            WriteFileWithDirs(marker, $"{Environment.ProcessId}\n");
        }

        if (opts.CwdFile is { Length: > 0 } cwdFile)
        {
            WriteFileWithDirs(cwdFile, Environment.CurrentDirectory);
        }

        var stdout = Console.OpenStandardOutput();
        var stderr = Console.OpenStandardError();
        var lines = Math.Max(opts.StdoutLines, opts.StderrLines);

        for (var i = 0; i < lines; i++)
        {
            if (i < opts.StdoutLines)
            {
                var line = $"OUT{i.ToString("0000", CultureInfo.InvariantCulture)}|中文行|"
                           + new string('x', opts.ChunkBytes) + "\n";
                WriteRaw(stdout, Utf8NoBom.GetBytes(line));
            }

            if (i < opts.StderrLines)
            {
                var line = $"ERR{i.ToString("0000", CultureInfo.InvariantCulture)}|错误中文行|"
                           + new string('y', opts.ChunkBytes) + "\n";
                WriteRaw(stderr, Utf8NoBom.GetBytes(line));
            }

            if (opts.IntervalMs > 0)
            {
                Thread.Sleep(opts.IntervalMs);
            }
        }

        return opts.ExitCode;
    }

    private static void WriteFileWithDirs(string path, string content)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        File.WriteAllText(full, content, Utf8NoBom);
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
            // 读端已关闭：静默，不把收尾阶段的管道拆除变成崩溃噪音。
        }
    }

    private sealed class Options
    {
        public int StdoutLines { get; private set; } = 3;
        public int StderrLines { get; private set; } = 3;
        public int IntervalMs { get; private set; }
        public int ChunkBytes { get; private set; }
        public int ExitCode { get; private set; }
        public string? Marker { get; private set; }
        public string? CwdFile { get; private set; }

        public static Options Parse(string[] args)
        {
            var o = new Options();
            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--stdout-lines":
                        o.StdoutLines = Int(Next(args, ref i), "--stdout-lines");
                        break;
                    case "--stderr-lines":
                        o.StderrLines = Int(Next(args, ref i), "--stderr-lines");
                        break;
                    case "--interval-ms":
                        o.IntervalMs = Int(Next(args, ref i), "--interval-ms");
                        break;
                    case "--chunk-bytes":
                        o.ChunkBytes = Int(Next(args, ref i), "--chunk-bytes");
                        break;
                    case "--exit":
                        o.ExitCode = Int(Next(args, ref i), "--exit");
                        break;
                    case "--marker":
                        o.Marker = Next(args, ref i);
                        break;
                    case "--cwd-file":
                        o.CwdFile = Next(args, ref i);
                        break;
                    default:
                        // 未知参数原样忽略（不报错，避免把桥的行为差异误判成夹具失败）。
                        break;
                }
            }

            if (o.StdoutLines < 0 || o.StderrLines < 0 || o.IntervalMs < 0 || o.ChunkBytes < 0)
            {
                throw new ArgumentException("行数 / 间隔 / 填充字节不能为负。");
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

        private static int Int(string text, string flag) =>
            int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v)
                ? v
                : throw new ArgumentException($"{flag} 需要整数，收到：{text}");
    }
}
