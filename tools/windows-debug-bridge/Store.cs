// Windows 调试桥 · 落盘层（CLI 与 Worker 共用）
//
// 所有写盘都走「同目录临时文件 + File.Move(overwrite)」原子替换；
// 所有读盘都对共享冲突/半写重试，因为队列通常位于 9p（WSL 文件系统）或 UNC 上。
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace GalleriesManager.Wdb;

/// <summary>一次日志读取的结果。<see cref="NextOffset"/> 可回传给下一次 --offset。</summary>
/// <param name="Offset">本次实际开始读取的字节偏移。</param>
/// <param name="NextOffset">下次应继续读取的字节偏移（末尾不完整的多字节序列不会被消费）。</param>
/// <param name="Length">文件当前总长度。</param>
/// <param name="Lines">读到的行（不含换行符）。</param>
/// <param name="Truncated">文件比请求的 offset 短（被截断/重建），本次已从 0 重读。</param>
public readonly record struct LogSlice(long Offset, long NextOffset, long Length, string[] Lines, bool Truncated)
{
    /// <summary>
    /// 本次读到的**原始文本片段**：offset 模式保留「尚无换行符的尾部片段」，逐字写回不会插入多余换行；
    /// tail 模式为按行拼接（每行带换行符）。用 init 属性以保持既有位置构造调用兼容。
    /// </summary>
    public string RawText { get; init; } = string.Empty;
}

public static class Store
{
    /// <summary>单次读取的上限，避免 CLI 在超大日志上爆内存。</summary>
    public const int MaxReadBytes = 8 * 1024 * 1024;

    private const int ReadRetryCount = 3;
    private const int ReadRetryDelayMs = 60;

    public static bool Exists(string path) => !string.IsNullOrEmpty(path) && File.Exists(path);

    public static bool DirectoryExists(string path) => !string.IsNullOrEmpty(path) && Directory.Exists(path);

    public static void EnsureDir(string dir)
    {
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
    }

