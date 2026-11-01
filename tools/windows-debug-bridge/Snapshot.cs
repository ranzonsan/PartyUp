// Windows 调试桥 · 源快照（CLI 侧，只写队列目录内的 payload）
//
// 目的：把 WSL 侧源码冻结成任务目录里的 <queue>/jobs/<id>/payload，
// 由 Windows Worker 再复制成本机专属副本后构建/运行（绝不在 UNC 上构建）。
//
// 安全约束（硬性）：
//   * 只读源目录、只写 payload 根之内；任何目标路径归一化后必须仍在 payload 根内；
//   * 跳过符号链接/重解析点（既防路径穿越，也防把仓库外的内容搬进副本）；
//   * 遇到 Windows 非法文件名（保留名、非法字符、结尾空格/点）直接失败，绝不静默改名；
//   * 不做任何删除：payload 目录是新任务目录，天然为空。
using System.Security.Cryptography;
using System.Text;

namespace GalleriesManager.Wdb;

public sealed class SnapshotException : Exception
{
    public SnapshotException(string message) : base(message) { }
}

/// <param name="SourceRoot">WSL 侧源目录（绝对路径）。</param>
/// <param name="PayloadRoot">payload 根（绝对路径）。</param>
/// <param name="FileCount">已复制（或清点）的文件数。</param>
/// <param name="TotalBytes">已复制（或清点）的字节总数。</param>
/// <param name="SkippedCount">被跳过的条目总数（真值，可能大于 <paramref name="Skipped"/> 的长度）。</param>
/// <param name="Skipped">被跳过的条目明细（相对路径 + 原因），最多保留前 200 条。</param>
/// <param name="Excluded">命中的排除目录名（去重排序）。</param>
/// <param name="ManifestHash">相对路径+大小清单的 SHA-256（小写）。</param>
public sealed record SnapshotResult(
    string SourceRoot,
    string PayloadRoot,
    int FileCount,
    long TotalBytes,
    int SkippedCount,
    string[] Skipped,
    string[] Excluded,
    string ManifestHash);

public static class Snapshot
{
    /// <summary>单文件上限：超过则跳过并记录（仓库内源码远小于此值）。</summary>
    public const long MaxFileBytes = 64L * 1024 * 1024;

    private const int MaxSkippedReported = 200;

    /// <summary>
    /// 默认排除的目录名（按名字、大小写不敏感）。
    /// 注意：这里只排除**目录名**，因此仓库根的 Directory.Build.props / Directory.Packages.props
    /// 等根级文件一定会被包含（CPM 与 TargetFramework 依赖它们）。
    /// </summary>
    public static readonly string[] DefaultExcludedDirectoryNames =
    {
        ".git", ".hg", ".svn", ".vs", ".idea", ".scratch", ".wdb", ".queue", ".dsh-memory",
        "bin", "obj", "node_modules", "artifacts", "TestResults", "llamacpp", "eval",
    };

    private static readonly string[] SkippedFileNames =
    {
        "Thumbs.db", ".DS_Store", "desktop.ini",
    };

    /// <summary>只清点不复制（doctor 用）。</summary>
    public static SnapshotResult Inspect(string sourceRoot, IEnumerable<string>? extraExcludes = null, string? queueRoot = null) =>
        Walk(sourceRoot, payloadRoot: null, extraExcludes, queueRoot);

    /// <summary>复制源快照到 payload 根。失败抛 <see cref="SnapshotException"/>（调用方据此拒绝提交）。</summary>
    public static SnapshotResult Copy(string sourceRoot, string payloadRoot, IEnumerable<string>? extraExcludes = null, string? queueRoot = null) =>
        Walk(sourceRoot, payloadRoot, extraExcludes, queueRoot);

