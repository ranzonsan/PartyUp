using System;
using System.Threading;
using System.Threading.Tasks;
using Velopack;

namespace PartyUp.Client.Services;

/// <summary>一次「检查更新」的结果分类。UI 只按这个分类选文案，不去解析异常文本。</summary>
public enum UpdateCheckStatus
{
    /// <summary>不是 Velopack 安装版（便携版 / 调试桥构建 / dotnet run），根本没有更新通道。</summary>
    NotInstalled,

    /// <summary>已是最新版。</summary>
    UpToDate,

    /// <summary>发现新版本。</summary>
    Available,

    /// <summary>检查失败（网络 / feed 配置 / 未知异常）。只记日志，不弹窗打扰用户。</summary>
    Unavailable,
}

/// <summary><see cref="UpdateService.CheckAsync"/> 的返回值；<see cref="Version"/>/<see cref="Notes"/> 只在 Available 时有值。</summary>
public sealed record UpdateCheckResult(
    UpdateCheckStatus Status,
    string? Version = null,
    string? Notes = null,
    string? Error = null);

/// <summary>
/// Velopack 更新通道（GitHub Releases 的 vpk feed）。
///
/// 约定：
///   * 对外方法**不抛异常**——更新是附加能力，不能因为网络问题影响登录 / 聊天 / 语音。
///   * <see cref="IsInstalled"/> 为 false（便携版、调试桥构建、dotnet 跑）时**不发起任何网络请求**。
///   * <see cref="DownloadAndApplyAsync"/> 成功时进程会被 Velopack 结束并重启，因此正常路径不会返回 true 之后继续跑。
/// </summary>
public sealed class UpdateService
{
    /// <summary>Velopack 会从 GitHub 仓库自动推导 Releases feed（releases.win.json）。</summary>
    public const string RepositoryUrl = "https://github.com/ranzonsan/PartyUp";

    private readonly object _gate = new();
    private UpdateManager? _manager;
    private bool _managerUnavailable;
    private UpdateInfo? _pending;

    /// <summary>
    /// 当前客户端版本。真源是程序集版本（csproj 从仓库根 VERSION 注入），
    /// 保证「显示的版本 = 包版本 = 发布 tag」。
    /// </summary>
    public string CurrentVersion =>
        typeof(UpdateService).Assembly.GetName().Version?.ToString(3) ?? "0.0.0";

    /// <summary>只有 Velopack 安装版才有更新通道；便携版恒为 false。</summary>
    public bool IsInstalled => GetManager()?.IsInstalled == true;

    private UpdateManager? GetManager()
    {
        lock (_gate)
        {
            if (_manager is not null)
            {
                return _manager;
            }
            if (_managerUnavailable)
            {
                return null;
            }
            try
            {
                // 位置参数：UpdateOptions 用默认值，locator 传 null 表示让 Velopack 自己探测安装位置。
                _manager = new UpdateManager(RepositoryUrl, new UpdateOptions(), null!);
                return _manager;
            }
            catch (Exception ex)
            {
                _managerUnavailable = true;
                AppLog.Fail("update: 初始化更新通道失败: " + ex.Message);
                return null;
            }
        }
    }

    /// <summary>检查更新；任何失败都折叠成 <see cref="UpdateCheckStatus.Unavailable"/>，绝不抛出。</summary>
    public async Task<UpdateCheckResult> CheckAsync(CancellationToken cancellationToken = default)
    {
        var manager = GetManager();
        if (manager is null)
        {
            return new UpdateCheckResult(UpdateCheckStatus.Unavailable, Error: "update manager unavailable");
        }
        if (!manager.IsInstalled)
        {
            AppLog.Info("update: 便携版（非 Velopack 安装），跳过检查");
            return new UpdateCheckResult(UpdateCheckStatus.NotInstalled);
        }

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var info = await manager.CheckForUpdatesAsync().ConfigureAwait(false);
            if (info is null)
            {
                AppLog.Info("update: 已是最新 (v" + CurrentVersion + ")");
                return new UpdateCheckResult(UpdateCheckStatus.UpToDate);
            }

            // VelopackAsset.Version 的类型随 Velopack 版本演过（string / SemanticVersion），
            // 这里只做字符串化，不绑死具体类型。
            var rawVersion = Convert.ToString(info.TargetFullRelease.Version) ?? string.Empty;
            var version = rawVersion.Trim().TrimStart('v', 'V');
            lock (_gate)
            {
                _pending = info;
            }
            AppLog.Pass("update: 发现新版本 v" + version + "（当前 v" + CurrentVersion + "）");
            return new UpdateCheckResult(UpdateCheckStatus.Available, version, info.TargetFullRelease.NotesMarkdown);
        }
        catch (OperationCanceledException)
        {
            return new UpdateCheckResult(UpdateCheckStatus.Unavailable);
        }
        catch (Exception ex)
        {
            AppLog.Fail("update: 检查失败: " + ex.Message);
            return new UpdateCheckResult(UpdateCheckStatus.Unavailable, Error: ex.Message);
        }
    }

    /// <summary>
    /// 下载已发现的新版本并重启应用完成更新。返回 false 表示没下载成（界面保持可用、可重试）。
    /// 成功路径不返回：<c>ApplyUpdatesAndRestart</c> 结束当前进程，由 Velopack 换文件后重启。
    /// </summary>
    public async Task<bool> DownloadAndApplyAsync(
        Action<int>? progress = null,
        CancellationToken cancellationToken = default)
    {
        var manager = GetManager();
        if (manager is null || !manager.IsInstalled)
        {
            return false;
        }

        UpdateInfo? info;
        lock (_gate)
        {
            info = _pending;
        }
        if (info is null)
        {
            var check = await CheckAsync(cancellationToken).ConfigureAwait(false);
            if (check.Status != UpdateCheckStatus.Available)
            {
                return false;
            }
            lock (_gate)
            {
                info = _pending;
            }
        }
        if (info is null)
        {
            return false;
        }

        try
        {
            await manager.DownloadUpdatesAsync(info, p => progress?.Invoke(p), cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            AppLog.Fail("update: 下载失败: " + ex.Message);
            return false;
        }

        try
        {
            AppLog.Info("update: 下载完成，重启应用完成更新");
            manager.ApplyUpdatesAndRestart(info.TargetFullRelease, null);
            return true;
        }
        catch (Exception ex)
        {
            AppLog.Fail("update: 应用更新失败: " + ex.Message);
            return false;
        }
    }
}
