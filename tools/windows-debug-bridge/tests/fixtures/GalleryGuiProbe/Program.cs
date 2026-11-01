// GalleryGuiProbe —— 「真实 Avalonia 画廊应用隔离 GUI 验收」的 Windows 侧只读探针。
//
// 纯 .NET 10 BCL、零 NuGet、零第三方库；只 P/Invoke user32.dll；**绝不写任何非 --out 文件**。
//
// 用法（由 Windows 调试桥以独立 job 在 Windows 上执行）：
//   GalleryGuiProbe --baseline --out <json> [--hash-limit-mb 8] [--emit-cap 5000]
//   GalleryGuiProbe --verify-shot --bmp <winpath> [--png <winpath>] [--expect-w N] [--expect-h N] --out <json>
//   GalleryGuiProbe --window-probe --pid <N> [--watch-seconds 20] [--interval-ms 250] --out <json>
//
// stdout 协议：始终输出一行 `WDB_GALLERY_PROBE {json}`（纯 ASCII）。
// 退出码：0 = 成功；1 = 运行失败；2 = 用法错误；3 = fail closed（权限/IO 读不到，上层必须视为不可信）。

using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace GalleryGuiProbe;

internal static class Program
{
    private const string Prefix = "WDB_GALLERY_PROBE ";

    private static int Main(string[] args)
    {
        var opts = ParseArgs(args);
        if (opts is null)
        {
            Console.Error.WriteLine("用法错误。见 GalleryGuiProbe/Program.cs 头部注释。");
            return 2;
        }

        try
        {
            var payload = opts.Mode switch
            {
                "baseline" => Baseline(opts),
                "verify-shot" => VerifyShot(opts),
                "window-probe" => WindowProbe(opts),
                _ => null,
            };

            if (payload is null)
            {
                Console.Error.WriteLine("未知模式：" + opts.Mode);
                return 2;
            }

            var json = JsonSerializer.Serialize(payload);
            Console.Out.WriteLine(Prefix + json);
            Console.Out.Flush();

            if (opts.Out is { Length: > 0 } outPath)
            {
                try
                {
                    var dir = Path.GetDirectoryName(outPath);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    File.WriteAllText(outPath, json);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("写 --out 失败：" + ex.GetType().Name + ": " + ex.Message);
                }
            }

            return payload.TryGetValue("exitCodeHint", out var hint) && hint is int code ? code : 0;
        }
        catch (Exception ex)
        {
            var fail = new Dictionary<string, object?>
            {
                ["mode"] = opts.Mode,
                ["ok"] = false,
                ["failClosed"] = true,
                ["error"] = ex.GetType().Name + ": " + ex.Message,
                ["exitCodeHint"] = 3,
            };
            Console.Out.WriteLine(Prefix + JsonSerializer.Serialize(fail));
            Console.Out.Flush();
            return 3;
        }
    }

    // ---------------------------------------------------------------- baseline

    private static Dictionary<string, object?> Baseline(Opts opts)
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var hashLimit = (long)opts.HashLimitMb * 1024 * 1024;

        var targets = new (string Label, string Path)[]
        {
            ("prod", Path.Combine(local, "galleries-manager")),
            ("dev", Path.Combine(local, "galleries-manager-dev")),
        };

        var scans = new List<Dictionary<string, object?>>();
        var anyFailClosed = false;

        foreach (var (label, path) in targets)
        {
            var scan = ScanRoot(label, path, hashLimit, opts.EmitCap);
            if (scan.TryGetValue("failClosed", out var fc) && fc is true)
                anyFailClosed = true;
            scans.Add(scan);
        }

