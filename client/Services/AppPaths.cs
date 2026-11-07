using System;
using System.IO;

namespace PartyUp.Client.Services;

/// <summary>
/// 客户端本机配置根目录的唯一来源。
///
/// 默认是 <c>%APPDATA%\PartyUp</c>；<c>--appdata DIR</c> 会覆盖它，让同一台机器上
/// 可以跑多个互不干扰的实例（自动化验收、便携配置、双账号调试）。
///
/// 注意：不能用设置 <c>APPDATA</c> 环境变量的办法来"改配置目录"——Windows 上
/// <see cref="Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData)"/>
/// 走的是 KnownFolder API，不读环境变量（2026-11-07 实测：探针设了 APPDATA 仍然
/// 写进了真实目录，两个并发验收互相污染服务器簿）。所以必须由进程内显式覆盖。
/// </summary>
public static class AppPaths
{
    private const string FolderName = "PartyUp";

    /// <summary>非 null 时替代默认配置根（<c>--appdata</c> 指向的目录本身，不含 PartyUp 后缀）。</summary>
    public static string? OverrideRoot { get; set; }

    /// <summary>PartyUp 配置目录；覆盖时自动创建。</summary>
    public static string Root
    {
        get
        {
            if (string.IsNullOrWhiteSpace(OverrideRoot))
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), FolderName);
            }

            var root = Path.GetFullPath(OverrideRoot);
            Directory.CreateDirectory(root);
            return root;
        }
    }

    public static string File(string name) => Path.Combine(Root, name);
}
