using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using PartyUp.Client.Localization;

namespace PartyUp.Client.Services;

/// <summary>
/// 本机客户端设置（与账号、服务器数据无关，纯 UI/设备偏好）。
///
/// 存 `%APPDATA%\PartyUp\settings.json`（Linux 上是 `~/.config/PartyUp/`），用 System.Text.Json 读写。
/// 写入走「临时文件 + 覆盖移动」，避免进程被杀时留下半截 JSON。
///
/// 注意：这里**不存**登录 token——token 属于凭据，需要用 DPAPI 单独加密保存
/// （见登录流程重构；未加密的 JSON 里不放凭据）。
/// </summary>
public sealed class AppSettings
{
    private const string FolderName = "PartyUp";
    private const string FileName = "settings.json";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        PropertyNameCaseInsensitive = true,
    };

    /// <summary>进程内共享实例；<see cref="Load"/> 之后可读。</summary>
    public static AppSettings Instance { get; private set; } = new();

    public static string FolderPath => AppPaths.Root;

    public static string FilePath => Path.Combine(FolderPath, FileName);

    // ---- 外观 ----
    /// <summary>语言标签：auto / zh-Hans / en（见 <see cref="L"/>）。</summary>
    public string Language { get; set; } = L.AutoTag;

    /// <summary>主题选项的稳定键（light / dark / system），不是显示名。</summary>
    public string Theme { get; set; } = "system";

    /// <summary>文字大小稳定键（normal / large）。</summary>
    public string TextSize { get; set; } = "normal";

    // ---- 音频 ----
    /// <summary>输入设备名（与 pu_audio_devices 返回的 name 对应；空 = 系统默认）。</summary>
    public string? InputDevice { get; set; }

    /// <summary>输出设备名（空 = 系统默认）。</summary>
    public string? OutputDevice { get; set; }

    // ---- 窗口与启动 ----
    /// <summary>「每次询问」：点窗口关闭时弹框让用户选（默认）。</summary>
    public const string CloseActionAsk = "ask";

    /// <summary>「最小化到托盘」：点关闭直接隐藏窗口，不再询问。</summary>
    public const string CloseActionTray = "tray";

    /// <summary>「直接退出」：点关闭立即退出进程，不再询问。</summary>
    public const string CloseActionExit = "exit";

    /// <summary>
    /// 点窗口关闭时的行为：<see cref="CloseActionAsk"/>（默认，每次弹框）/
    /// <see cref="CloseActionTray"/> / <see cref="CloseActionExit"/>。
    /// 用户勾选「记住我的选择」或改设置页下拉都会写这里。
    /// </summary>
    public string CloseAction { get; set; } = CloseActionAsk;

    /// <summary>旧版本的布尔开关（true=关闭到托盘）。仅为兼容旧 settings.json 保留，新逻辑一律用 <see cref="CloseAction"/>。</summary>
    public bool CloseToTray { get; set; }

    public bool LaunchAtStartup { get; set; }

    // ---- 更新 ----
    /// <summary>
    /// 启动后静默检查更新（默认开）。关掉后只有设置页的「检查更新」按钮会检查。
    /// 用户明确要求：静默检查、默认开启、只有发现新版本才提示。
    /// </summary>
    public bool AutoCheckUpdates { get; set; } = true;

    // ---- 登录 ----
    /// <summary>「记住我」。默认开：单端登录下每个服务器只有一个有效 token，缓存的收益大于风险。</summary>
    public bool RememberMe { get; set; } = true;

    /// <summary>上次连接的服务器地址（服务器列表落地前先用它做"上次的服务器"预填）。</summary>
    public string? LastServer { get; set; }

    // 窗口位置与尺寸（物理像素坐标 + 逻辑尺寸）。多屏用户希望下次启动回到同一块屏；
    // 恢复前必须校验那块屏还在，否则窗口会跑到屏幕外变成"打不开"。
    public int? WindowX { get; set; }

    public int? WindowY { get; set; }

    public double? WindowWidth { get; set; }

    public double? WindowHeight { get; set; }

    /// <summary>上次窗口所在显示器的稳定标识（设备名，如 <c>\\.\DISPLAY2</c>）；没有则回落主屏。</summary>
    public string? WindowScreenName { get; set; }

    /// <summary>读取设置；文件不存在或损坏时返回默认值，并把损坏文件另存一份便于排查。</summary>
    public static AppSettings Load()
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                Instance = new AppSettings();
                return Instance;
            }
            var json = File.ReadAllText(FilePath);
            Instance = Migrate(JsonSerializer.Deserialize<AppSettings>(json, JsonOptions) ?? new AppSettings());
        }
        catch (Exception ex)
        {
            AppLog.Fail("settings: load failed, using defaults: " + ex.Message);
            TryPreserveBrokenFile();
            Instance = new AppSettings();
        }
        return Instance;
    }

    /// <summary>
    /// 旧版本只有布尔 <see cref="CloseToTray"/>（true=关闭到托盘）。迁移到三态 <see cref="CloseAction"/>：
    /// true → tray；false/缺省 → ask，因为旧默认值 false 只在用户没动过开关时出现，不能当成「明确选择」。
    /// </summary>
    private static AppSettings Migrate(AppSettings settings)
    {
        if (settings.CloseAction is not (CloseActionAsk or CloseActionTray or CloseActionExit))
        {
            settings.CloseAction = CloseActionAsk;
        }
        if (settings.CloseToTray && settings.CloseAction == CloseActionAsk)
        {
            settings.CloseAction = CloseActionTray;
        }
        return settings;
    }

    /// <summary>写盘。失败只记日志，不让设置项把 UI 卡住或抛异常。</summary>
    public void Save()
    {
        try
        {
            Directory.CreateDirectory(FolderPath);
            var json = JsonSerializer.Serialize(this, JsonOptions);
            var temp = FilePath + ".tmp";
            File.WriteAllText(temp, json);
            File.Move(temp, FilePath, overwrite: true);
        }
        catch (Exception ex)
        {
            AppLog.Fail("settings: save failed: " + ex.Message);
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
                AppLog.Info("settings: broken file kept at " + backup);
            }
        }
        catch
        {
            // 保留失败不影响启动：下次保存会覆盖。
        }
    }
}