    private static SnapshotResult Walk(string sourceRoot, string? payloadRoot, IEnumerable<string>? extraExcludes, string? queueRoot)
    {
        if (string.IsNullOrWhiteSpace(sourceRoot))
            throw new SnapshotException("源目录未指定。");

        var root = TrimSeparators(Path.GetFullPath(sourceRoot));
        if (!Directory.Exists(root))
            throw new SnapshotException("源目录不存在：" + root);

        var dest = payloadRoot is null ? null : TrimSeparators(Path.GetFullPath(payloadRoot));

        var excludedNames = new HashSet<string>(DefaultExcludedDirectoryNames, StringComparer.OrdinalIgnoreCase);
        var excludedPaths = new List<string>();

        foreach (var raw in extraExcludes ?? Array.Empty<string>())
        {
            if (string.IsNullOrWhiteSpace(raw)) continue;
            var value = raw.Trim();

            if (value.Contains('/') || value.Contains('\\'))
            {
                var full = TrimSeparators(Path.GetFullPath(Path.Combine(root, value.Replace('\\', '/'))));
                excludedPaths.Add(full);
            }
            else
            {
                excludedNames.Add(value);
            }
        }

        // 队列目录本身（通常位于仓库根 .wdb/）必须排除，否则快照会递归吞掉自己
        if (!string.IsNullOrWhiteSpace(queueRoot))
            excludedPaths.Add(TrimSeparators(Path.GetFullPath(queueRoot)));

        // payload 目标若与源目录重叠，同样必须排除（否则会把正在生成的 payload 递归打包进自己）
        if (dest is not null)
        {
            if (string.Equals(dest, root, PathComparison))
                throw new SnapshotException("payload 目标目录不能等于源目录：" + dest);
            if (IsUnder(dest, root))
                excludedPaths.Add(dest);
        }

        if (dest is not null) Directory.CreateDirectory(dest);

        var manifest = new List<string>();
        var skipped = new List<string>();
        var excludedHits = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var fileCount = 0;
        var skippedCount = 0;
        long totalBytes = 0;

        // 跳过项的明细有上限，但计数必须是真的 —— 调用方据此告警（例如关键文件超限被跳过）
        void Skip(string item)
        {
            skippedCount++;
            if (skipped.Count < MaxSkippedReported) skipped.Add(item);
        }

        // 栈里必须同时携带「源目录 / 相对路径 / 镜像目标目录」，否则子目录内容会被摊平到 payload 根
        var stack = new Stack<(string Dir, string Rel, string? DestDir)>();
        stack.Push((root, string.Empty, dest));

        while (stack.Count > 0)
        {
            var (dir, rel, destDir) = stack.Pop();

            string[] entries;
            try
            {
                entries = Directory.GetFileSystemEntries(dir);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Skip(Rel(rel, Path.GetFileName(dir)) + " (" + ex.GetType().Name + ")");
                continue;
            }

            Array.Sort(entries, StringComparer.Ordinal);

            // Windows 文件系统大小写不敏感：同目录内 README.md 与 Readme.md 会在副本里互相覆盖
            var seenNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var entry in entries)
            {
                var name = Path.GetFileName(entry);
                var entryRel = Rel(rel, name);

                FileAttributes attributes;
                try
                {
                    attributes = File.GetAttributes(entry);
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Skip(entryRel + " (" + ex.GetType().Name + ")");
                    continue;
                }

                // 符号链接/重解析点一律跳过：防穿越，也防把仓库外内容卷入副本
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    Skip(entryRel + " (reparse-point)");
                    continue;
                }

                var isDirectory = (attributes & FileAttributes.Directory) != 0;

                // 路径级排除（队列目录 / --exclude 相对路径）
                var fullEntry = TrimSeparators(Path.GetFullPath(entry));
                var pathExcluded = excludedPaths.Any(p =>
                    string.Equals(fullEntry, p, PathComparison) || IsUnder(fullEntry, p));
                if (pathExcluded)
                {
                    excludedHits.Add(entryRel);
                    continue;
                }

                if (isDirectory)
                {
                    if (excludedNames.Contains(name))
                    {
                        excludedHits.Add(name);
                        continue;
                    }

                    if (!seenNames.Add(name))
                        throw new SnapshotException($"同一目录存在 Windows 大小写不敏感重名（副本会互相覆盖）：{entryRel}");

                    if (!IsWindowsSafeName(name, out var dirReason))
                        throw new SnapshotException($"目录名不是合法的 Windows 文件名（{dirReason}）：{entryRel}");

                    string? childDest = null;
                    if (destDir is not null)
                    {
                        childDest = Path.Combine(destDir, name);
                        AssertUnder(childDest, dest!, entryRel);
                        if (!Directory.Exists(childDest)) Directory.CreateDirectory(childDest);
                    }

                    stack.Push((entry, entryRel, childDest));
                    continue;
                }

                if (SkippedFileNames.Contains(name, StringComparer.OrdinalIgnoreCase)) continue;
                if (name.EndsWith(".user", StringComparison.OrdinalIgnoreCase)) continue;

                if (!seenNames.Add(name))
                    throw new SnapshotException($"同一目录存在 Windows 大小写不敏感重名（副本会互相覆盖）：{entryRel}");

                if (!IsWindowsSafeName(name, out var fileReason))
                    throw new SnapshotException($"文件名不是合法的 Windows 文件名（{fileReason}）：{entryRel}");

                long size;
                try
                {
                    size = new FileInfo(entry).Length;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Skip(entryRel + " (" + ex.GetType().Name + ")");
                    continue;
                }

                if (size > MaxFileBytes)
                {
                    Skip($"{entryRel} (size={size} > {MaxFileBytes})");
                    continue;
                }

                if (destDir is not null)
                {
                    var destFile = Path.Combine(destDir, name);
                    AssertUnder(destFile, dest!, entryRel);
                    File.Copy(entry, destFile, overwrite: true);
                }

                fileCount++;
                totalBytes += size;
                manifest.Add(entryRel + "|" + size.ToString(System.Globalization.CultureInfo.InvariantCulture));
            }
        }

        manifest.Sort(StringComparer.Ordinal);
        var hashInput = new StringBuilder("wdb-payload-v1\n");
        foreach (var line in manifest) hashInput.Append(line).Append('\n');
        var hash = Convert.ToHexString(SHA256.HashData(new UTF8Encoding(false).GetBytes(hashInput.ToString()))).ToLowerInvariant();

        return new SnapshotResult(
            root,
            dest ?? string.Empty,
            fileCount,
            totalBytes,
            skippedCount,
            skipped.ToArray(),
            excludedHits.OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray(),
            hash);
    }

    /// <summary>Windows 非法文件名判定（保留设备名、非法字符、结尾空格/点、空名）。</summary>
    public static bool IsWindowsSafeName(string name, out string reason)
    {
        reason = string.Empty;

        if (string.IsNullOrWhiteSpace(name)) { reason = "空名"; return false; }
        if (name is "." or "..") { reason = "相对路径段"; return false; }

        foreach (var c in name)
        {
            if (c < 0x20) { reason = "包含控制字符"; return false; }
            if (c is '<' or '>' or ':' or '"' or '/' or '\\' or '|' or '?' or '*') { reason = "包含非法字符 " + c; return false; }
        }

        if (name[^1] is ' ' or '.') { reason = "以空格或点结尾"; return false; }

        var stem = name;
        var dot = stem.IndexOf('.');
        if (dot >= 0) stem = stem[..dot];

        if (ReservedDeviceNames.Contains(stem)) { reason = "Windows 保留设备名 " + stem; return false; }

        return true;
    }

    private static readonly HashSet<string> ReservedDeviceNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    private static StringComparison PathComparison =>
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    private static string Rel(string parent, string name) => parent.Length == 0 ? name : parent + "/" + name;

    private static string TrimSeparators(string path) => path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    private static bool IsUnder(string candidate, string root)
    {
        var prefix = root + Path.DirectorySeparatorChar;
        return candidate.StartsWith(prefix, PathComparison);
    }

    private static void AssertUnder(string candidate, string root, string relativePath)
    {
        var full = TrimSeparators(Path.GetFullPath(candidate));
        if (!IsUnder(full, TrimSeparators(Path.GetFullPath(root))))
            throw new SnapshotException("目标路径越出 payload 根：" + relativePath);
    }
}