        return new Dictionary<string, object?>
        {
            ["mode"] = "baseline",
            ["probeVersion"] = 1,
            ["ok"] = true,
            ["failClosed"] = anyFailClosed,
            ["os"] = RuntimeInformation.OSDescription,
            ["machineName"] = Environment.MachineName,
            ["utc"] = DateTimeOffset.UtcNow.ToString("O"),
            ["localAppData"] = local,
            ["hashLimitBytes"] = hashLimit,
            ["emitCap"] = opts.EmitCap,
            ["roots"] = scans,
            ["exitCodeHint"] = anyFailClosed ? 3 : 0,
        };
    }

    /// <summary>
    /// 扫描单个根目录，产出「完整元数据 + 必要内容哈希」清单与一个覆盖**全部条目**的摘要哈希。
    /// 关键纪律：只记录 路径/种类/大小/mtime/SHA256 —— **绝不输出任何文件内容**。
    /// </summary>
    private static Dictionary<string, object?> ScanRoot(string label, string path, long hashLimit, int emitCap)
    {
        var result = new Dictionary<string, object?>
        {
            ["label"] = label,
            ["path"] = path,
            ["exists"] = false,
            ["failClosed"] = false,
            ["unreadableCount"] = 0,
            ["entriesOmitted"] = true,
            ["entriesTruncated"] = false,
        };

        if (!Directory.Exists(path))
        {
            // 目录不存在是**合法**状态（例如用户从未跑过 dev 构建），不是失败。
            return result;
        }
        result["exists"] = true;

        var lines = new List<string>();
        var entries = new List<Dictionary<string, object?>>();
        var unreadable = new List<string>();
        var emitEntries = Environment.GetCommandLineArgs().Any(a => a == "--emit-entries");
        long fileCount = 0, dirCount = 0, totalBytes = 0, hashedCount = 0, skippedHashCount = 0;
        var failClosed = false;

        var stack = new Stack<string>();
        stack.Push(path);

        while (stack.Count > 0)
        {
            var dir = stack.Pop();
            string[] subDirs;
            string[] files;
            try
            {
                subDirs = Directory.GetDirectories(dir);
                files = Directory.GetFiles(dir);
            }
            catch (Exception ex)
            {
                failClosed = true;
                unreadable.Add(Rel(path, dir) + " : " + ex.GetType().Name);
                continue;
            }

            foreach (var sub in subDirs)
            {
                var rel = Rel(path, sub);
                dirCount++;
                lines.Add(rel + "|dir|||");
                if (entries.Count < emitCap)
                    entries.Add(new Dictionary<string, object?> { ["rel"] = rel, ["kind"] = "dir" });

                // 重解析点（符号链接 / junction）不递归，避免 %LOCALAPPDATA% 下经典的自引用环。
                try
                {
                    if (!new DirectoryInfo(sub).Attributes.HasFlag(FileAttributes.ReparsePoint))
                        stack.Push(sub);
                }
                catch (Exception ex)
                {
                    failClosed = true;
                    unreadable.Add(rel + " : " + ex.GetType().Name);
                }
            }

            foreach (var f in files)
            {
                var rel = Rel(path, f);
                long size;
                DateTime mtimeUtc;
                try
                {
                    var fi = new FileInfo(f);
                    size = fi.Length;
                    mtimeUtc = fi.LastWriteTimeUtc;
                }
                catch (Exception ex)
                {
                    failClosed = true;
                    unreadable.Add(rel + " : " + ex.GetType().Name);
                    continue;
                }

                fileCount++;
                totalBytes += size;

                string? sha = null;
                if (size <= hashLimit)
                {
                    try
                    {
                        sha = Sha256File(f);
                        hashedCount++;
                    }
                    catch (Exception ex)
                    {
                        // 读不到内容 → 降级为 metadata-only，并显式标注（不 fail closed：大小+mtime 仍在）。
                        unreadable.Add(rel + " : hash failed : " + ex.GetType().Name);
                    }
                }
                else
                {
                    skippedHashCount++;
                }

                lines.Add(rel + "|file|" + size + "|" + mtimeUtc.Ticks + "|" + (sha ?? "-"));

                if (entries.Count < emitCap)
                {
                    entries.Add(new Dictionary<string, object?>
                    {
                        ["rel"] = rel,
                        ["kind"] = "file",
                        ["size"] = size,
                        ["mtimeUtcTicks"] = mtimeUtc.Ticks,
                        ["sha256"] = sha,
                    });
                }
            }
        }

        lines.Sort(StringComparer.Ordinal);
        var manifest = Sha256Text(string.Join("\n", lines));

        result["failClosed"] = failClosed;
        result["fileCount"] = fileCount;
        result["dirCount"] = dirCount;
        result["totalBytes"] = totalBytes;
        result["hashedCount"] = hashedCount;
        result["skippedHashCount"] = skippedHashCount;
        result["manifestLineCount"] = lines.Count;
        result["manifestSha256"] = manifest;
        result["entriesTruncated"] = entries.Count >= emitCap && lines.Count > entries.Count;
        // 默认**不**把逐文件清单（路径 / 大小 / mtime / 逐文件 SHA256）写进 stdout：
        // stdout.log 落在队列目录内，而逐文件 SHA 对低熵配置文件（如 settings.json）构成弱泄露。
        // 前后比对只需根级 aggregate（manifestSha256 + fileCount/dirCount/totalBytes），
        // 清单仅在显式 --emit-entries（仅本机调试）时输出。
        result["unreadableCount"] = unreadable.Count;
        if (emitEntries)
        {
            result["unreadable"] = unreadable;
            result["entries"] = entries;
            result["entriesOmitted"] = false;
        }
        return result;
    }

    private static string Rel(string root, string full)
    {
        var r = Path.GetRelativePath(root, full).Replace('\\', '/');
        return r;
    }

    private static string Sha256Text(string s)
    {
        using var sha = SHA256.Create();
        return Convert.ToHexString(sha.ComputeHash(Encoding.UTF8.GetBytes(s))).ToLowerInvariant();
    }

    private static string Sha256File(string path)
    {
        using var sha = SHA256.Create();
        using var fs = File.OpenRead(path);
        return Convert.ToHexString(sha.ComputeHash(fs)).ToLowerInvariant();
    }

    // -------------------------------------------------------------- verify-shot

    private static Dictionary<string, object?> VerifyShot(Opts opts)
    {
        var outDict = new Dictionary<string, object?>
        {
            ["mode"] = "verify-shot",
            ["ok"] = false,
            ["bmpExpected"] = new int?[] { opts.ExpectW, opts.ExpectH },
            ["pngExpected"] = new int?[] { opts.PngExpectW, opts.PngExpectH },
            // 逐通道使用**该通道自己的**预期尺寸：BMP 是整个窗口矩形，PNG 是 Avalonia 的客户区
            // （DIP 尺寸，150% 缩放下与物理客户区不同），二者不可互相借用，否则必然假红。
            ["bmp"] = opts.Bmp is null ? null : InspectBmp(opts.Bmp, opts.ExpectW, opts.ExpectH),
            ["png"] = opts.Png is null ? null : InspectPng(opts.Png, opts.PngExpectW, opts.PngExpectH),
            ["exitCodeHint"] = 0,
        };

        var bmp = outDict["bmp"] as Dictionary<string, object?>;
        var png = outDict["png"] as Dictionary<string, object?>;
        var bmpDims = bmp is not null && bmp.TryGetValue("dimsOk", out var bd) && bd is true;
        var pngDims = png is not null && png.TryGetValue("dimsOk", out var pd) && pd is true;
        var bmpNonBlank = bmp is not null && bmp.TryGetValue("nonBlank", out var bn) && bn is true;
        var pngNonBlank = png is not null && png.TryGetValue("nonBlank", out var pn) && pn is true;

        var requested = (bmp is not null) || (png is not null);
        var dimsOk = requested && (!(bmp is not null) || bmpDims) && (!(png is not null) || pngDims);

        // 「窗口真的被绘制过」的**权威通道是 PNG**：Avalonia RenderTargetBitmap 渲染本应用自己的可视树，
        // 与 GPU/DWM 合成无关。Win32 PrintWindow 对 Skia/ANGLE 的 GPU 合成窗口常只得全黑，
        // 属已知环境限制 ⇒ 只作完整性（尺寸 + 哈希）复核，**不作非空白判据**，但如实上报。
        var nonBlankAuthority = png is not null ? pngNonBlank : bmpNonBlank;

        outDict["requestedChannelsDimsOk"] = dimsOk;
        outDict["bmpNonBlank"] = bmp is null ? null : bmpNonBlank;
        outDict["pngNonBlank"] = png is null ? null : pngNonBlank;
        outDict["nonBlankAuthority"] = png is not null ? "png" : "bmp";
        outDict["ok"] = dimsOk && nonBlankAuthority;
        outDict["note"] = (bmp is not null && !bmpNonBlank)
            ? "BMP(PrintWindow) 为单一色：GPU 合成窗口的已知限制，非失败；非空白证明以 PNG 通道为准"
            : null;
        outDict["exitCodeHint"] = outDict["ok"] is true ? 0 : 1;
        return outDict;
    }

    private static Dictionary<string, object?> InspectBmp(string path, int? expectW, int? expectH)
    {
        var r = new Dictionary<string, object?> { ["path"] = path, ["ok"] = false };
        try
        {
            if (!File.Exists(path))
            {
                r["error"] = "not found";
                return r;
            }
            var bytes = File.ReadAllBytes(path);
            r["bytes"] = bytes.LongLength;
            r["sha256"] = Sha256File(path);

            if (bytes.Length < 54 || bytes[0] != 'B' || bytes[1] != 'M')
            {
                r["error"] = "bad BMP signature";
                return r;
            }
            int offBits = BitConverter.ToInt32(bytes, 10);
            int headerSize = BitConverter.ToInt32(bytes, 14);
            int w = BitConverter.ToInt32(bytes, 18);
            int h = BitConverter.ToInt32(bytes, 22);
            short bpp = BitConverter.ToInt16(bytes, 28);
            int compression = BitConverter.ToInt32(bytes, 30);

            r["width"] = w;
            r["height"] = h;
            r["bitCount"] = bpp;
            r["compression"] = compression;
            r["headerSize"] = headerSize;
            r["offBits"] = offBits;

            if (bpp != 24 || compression != 0)
            {
                r["error"] = "only 24bpp BI_RGB BMP is parsed";
                return r;
            }

            int stride = ((w * 3 + 3) / 4) * 4;
            long need = offBits + (long)stride * h;
            if (need > bytes.Length)
            {
                r["error"] = $"truncated: need {need} have {bytes.Length}";
                return r;
            }

            // 采样像素，证明「窗口真的被绘制过」而不是单一填充色。
            var distinct = new HashSet<int>();
            int min = 255, max = 0;
            long sum = 0, n = 0;
            int stepY = Math.Max(1, h / 200);
            int stepX = Math.Max(1, w / 200);
            for (int y = 0; y < h; y += stepY)
            {
                int row = offBits + y * stride;
                for (int x = 0; x < w; x += stepX)
                {
                    int p = row + x * 3;
                    int bl = bytes[p], g = bytes[p + 1], rr = bytes[p + 2];
                    if (distinct.Count < 4096) distinct.Add((rr << 16) | (g << 8) | bl);
                    int lum = (rr * 299 + g * 587 + bl * 114) / 1000;
                    if (lum < min) min = lum;
                    if (lum > max) max = lum;
                    sum += lum; n++;
                }
            }

            r["sampledPixels"] = n;
            r["distinctColors"] = distinct.Count;
            r["lumaMin"] = min;
            r["lumaMax"] = max;
            r["lumaMean"] = n > 0 ? Math.Round((double)sum / n, 2) : null;
            r["nonBlank"] = distinct.Count > 1;

            var dimsOk = (expectW is null || expectW == w) && (expectH is null || expectH == h);
            r["dimsOk"] = dimsOk;
            r["ok"] = dimsOk;
            if (!dimsOk) r["error"] = $"size mismatch: got {w}x{h} expect {expectW}x{expectH}";
            return r;
        }
        catch (Exception ex)
        {
            r["error"] = ex.GetType().Name + ": " + ex.Message;
            return r;
        }
    }

    private static Dictionary<string, object?> InspectPng(string path, int? expectW, int? expectH)
    {
        var r = new Dictionary<string, object?> { ["path"] = path, ["ok"] = false };
        try
        {
            if (!File.Exists(path))
            {
                r["error"] = "not found";
                return r;
            }
            var bytes = File.ReadAllBytes(path);
            r["bytes"] = bytes.LongLength;
            r["sha256"] = Sha256File(path);

            if (bytes.Length < 33 ||
                bytes[0] != 0x89 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
            {
                r["error"] = "bad PNG signature";
                return r;
            }
            if (bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
            {
                r["error"] = "first chunk is not IHDR";
                return r;
            }
            int w = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
            int h = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
            r["width"] = w;
            r["height"] = h;

            var dimsOk = (expectW is null || expectW == w) && (expectH is null || expectH == h);
            r["dimsOk"] = dimsOk;
            r["ok"] = dimsOk;
            if (!dimsOk) r["error"] = $"size mismatch: got {w}x{h} expect {expectW}x{expectH}";

            // 解 PNG 像素（纯 BCL：ZLibStream 解压 IDAT + 逐行反滤波），
            // 使「窗口真的被绘制过」可在 Windows 侧**独立于应用自报**地被证明。
            try
            {
                var px = DecodePngPixels(bytes, w, h);
                foreach (var kv in px) r[kv.Key] = kv.Value;
            }
            catch (Exception ex)
            {
                r["pixelDecodeError"] = ex.GetType().Name + ": " + ex.Message;
            }
            return r;
        }
        catch (Exception ex)
        {
            r["error"] = ex.GetType().Name + ": " + ex.Message;
            return r;
        }
    }

    private static int Paeth(int a, int b, int c)
    {
        int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
    }

    /// <summary>解 PNG 像素并统计颜色多样性（支持 8bit 灰度/RGB/RGBA/GA、非隔行）。</summary>
    private static Dictionary<string, object?> DecodePngPixels(byte[] bytes, int w, int h)
    {
        var r = new Dictionary<string, object?>();
        int pos = 8, bitDepth = 0, colorType = 0, interlace = 0;
        var idat = new MemoryStream();
        while (pos + 8 <= bytes.Length)
        {
            int len = BE32(bytes, pos);
            if (len < 0 || pos + 12 + len > bytes.Length) break;
            byte t0 = bytes[pos + 4], t1 = bytes[pos + 5], t2 = bytes[pos + 6], t3 = bytes[pos + 7];
            if (t0 == 'I' && t1 == 'H' && t2 == 'D' && t3 == 'R')
            {
                bitDepth = bytes[pos + 16];
                colorType = bytes[pos + 17];
                interlace = bytes[pos + 20];
            }
            else if (t0 == 'I' && t1 == 'D' && t2 == 'A' && t3 == 'T')
            {
                idat.Write(bytes, pos + 8, len);
            }
            else if (t0 == 'I' && t1 == 'E' && t2 == 'N' && t3 == 'D') break;
            pos += 12 + len;
        }

        int ch = colorType switch { 0 => 1, 2 => 3, 4 => 2, 6 => 4, _ => 0 };
        r["bitDepth"] = bitDepth;
        r["colorType"] = colorType;
        r["interlace"] = interlace;
        if (ch == 0 || bitDepth != 8 || interlace != 0)
        {
            r["pixelDecode"] = "unsupported";
            r["nonBlank"] = null;
            return r;
        }

        idat.Position = 0;
        using var z = new ZLibStream(idat, CompressionMode.Decompress);
        int stride = w * ch;
        var raw = new byte[(stride + 1) * h];
        int read = 0;
        while (read < raw.Length)
        {
            int n = z.Read(raw, read, raw.Length - read);
            if (n <= 0) break;
            read += n;
        }
        if (read < raw.Length)
        {
            r["pixelDecode"] = $"short: {read}/{raw.Length}";
            r["nonBlank"] = null;
            return r;
        }

        var cur = new byte[stride];
        var prev = new byte[stride];
        var distinct = new HashSet<int>();
        int off = 0, min = 255, max = 0;
        long sum = 0, n2 = 0;
        int stepY = Math.Max(1, h / 120), stepX = Math.Max(1, w / 120);
        for (int y = 0; y < h; y++)
        {
            int ft = raw[off]; off++;
            Buffer.BlockCopy(raw, off, cur, 0, stride); off += stride;
            switch (ft)
            {
                case 1:
                    for (int i = ch; i < stride; i++) cur[i] = (byte)(cur[i] + cur[i - ch]);
                    break;
                case 2:
                    for (int i = 0; i < stride; i++) cur[i] = (byte)(cur[i] + prev[i]);
                    break;
                case 3:
                    for (int i = 0; i < stride; i++)
                    {
                        int a3 = i >= ch ? cur[i - ch] : 0;
                        cur[i] = (byte)(cur[i] + ((a3 + prev[i]) >> 1));
                    }
                    break;
                case 4:
                    for (int i = 0; i < stride; i++)
                    {
                        int a4 = i >= ch ? cur[i - ch] : 0;
                        int c4 = i >= ch ? prev[i - ch] : 0;
                        cur[i] = (byte)(cur[i] + Paeth(a4, prev[i], c4));
                    }
                    break;
            }

            if (y % stepY == 0)
            {
                for (int x = 0; x < w; x += stepX)
                {
                    int p = x * ch;
                    int rr, gg, bb, aa;
                    if (ch >= 3) { rr = cur[p]; gg = cur[p + 1]; bb = cur[p + 2]; aa = ch == 4 ? cur[p + 3] : 255; }
                    else { rr = gg = bb = cur[p]; aa = ch == 2 ? cur[p + 1] : 255; }
                    if (aa == 0) { rr = gg = bb = 0; }   // 全透明像素按「空白」计
                    if (distinct.Count < 4096) distinct.Add((aa << 24) | (rr << 16) | (gg << 8) | bb);
                    int lum = (rr * 299 + gg * 587 + bb * 114) / 1000;
                    if (lum < min) min = lum;
                    if (lum > max) max = lum;
                    sum += lum; n2++;
                }
            }
            Buffer.BlockCopy(cur, 0, prev, 0, stride);
        }

        r["pixelDecode"] = "ok";
        r["sampledPixels"] = n2;
        r["distinctColors"] = distinct.Count;
        r["lumaMin"] = min;
        r["lumaMax"] = max;
        r["lumaMean"] = n2 > 0 ? Math.Round((double)sum / n2, 2) : null;
        r["nonBlank"] = distinct.Count > 1;
        return r;
    }

    private static int BE32(byte[] b, int i) => (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3];

    // -------------------------------------------------------------- window-probe

    private static Dictionary<string, object?> WindowProbe(Opts opts)
    {
        var deadline = DateTime.UtcNow.AddSeconds(Math.Max(0, opts.WatchSeconds));
        var seen = new List<Dictionary<string, object?>>();
        var seenHandles = new HashSet<long>();
        Dictionary<string, object?>? firstVisible = null;

        do
        {
            foreach (var w in EnumTopLevelWindows())
            {
                if (w.Pid != opts.Pid) continue;
                if (!seenHandles.Add(w.Hwnd)) continue;
                seen.Add(new Dictionary<string, object?>
                {
                    ["hwnd"] = "0x" + w.Hwnd.ToString("X"),
                    ["hwndInt"] = w.Hwnd,
                    ["pid"] = w.Pid,
                    ["visible"] = w.Visible,
                    ["w"] = w.W,
                    ["h"] = w.H,
                    ["title"] = w.Title,
                    ["className"] = w.ClassName,
                    ["utc"] = DateTimeOffset.UtcNow.ToString("O"),
                });
                if (firstVisible is null && w.Visible && w.W > 0 && w.H > 0)
                    firstVisible = seen[^1];
            }
            if (firstVisible is not null) break;
            if (DateTime.UtcNow >= deadline) break;
            Thread.Sleep(Math.Max(50, opts.IntervalMs));
        } while (true);

        return new Dictionary<string, object?>
        {
            ["mode"] = "window-probe",
            ["probeVersion"] = 1,
            ["ok"] = firstVisible is not null,
            ["failClosed"] = false,
            ["os"] = RuntimeInformation.OSDescription,
            ["pid"] = opts.Pid,
            ["watchSeconds"] = opts.WatchSeconds,
            ["firstVisibleWindow"] = firstVisible,
            ["windowsSeen"] = seen,
            ["utc"] = DateTimeOffset.UtcNow.ToString("O"),
            ["exitCodeHint"] = firstVisible is not null ? 0 : 1,
        };
    }

    private sealed record WinInfo(nint Hwnd, int Pid, bool Visible, int W, int H, string Title, string ClassName);

    private static List<WinInfo> EnumTopLevelWindows()
    {
        var list = new List<WinInfo>();
        if (!OperatingSystem.IsWindows()) return list;

        NativeMethods.EnumWindows((hwnd, _) =>
        {
            try
            {
                NativeMethods.GetWindowThreadProcessId(hwnd, out uint pid);
                bool visible = NativeMethods.IsWindowVisible(hwnd);
                int w = 0, h = 0;
                if (NativeMethods.GetWindowRect(hwnd, out var rc))
                {
                    w = rc.Right - rc.Left;
                    h = rc.Bottom - rc.Top;
                }
                var sb = new StringBuilder(512);
                NativeMethods.GetWindowTextW(hwnd, sb, sb.Capacity);
                var cb = new StringBuilder(256);
                NativeMethods.GetClassNameW(hwnd, cb, cb.Capacity);
                list.Add(new WinInfo(hwnd, (int)pid, visible, w, h, sb.ToString(), cb.ToString()));
            }
            catch
            {
                // 单个窗口枚举失败不应中断整体枚举。
            }
            return true;
        }, 0);

        return list;
    }

    private static class NativeMethods
    {
        internal delegate bool EnumWindowsProc(nint hWnd, nint lParam);

        [StructLayout(LayoutKind.Sequential)]
        internal struct RECT { public int Left, Top, Right, Bottom; }

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, nint lParam);

        [DllImport("user32.dll", SetLastError = true)]
        internal static extern uint GetWindowThreadProcessId(nint hWnd, out uint lpdwProcessId);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool IsWindowVisible(nint hWnd);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetWindowRect(nint hWnd, out RECT lpRect);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern int GetWindowTextW(nint hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern int GetClassNameW(nint hWnd, StringBuilder lpClassName, int nMaxCount);
    }

    // ------------------------------------------------------------------- args

    private sealed class Opts
    {
        public string Mode = "";
        public string? Out;
        public string? Bmp;
        public string? Png;
        public int? ExpectW;
        public int? ExpectH;
        public int? PngExpectW;
        public int? PngExpectH;
        public int Pid;
        public int WatchSeconds = 20;
        public int IntervalMs = 250;
        public int HashLimitMb = 8;
        public int EmitCap = 5000;
    }

    private static Opts? ParseArgs(string[] args)
    {
        if (args.Length == 0) return null;
        // 子命令书写为 --baseline / --verify-shot / --window-probe，此处统一去掉前导短横线。
        var o = new Opts { Mode = args[0].TrimStart('-') };
        if (o.Mode is not ("baseline" or "verify-shot" or "window-probe")) return null;

        for (int i = 1; i < args.Length; i++)
        {
            var a = args[i];
            string? Next() => i + 1 < args.Length ? args[++i] : null;
            switch (a)
            {
                case "--out": o.Out = Next(); break;
                case "--bmp": o.Bmp = Next(); break;
                case "--png": o.Png = Next(); break;
                case "--pid": if (int.TryParse(Next(), out var p)) o.Pid = p; break;
                case "--expect-w": if (int.TryParse(Next(), out var ew)) o.ExpectW = ew; break;
                case "--expect-h": if (int.TryParse(Next(), out var eh)) o.ExpectH = eh; break;
                case "--png-expect-w": if (int.TryParse(Next(), out var pew)) o.PngExpectW = pew; break;
                case "--png-expect-h": if (int.TryParse(Next(), out var peh)) o.PngExpectH = peh; break;
                case "--watch-seconds": if (int.TryParse(Next(), out var ws)) o.WatchSeconds = ws; break;
                case "--interval-ms": if (int.TryParse(Next(), out var im)) o.IntervalMs = im; break;
                case "--hash-limit-mb": if (int.TryParse(Next(), out var hl)) o.HashLimitMb = hl; break;
                case "--emit-cap": if (int.TryParse(Next(), out var ec)) o.EmitCap = ec; break;
                default:
                    Console.Error.WriteLine("未知参数：" + a);
                    return null;
            }
        }

        // stdout 协议行是主通道；--out 只是可选的本地留存，故 baseline 不强制要求 --out。
        if (o.Mode == "verify-shot" && o.Bmp is null && o.Png is null) return null;
        if (o.Mode == "window-probe" && o.Pid <= 0) return null;
        return o;
    }
}
