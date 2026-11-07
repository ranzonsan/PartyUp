using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PartyUp.Client.Services;

/// <summary>
/// 本机记住的一台服务器。只放展示信息与规范化后的 base URL；
/// 登录凭据不在这里（token 由 DPAPI 单独加密保存）。
/// </summary>
public sealed record KnownServer
{
    public string Url { get; init; } = string.Empty;

    /// <summary>服务器自报的名字（来自 `GET /api/server-info`）；探测失败时回落到主机名。</summary>
    public string Name { get; init; } = string.Empty;

    /// <summary>上次在这台服务器上登录的账号名（只用于输入框预填，不是凭据）。</summary>
    public string? LastUser { get; init; }

    public DateTimeOffset LastUsed { get; init; }
}

/// <summary>
/// 服务器簿：第一屏「我的服务器」列表的数据源，存 `%APPDATA%\PartyUp\servers.json`。
///
/// 设计要点：
///  * URL 一律先经 <see cref="ServerEndpoint.NormalizeBase"/> 规范化再比较/落盘，
///    否则 `http://h` 与 `http://h/` 会变成两条重复记录；
///  * 写入走「临时文件 + 覆盖移动」，进程被杀不会留下半截 JSON；
///  * 读坏时保留原文件（`.broken-<时间戳>`）再回落空列表——不能因为一条坏记录就让人打不开客户端。
/// </summary>
public sealed class ServerBook
{
    private const string FolderName = "PartyUp";
    private const string FileName = "servers.json";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        PropertyNameCaseInsensitive = true,
    };

    public static ServerBook Instance { get; private set; } = new();

    public static string FolderPath => AppPaths.Root;

    public static string FilePath => Path.Combine(FolderPath, FileName);

    /// <summary>按最近使用倒序的服务器列表。</summary>
    public List<KnownServer> Servers { get; set; } = new();

    /// <summary>上次使用的服务器 URL（规范化后）；没有则为 null。</summary>
    public string? SelectedUrl { get; set; }

    public KnownServer? Selected =>
        SelectedUrl is null
            ? null
            : Servers.FirstOrDefault(s => string.Equals(s.Url, SelectedUrl, StringComparison.Ordinal));

    public static ServerBook Load()
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                Instance = new ServerBook();
                return Instance;
            }

            var json = File.ReadAllText(FilePath);
            Instance = JsonSerializer.Deserialize<ServerBook>(json, JsonOptions) ?? new ServerBook();
            // 防御坏数据：空 URL 的条目没有意义，直接丢掉（不把坏记录带进 UI）。
            Instance.Servers = Instance.Servers
                .Where(s => !string.IsNullOrWhiteSpace(s.Url))
                .OrderByDescending(s => s.LastUsed)
                .ToList();
            return Instance;
        }
        catch (Exception ex)
        {
            AppLog.Warn("servers: load failed: " + ex.Message);
            TryPreserveBrokenFile();
            Instance = new ServerBook();
            return Instance;
        }
    }

    /// <summary>新增或更新一条（同 URL 视为同一台服务器），并置为当前选中。</summary>
    public KnownServer Upsert(Uri baseUri, string name, string? lastUser)
    {
        var url = ServerEndpoint.NormalizeBase(baseUri).ToString().TrimEnd('/');
        var existing = Servers.FirstOrDefault(s => string.Equals(s.Url, url, StringComparison.Ordinal));
        var entry = new KnownServer
        {
            Url = url,
            Name = string.IsNullOrWhiteSpace(name) ? (existing?.Name ?? baseUri.Host) : name,
            LastUser = string.IsNullOrWhiteSpace(lastUser) ? existing?.LastUser : lastUser,
            LastUsed = DateTimeOffset.Now,
        };

        Servers.RemoveAll(s => string.Equals(s.Url, url, StringComparison.Ordinal));
        Servers.Insert(0, entry);
        SelectedUrl = url;
        Save();
        return entry;
    }

    /// <summary>只更新「上次登录账号」（登录成功后调用，便于下次预填）。</summary>
    public void RememberUser(Uri baseUri, string? username)
    {
        var url = ServerEndpoint.NormalizeBase(baseUri).ToString().TrimEnd('/');
        var index = Servers.FindIndex(s => string.Equals(s.Url, url, StringComparison.Ordinal));
        if (index < 0)
        {
            Upsert(baseUri, baseUri.Host, username);
            return;
        }
        Servers[index] = Servers[index] with { LastUser = username, LastUsed = DateTimeOffset.Now };
        SelectedUrl = url;
        Save();
    }

    public void Remove(string url)
    {
        Servers.RemoveAll(s => string.Equals(s.Url, url, StringComparison.Ordinal));
        if (string.Equals(SelectedUrl, url, StringComparison.Ordinal))
        {
            SelectedUrl = Servers.FirstOrDefault()?.Url;
        }
        Save();
    }

    public void Select(string url)
    {
        if (Servers.All(s => !string.Equals(s.Url, url, StringComparison.Ordinal)))
        {
            return;
        }
        SelectedUrl = url;
        var index = Servers.FindIndex(s => string.Equals(s.Url, url, StringComparison.Ordinal));
        if (index >= 0)
        {
            Servers[index] = Servers[index] with { LastUsed = DateTimeOffset.Now };
            Servers.Sort((a, b) => b.LastUsed.CompareTo(a.LastUsed));
        }
        Save();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(FolderPath);
            var temp = FilePath + ".tmp";
            File.WriteAllText(temp, JsonSerializer.Serialize(this, JsonOptions));
            File.Move(temp, FilePath, overwrite: true);
        }
        catch (Exception ex)
        {
            AppLog.Warn("servers: save failed: " + ex.Message);
        }
    }

    private static void TryPreserveBrokenFile()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var backup = FilePath + ".broken-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
                File.Move(FilePath, backup, overwrite: true);
                AppLog.Info("servers: broken file kept at " + backup);
            }
        }
        catch
        {
            // 保留失败不影响启动：下次保存会覆盖。
        }
    }
}
