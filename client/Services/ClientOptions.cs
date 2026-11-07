namespace PartyUp.Client.Services;

/// <summary>客户端运行模式。</summary>
public enum ClientMode
{
    /// <summary>正常 GUI。</summary>
    Gui,

    /// <summary>无头自检：媒体核心 + 两个连接的完整消息往返，打印 SELFTEST_* 行。</summary>
    SelfTest,

    /// <summary>两个真实进程互通测试的「A 角色」。</summary>
    RoleA,

    /// <summary>两个真实进程互通测试的「B 角色」。</summary>
    RoleB,

    /// <summary>由本进程再拉起一个 --role b 子进程，两个 OS 进程在同一频道里互发消息。</summary>
    Duo,

    /// <summary>发一条消息就退出（给 GUI 取证造一条「别人发来的实时消息」）。</summary>
    Send,

    /// <summary>屏幕共享的「共享方」：开一路共享，把信令转给服务端，跑若干秒后退出。</summary>
    ShareHost,

    /// <summary>屏幕共享的「观看方」：收到 offer 后应答，统计远端帧并打印结果。</summary>
    ShareView,

    /// <summary>只打印 pu_screen_sources 枚举结果（显示器 id / 标题 / 分辨率 / 类型）后退出。</summary>
    Sources,
}

/// <summary>
/// 命令行参数。GUI 模式只用 --server/--token 做自动登录；测试模式用其余字段。
/// 只解析 <c>--key value</c> 形态，不做任何格式解析。
/// </summary>
public sealed record ClientOptions
{
    public ClientMode Mode { get; init; } = ClientMode.Gui;

    public string Server { get; init; } = "http://127.0.0.1:8080";

    public string? BootstrapToken { get; init; }

    public string? InviteCode { get; init; }

    /// <summary>GUI 自动登录 / 单角色测试用的会话 token。</summary>
    public string? Token { get; init; }

    /// <summary>--duo 模式下 A 进程的 token。</summary>
    public string? TokenA { get; init; }

    /// <summary>--duo 模式下 B 子进程的 token。</summary>
    public string? TokenB { get; init; }

    /// <summary>
    /// 只用于内存归因测量：跳过媒体核心的加载与引擎创建（不再 <c>pu_engine_create</c>），
    /// 其他功能（连接/文字/presence）照常。默认 false = 正常加载。
    /// </summary>
    public bool NoMedia { get; init; }

    /// <summary>
    /// `--classic-ui`：启动旧骨架窗口而不是 v1.1 重构外壳。仅作为回退通道保留：
    /// 新外壳验收通过前，出问题可以用它跑既有链路；不再往里加功能。
    /// </summary>
    public bool ClassicUi { get; init; }

    /// <summary>
    /// `--screen N`（1 起算，对应 <c>Screens.All</c> 枚举顺序）：把主窗口放到指定显示器。
    /// 只用于调试与自动化验收（多屏机器上不占用用户正在使用的屏幕），不是终端用户功能。
    /// </summary>
    public int? WindowScreen { get; init; }

    /// <summary>
    /// `--appdata DIR`：用独立配置根目录（设置/服务器簿/记住我 token 都落到这里）。
    /// 用途：同一台机器上跑多个互不干扰的实例（自动化验收、便携配置、双账号调试）。
    /// 不指定时仍是 <c>%APPDATA%\PartyUp</c>。
    /// </summary>
    public string? AppDataRoot { get; init; }

    /// <summary>测试用频道名（不存在时由 A 创建）。</summary>
    public string Channel { get; init; } = "interop";

    /// <summary>
    /// 命令行里是否**显式**给了 <c>--channel</c>。GUI 的浏览频道选择只在显式指定时
    /// 覆盖「进第一个频道」的默认行为；否则每次登录都会对默认值报一次 "not found"。
    /// </summary>
    public bool ChannelSpecified { get; init; }

    /// <summary>`--voice-join`：登录并进入频道后自动加入语音（自动化验收用）。</summary>
    public bool VoiceAutoJoin { get; init; }

    /// <summary>
    /// 仅用于自动化/调试：强制以「被动（answerer）」身份加入 direct 语音，等对方 offer。
    /// 这不是传输方式选择（传输方式由服务端策略决定），只是直连模式下的角色编排。
    /// </summary>
    public bool VoicePassive { get; init; }

    /// <summary>`--voice-active`：自动加入时强制做 offerer（direct 验收用，与 --voice-passive 对称）。
    /// 仅影响 direct 模式；relay 下服务端才是 offerer，此开关被忽略。</summary>
    public bool VoiceActive { get; init; }

    /// <summary>
    /// 「谁在说话」的峰值阈值（16 bit 采样绝对值）。默认 500；
    /// 环境变量 PARTYUP_SPEAK_THRESHOLD 或 --speak-threshold 可调。
    /// 注意原生侧默认阈值 8 只是「非零门控」，不是 VAD（见 partyup_media.h）。
    /// </summary>
    public uint SpeakThreshold { get; init; } = ParseSpeakThreshold();