    public static bool TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) { File.Delete(path); return true; }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return false;
    }

    /// <summary>原子写文本（UTF-8 无 BOM）。临时文件与目标同目录，最后一步是 File.Move 覆盖。</summary>
    public static void WriteTextAtomic(string path, string text)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        var tmp = full + ".tmp-" + NewToken(6);
        try
        {
            File.WriteAllText(tmp, text, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(tmp, full, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp)) TryDelete(tmp);
        }
    }

    public static void WriteBytesAtomic(string path, byte[] bytes)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        var tmp = full + ".tmp-" + NewToken(6);
        try
        {
            File.WriteAllBytes(tmp, bytes);
            File.Move(tmp, full, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp)) TryDelete(tmp);
        }
    }

    public static void WriteJsonAtomic<T>(string path, T value) =>
        WriteTextAtomic(path, BridgeJson.Serialize(value) + "\n");

    /// <summary>写「空标记文件」（cancel / shutdown）：存在即信号，内容为空。</summary>
    public static void WriteMarker(string path) => WriteBytesAtomic(path, Array.Empty<byte>());

    public static T? ReadJson<T>(string path) where T : class
    {
        return TryReadJson<T>(path, out var value, out _) ? value : null;
    }

    public static bool TryReadJson<T>(string path, out T? value, out string? error) where T : class
    {
        value = null;
        error = null;

        for (var attempt = 0; attempt < ReadRetryCount; attempt++)
        {
            try
            {
                if (!File.Exists(path))
                {
                    error = "not-found";
                    return false;
                }

                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
                var text = reader.ReadToEnd();
                if (string.IsNullOrWhiteSpace(text))
                {
                    error = "empty";
                    Thread.Sleep(ReadRetryDelayMs);
                    continue;
                }

                var parsed = BridgeJson.Deserialize<T>(text);
                if (parsed is null)
                {
                    error = "null";
                    Thread.Sleep(ReadRetryDelayMs);
                    continue;
                }

                value = parsed;
                error = null;
                return true;
            }
            catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException or UnauthorizedAccessException)
            {
                error = ex.GetType().Name + ": " + ex.Message;
                Thread.Sleep(ReadRetryDelayMs);
            }
        }

        return false;
    }

    public static string? ReadText(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
            return reader.ReadToEnd();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>追加文本。全程以 FileShare.ReadWrite 打开，保证读侧（CLI/Worker）可并行读。</summary>
    public static void AppendText(string path, string text)
    {
        var full = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        using var stream = new FileStream(full, FileMode.Append, FileAccess.Write,
            FileShare.ReadWrite | FileShare.Delete);
        var bytes = new UTF8Encoding(false).GetBytes(text);
        stream.Write(bytes, 0, bytes.Length);
        stream.Flush();
    }

    public static void AppendLine(string path, string line) => AppendText(path, line + Environment.NewLine);

    /// <summary>按字节 offset 或尾部行数读取日志。tailLines &gt; 0 时按行尾读；否则从 offset 读到文件末尾（单次上限 8MiB）。</summary>
    public static LogSlice ReadLog(string path, long offset, int tailLines)
    {
        var safeOffset = offset < 0 ? 0 : offset;

        if (!File.Exists(path))
            return new LogSlice(safeOffset, safeOffset, 0, Array.Empty<string>(), false);

        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var length = stream.Length;

            if (tailLines > 0)
            {
                var window = Math.Min(length, 128L * 1024);
                var read = ReadTailWindow(stream, length, window);
                while (read.Lines.Length < tailLines && window < length && window < MaxReadBytes)
                {
                    var next = Math.Min(window * 4, Math.Min(length, MaxReadBytes));
                    if (next <= window) break;
                    window = next;
                    read = ReadTailWindow(stream, length, window);
                }

                // 只保留最后 N 条，且按**原始文本**切：保留原有 CR/LF，保留末尾尚未换行的片段，
                // 不补换行；NextOffset 只消费完整 UTF-8 前缀（尾部不完整序列留给下一轮）。
                var selectedText = TakeLastLines(read.RawText, tailLines);
                return new LogSlice(safeOffset, read.NextOffset, length, SplitLines(selectedText), false)
                {
                    RawText = selectedText,
                };
            }

            if (safeOffset > length)
            {
                // 文件被截断/重建：从 0 重读，并告知调用方
                return ReadFrom(stream, 0, length, truncated: true);
            }

            return ReadFrom(stream, safeOffset, length, truncated: false);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return new LogSlice(safeOffset, safeOffset, 0, Array.Empty<string>(), false);
        }
    }

    /// <summary>
    /// 终态收尾读：从 offset 一直读到文件末尾，**不再扣留**尾部不完整序列（按替换字符明确吐出一次）。
    /// 只在任务已进入终态、不会再写入时调用，用于避免尾部残缺字节被静默丢弃。
    /// </summary>
    public static LogSlice ReadLogRemainder(string path, long offset)
    {
        var safeOffset = offset < 0 ? 0 : offset;

        if (!File.Exists(path))
            return new LogSlice(safeOffset, safeOffset, 0, Array.Empty<string>(), false) { RawText = string.Empty };

        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var length = stream.Length;
            if (safeOffset >= length)
                return new LogSlice(safeOffset, length, length, Array.Empty<string>(), false) { RawText = string.Empty };

            var cap = (int)Math.Min(MaxReadBytes, length - safeOffset);
            stream.Seek(safeOffset, SeekOrigin.Begin);

            var buffer = new byte[cap];
            var read = 0;
            while (read < cap)
            {
                var n = stream.Read(buffer, read, cap - read);
                if (n <= 0) break;
                read += n;
            }

            var text = Encoding.UTF8.GetString(buffer, 0, read);
            return new LogSlice(safeOffset, safeOffset + read, length, SplitLines(text), false) { RawText = text };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return new LogSlice(safeOffset, safeOffset, 0, Array.Empty<string>(), false) { RawText = string.Empty };
        }
    }

    private static LogSlice ReadFrom(FileStream stream, long offset, long length, bool truncated)
    {
        var remaining = length - offset;
        var cap = (int)Math.Min(MaxReadBytes, remaining < 0 ? 0 : remaining);
        if (cap <= 0)
            return new LogSlice(offset, offset, length, Array.Empty<string>(), truncated) { RawText = string.Empty };

        stream.Seek(offset, SeekOrigin.Begin);
        var buffer = new byte[cap];
        var read = 0;
        while (read < cap)
        {
            var n = stream.Read(buffer, read, cap - read);
            if (n <= 0) break;
            read += n;
        }

        // 文件可能仍在增长，或 8MiB 上限正好切在多字节字符中间：
        // 末尾不完整的多字节序列留给下一轮（不推进 offset），避免解码出替换字符丢字。
        var complete = TrimIncompleteUtf8Tail(buffer, read);
        var text = complete <= 0 ? string.Empty : Encoding.UTF8.GetString(buffer, 0, complete);

        return new LogSlice(offset, offset + complete, length, SplitLines(text), truncated) { RawText = text };
    }

    /// <summary>
    /// 返回 buffer 中「完整 UTF-8 字符」的字节数：末尾若是被截断的多字节序列，则扣留其全部字节。
    /// 真正的非法字节（既非合法前导也非续字节，或续字节前找不到前导）一律按完整消费，
    /// 否则调用方会永远停在同一个 offset 上（死循环）。
    /// </summary>
    public static int TrimIncompleteUtf8Tail(byte[] buffer, int count) => TrimIncompleteUtf8Tail(buffer, 0, count);

    /// <summary>同上，但从 buffer 的 start 开始、长度为 count；返回的是**相对 start 的完整字节数**。</summary>
    public static int TrimIncompleteUtf8Tail(byte[] buffer, int start, int count)
    {
        if (buffer is null || count <= 0) return 0;
        if (start < 0) start = 0;
        if (start + count > buffer.Length) count = buffer.Length - start;
        if (count <= 0) return 0;

        var last = buffer[start + count - 1];
        if (last < 0x80) return count;                                  // ASCII：完整
        if (last is >= 0xC0 and <= 0xF7) return count - 1;              // 只有前导字节 → 序列不完整
        if (last is < 0x80 or > 0xBF) return count;                     // 非法单字节：按完整消费，保证前进

        // last 是续字节：向前最多 3 步找前导字节
        for (var back = 2; back <= 4; back++)
        {
            if (count - back < 0) return count;                         // 找不到前导 → 非法，按完整消费
            var lead = buffer[start + count - back];
            if (lead is >= 0x80 and <= 0xBF) continue;                  // 仍是续字节，继续向前

            var expected = lead switch
            {
                >= 0xC0 and <= 0xDF => 2,
                >= 0xE0 and <= 0xEF => 3,
                >= 0xF0 and <= 0xF7 => 4,
                _ => 0,                                                 // 非法前导 → 按完整消费
            };

            if (expected == 0) return count;
            return back < expected ? count - back : count;
        }

        return count;
    }

    /// <summary>tail 窗口读取结果：原始文本（保留 CR/LF 与末尾未换行片段）+ 下次应继续的字节偏移。</summary>
    private readonly record struct TailRead(string RawText, string[] Lines, long NextOffset);

    private static TailRead ReadTailWindow(FileStream stream, long length, long window)
    {
        if (window <= 0 || length <= 0) return new TailRead(string.Empty, Array.Empty<string>(), length);

        var start = length - window;
        var cap = (int)Math.Min(window, length);
        stream.Seek(start, SeekOrigin.Begin);

        var buffer = new byte[cap];
        var read = 0;
        while (read < cap)
        {
            var n = stream.Read(buffer, read, cap - read);
            if (n <= 0) break;
            read += n;
        }

        // 窗口没有覆盖到文件开头时，首行是半行：按字节丢到第一个换行为止（连同换行符）
        var offset = 0;
        if (start > 0)
        {
            while (offset < read && buffer[offset] != (byte)'\n') offset++;
            if (offset < read) offset++;
        }

        // 窗口尾部若切在多字节字符中间，扣留该序列：NextOffset 只消费完整 UTF-8 前缀，
        // 这样 --follow 的下一轮会从被扣留的字节继续读，字符不会被吞掉。
        var available = read - offset;
        var complete = TrimIncompleteUtf8Tail(buffer, offset, available);
        var text = complete <= 0 ? string.Empty : Encoding.UTF8.GetString(buffer, offset, complete);

        return new TailRead(text, SplitLines(text), start + offset + complete);
    }

    /// <summary>按原始文本保留最后 maxLines 行：不补换行、不改 CR/LF；行数不足时原样返回。</summary>
    private static string TakeLastLines(string text, int maxLines)
    {
        if (maxLines <= 0 || text.Length == 0) return string.Empty;

        var starts = new List<int> { 0 };
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] == '\n') starts.Add(i + 1);
        }

        var count = starts.Count;
        if (starts[^1] == text.Length) count--;   // 末尾换行不产生新行（与 SplitLines 口径一致）
        if (count <= maxLines) return text;

        return text[starts[count - maxLines]..];
    }

    private static string[] SplitLines(string text)
    {
        if (text.Length == 0) return Array.Empty<string>();

        var parts = text.Split('\n');
        var count = parts.Length;
        if (count > 0 && parts[count - 1].Length == 0) count--; // 末尾换行不产生空行

        var result = new string[count];
        for (var i = 0; i < count; i++)
        {
            var line = parts[i];
            if (line.Length > 0 && line[^1] == '\r') line = line[..^1];
            result[i] = line;
        }

        return result;
    }

    /// <summary>生成任务 id：yyyyMMddTHHmmssfffZ-xxxxxx（UTC，文件名安全且按名排序≈按时间排序）。</summary>
    public static string NewId(DateTimeOffset now)
    {
        var stamp = now.ToUniversalTime().ToString("yyyyMMdd'T'HHmmssfff'Z'", CultureInfo.InvariantCulture);
        return stamp + "-" + NewToken(6);
    }

    /// <summary>小写十六进制随机串（用于临时文件名/workerId）。</summary>
    public static string NewToken(int hexChars = 8)
    {
        if (hexChars <= 0) hexChars = 8;
        var bytes = RandomNumberGenerator.GetBytes((hexChars + 1) / 2);
        var hex = Convert.ToHexString(bytes).ToLowerInvariant();
        return hex.Length <= hexChars ? hex : hex[..hexChars];
    }

    public static string Sha256Hex(string text)
    {
        var bytes = SHA256.HashData(new UTF8Encoding(false).GetBytes(text));
        return Convert.ToHexString(bytes).ToLowerInvariant();
    }
}
