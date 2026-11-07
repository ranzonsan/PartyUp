using System;
using System.IO;
using System.Reflection;
using System.Security;
using Microsoft.Win32;

namespace PartyUp.Client.Services;

/// <summary>
/// 开机启动注册（Windows 走 HKCU 的 Run 键）。
///
/// 设计约束：
///   * 只写当前用户的 <c>HKCU\Software\Microsoft\Windows\CurrentVersion\Run</c>，不碰 HKLM——
///     不需要管理员权限，也不影响其它用户；
///   * 写入后**读回校验**：注册表被组策略 / 安全软件静默拦截时 <c>SetValue</c> 可能不抛异常
///     但值并没有落地，所以只有读回一致才返回 true，读回不一致时回滚并给出可读原因；
///   * 非 Windows 平台明确返回 false + <c>"unsupported platform"</c>，由 UI 显示失败原因，
///     不允许「什么都没做却返回成功」（macOS/Linux 的 autostart 机制不同，未实现即如实报告）。
/// </summary>
public static class StartupRegistrar
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "PartyUp";

    /// <summary>
    /// 当前是否已注册开机启动。真源是注册表，不是 <c>AppSettings.LaunchAtStartup</c>：
    /// 用户可能手工删掉了 Run 值，或旧安装把值指向了别的路径（视为未注册，UI 应显示未勾选）。
    /// </summary>
    public static bool IsEnabled()
    {
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: false);
            var stored = key?.GetValue(ValueName) as string;
            if (string.IsNullOrWhiteSpace(stored))
            {
                return false;
            }
            var expected = BuildCommand();
            if (!string.Equals(stored, expected, StringComparison.OrdinalIgnoreCase))
            {
                // 旧安装 / 换过目录：值存在但不是当前程序的启动命令，不能当成「已启用」。
                AppLog.Warn($"startup: Run value points elsewhere: {stored}");
                return false;
            }
            return true;
        }
        catch (Exception ex) when (ex is SecurityException or UnauthorizedAccessException or IOException)
        {
            AppLog.Warn("startup: read HKCU Run failed: " + ex.Message);
            return false;
        }
        catch (Exception ex)
        {
            AppLog.Warn("startup: read HKCU Run failed: " + ex.Message);
            return false;
        }
    }

    /// <summary>
    /// 注册 / 取消开机启动。成功返回 true；失败返回 false 并通过 <paramref name="error"/>
    /// 返回可读原因（权限不足、被策略锁定、注册表读回不一致等）。
    /// </summary>
    public static bool SetEnabled(bool enabled, out string? error)
    {
        error = null;
        if (!OperatingSystem.IsWindows())
        {
            error = "unsupported platform";
            AppLog.Warn("startup: cannot register launch-at-startup on this platform");
            return false;
        }

        try
        {
            string command;
            try
            {
                command = BuildCommand();
            }
            catch (Exception ex)
            {
                error = "cannot build startup command: " + ex.Message;
                AppLog.Fail("startup: " + error);
                return false;
            }

            using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);
            if (key is null)
            {
                error = "cannot open HKCU Run key";
                AppLog.Fail("startup: " + error);
                return false;
            }

            if (enabled)
            {
                key.SetValue(ValueName, command, RegistryValueKind.String);

                // 读回校验：值是字符串且与刚写入的完全一致，才认为注册成功。
                var readBack = key.GetValue(ValueName) as string;
                if (!string.Equals(readBack, command, StringComparison.Ordinal))
                {
                    error = readBack is null
                        ? "value missing after write (registry write was blocked)"
                        : "value mismatch after write";
                    try
                    {
                        key.DeleteValue(ValueName, throwOnMissingValue: false);
                    }
                    catch (Exception revertEx)
                    {
                        AppLog.Warn("startup: rollback failed: " + revertEx.Message);
                    }
                    AppLog.Fail($"startup: register failed: {error}");
                    return false;
                }

                AppLog.Info($"startup: launch-at-startup registered as \"{command}\"");
                return true;
            }

            if (key.GetValue(ValueName) is not null)
            {
                key.DeleteValue(ValueName, throwOnMissingValue: false);
                if (key.GetValue(ValueName) is not null)
                {
                    error = "value still present after delete (registry write was blocked)";
                    AppLog.Fail("startup: " + error);
                    return false;
                }
            }

            AppLog.Info("startup: launch-at-startup removed");
            return true;
        }
        catch (UnauthorizedAccessException ex)
        {
            error = "access denied: " + ex.Message;
        }
        catch (SecurityException ex)
        {
            error = "blocked by policy: " + ex.Message;
        }
        catch (IOException ex)
        {
            error = "registry I/O failed: " + ex.Message;
        }
        catch (Exception ex)
        {
            error = ex.Message;
        }

        AppLog.Fail($"startup: register failed: {error}");
        return false;
    }

    /// <summary>
    /// 本次运行的启动命令。正常是带引号的 exe 全路径；
    /// 通过 <c>dotnet PartyUp.Client.dll</c> 启动（开发/调试）时退化为
    /// <c>dotnet "&lt;dll&gt;"</c>，并明确记日志说明注册的是宿主而不是 apphost。
    /// </summary>
    private static string BuildCommand()
    {
        var processPath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(processPath))
        {
            throw new InvalidOperationException("Environment.ProcessPath is empty");
        }

        var hostName = Path.GetFileNameWithoutExtension(processPath);
        if (hostName.Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            var entryAssembly = Assembly.GetEntryAssembly()
                ?? throw new InvalidOperationException("no entry assembly under dotnet host");
            var dllPath = entryAssembly.Location;
            if (string.IsNullOrWhiteSpace(dllPath))
            {
                throw new InvalidOperationException("entry assembly has no location");
            }
            AppLog.Warn($"startup: running under dotnet host ({processPath}); " +
                        $"registering dotnet \"{dllPath}\"");
            return $"\"{processPath}\" \"{dllPath}\"";
        }

        return $"\"{processPath}\"";
    }
}