    /// <summary>`--voice-passive`：自动加入时用被动模式（flags bit0=1，等对端 offer）。</summary>
    /// <summary>--role send 要发送的正文。</summary>
    public string? Text { get; init; }

    /// <summary>屏幕共享测试用的源 id（默认内置合成源，不碰真实桌面）。</summary>
    public string ShareSourceId { get; init; } = "synthetic";

    /// <summary>屏幕共享测试跑的秒数。</summary>
    public int ShareSeconds { get; init; } = 12;

    /// <summary>--share host 时不再拉起观看方子进程（用于配合已经打开的 GUI 观看）。</summary>
    public bool ShareNoChild { get; init; }

    public static ClientOptions Parse(string[] args)
    {
        var options = new ClientOptions();
        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            switch (arg)
            {
                case "--selftest":
                    options = options with { Mode = ClientMode.SelfTest };
                    break;
                case "--duo":
                    options = options with { Mode = ClientMode.Duo };
                    break;
                case "--role":
                    var role = TakeValue(args, ref i, arg);
                    options = options with
                    {
                        Mode = role.ToLowerInvariant() switch
                        {
                            "b" => ClientMode.RoleB,
                            "send" => ClientMode.Send,
                            _ => ClientMode.RoleA,
                        },
                    };
                    break;
                case "--server":
                    options = options with { Server = TakeValue(args, ref i, arg) };
                    break;
                case "--bootstrap-token":
                    options = options with { BootstrapToken = TakeValue(args, ref i, arg) };
                    break;
                case "--invite-code":
                    options = options with { InviteCode = TakeValue(args, ref i, arg) };
                    break;
                case "--token":
                    options = options with { Token = TakeValue(args, ref i, arg) };
                    break;
                case "--token-a":
                    options = options with { TokenA = TakeValue(args, ref i, arg) };
                    break;
                case "--token-b":
                    options = options with { TokenB = TakeValue(args, ref i, arg) };
                    break;
                case "--no-media":
                    options = options with { NoMedia = true };
                    break;
                case "--classic-ui":
                    options = options with { ClassicUi = true };
                    break;
                case "--screen":
                    // 1 起算，对应 Screens.All 的枚举顺序（1 = 主屏）。只给调试/验收用：
                    // 多屏机器上把测试窗口放到非主屏，避免打断正在主屏工作的人。
                    if (int.TryParse(TakeValue(args, ref i, arg), out var screenIndex) && screenIndex >= 1)
                    {
                        options = options with { WindowScreen = screenIndex };
                    }
                    break;
                case "--appdata":
                    // 独立配置根目录：验收脚本用它隔离 `%APPDATA%\PartyUp`，互不污染。
                    options = options with { AppDataRoot = TakeValue(args, ref i, arg) };
                    break;
                case "--channel":
                    options = options with
                    {
                        Channel = TakeValue(args, ref i, arg),
                        ChannelSpecified = true,
                    };
                    break;
                case "--text":
                    options = options with { Text = TakeValue(args, ref i, arg) };
                    break;
                case "--share":
                    var shareRole = TakeValue(args, ref i, arg).ToLowerInvariant();
                    options = options with
                    {
                        Mode = shareRole switch
                        {
                            "view" or "viewer" => ClientMode.ShareView,
                            _ => ClientMode.ShareHost,
                        },
                    };
                    break;
                case "--share-source":
                    options = options with { ShareSourceId = TakeValue(args, ref i, arg) };
                    break;
                case "--share-seconds":
                    options = options with { ShareSeconds = int.Parse(TakeValue(args, ref i, arg)) };
                    break;
                case "--share-no-child":
                    options = options with { ShareNoChild = true };
                    break;
                case "--voice-join":
                    options = options with { VoiceAutoJoin = true };
                    break;
                case "--voice-passive":
                    options = options with { VoicePassive = true };
                    break;
                case "--voice-active":
                    options = options with { VoiceActive = true };
                    break;
                case "--speak-threshold":
                    var raw = TakeValue(args, ref i, arg);
                    if (uint.TryParse(raw, out var threshold) && threshold > 0)
                    {
                        options = options with { SpeakThreshold = threshold };
                    }
                    break;
                case "--sources":
                    options = options with { Mode = ClientMode.Sources };
                    break;
                default:
                    // Avalonia 自己会忽略未知参数；这里也静默跳过，避免破坏 GUI 启动。
                    break;
            }
        }
        return options;
    }

    private static uint ParseSpeakThreshold()
    {
        var env = Environment.GetEnvironmentVariable("PARTYUP_SPEAK_THRESHOLD");
        return uint.TryParse(env, out var value) && value > 0 ? value : 500;
    }

    private static string TakeValue(string[] args, ref int index, string flag)
    {
        if (index + 1 >= args.Length)
        {
            throw new ArgumentException($"{flag} 需要一个值");
        }
        index++;
        return args[index];
    }
}
