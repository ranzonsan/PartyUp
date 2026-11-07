using Avalonia;
using PartyUp.Client.Headless;
using PartyUp.Client.Localization;
using PartyUp.Client.Services;
using Velopack;

namespace PartyUp.Client;

internal static class Program
{
    /// <summary>解析后的命令行参数。App/MainWindow/Headless 共用一份。</summary>
    public static ClientOptions Options { get; private set; } = new();

    [STAThread]
    public static int Main(string[] args)
    {
        // 必须是 Main 的第一句：Velopack 在安装/更新/卸载时会用 --veloapp-* 参数重新调用本 exe，
        // 由 Run() 处理完钩子后直接退出；放在后面会让这些钩子走到正常启动路径（弹 GUI、超时被杀）。
        // 非 Velopack 安装（绿色版/直接 dotnet run）下 Run() 是 no-op，不影响其它启动方式。
        VelopackApp.Build().Run();

        try
        {
            Options = ClientOptions.Parse(args);
        }
        catch (ArgumentException ex)
        {
            AppLog.Fail("bad arguments: " + ex.Message);
            return 2;
        }

        // --appdata 必须在**任何**依赖配置目录的动作之前生效：设置/服务器簿/记住我 token 都经
        // AppPaths 定位，而单实例互斥的作用域名也由它派生（否则带 --appdata 的第二个实例会被
        // 第一个实例的全局互斥挡掉，多开测试直接失效）。
        if (!string.IsNullOrWhiteSpace(Options.AppDataRoot))
        {
            AppPaths.OverrideRoot = Options.AppDataRoot;
        }

        // 单实例守卫：服务端单端登录下两个客户端会互相踢下线，GUI 只允许一个实例。
        // 无头/自动化模式（--selftest、--role send 等）刻意不参与：它们要能在一个已运行的
        // GUI 旁边跑（例如 --role send 给 GUI 造一条实时消息），不能被单实例挡掉。
        var singleInstanceGuard = Options.Mode == ClientMode.Gui;
        if (singleInstanceGuard && !SingleInstance.TryAcquire(out _))
        {
            AppLog.Info("single-instance: forwarding activation to the running client, exiting");
            SingleInstance.SignalExisting();
            return 0;
        }

        try
        {
            // 本机设置与语言必须在 Avalonia 之前决定：XAML 里的 {loc:Tr ...} 在窗口载入时求值，
            // 之后再改 CurrentUICulture 不会影响已经载入的界面（语言设置重启后生效即由此而来）。
            if (!string.IsNullOrWhiteSpace(AppPaths.OverrideRoot))
            {
                AppLog.Info($"appdata: using override root {AppPaths.Root}");
            }

            var settings = AppSettings.Load();
            L.Apply(settings.Language);

            // 服务器簿必须在主窗口（ShellViewModel 构造）之前载入：ShellViewModel 只读
            // ServerBook.Instance 的快照列表，不负责加载。漏了这一行会导致 servers.json
            // 写得进但重启读不回——「记住我」自动登录与服务器列表都会静默失效
            // （2026-11-07 UIA 回归用例 2/6 抓到：重启后永远回到空的第一屏）。
            ServerBook.Load();

            AppLog.Info($"partyup client starting: mode={Options.Mode} server={Options.Server} lang={L.CurrentTag}");

            try
            {
                switch (Options.Mode)
                {
                    case ClientMode.SelfTest:
                        return SelfTest.RunAsync(Options).GetAwaiter().GetResult();
                    case ClientMode.RoleA:
                    case ClientMode.RoleB:
                    case ClientMode.Duo:
                    case ClientMode.Send:
                    case ClientMode.ShareHost:
                    case ClientMode.ShareView:
                    case ClientMode.Sources:
                        return InteropRole.RunAsync(Options).GetAwaiter().GetResult();
                    default:
                        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
                        return 0;
                }
            }
            catch (Exception ex)
            {
                AppLog.Fail("unhandled exception: " + ex);
                return 1;
            }
        }
        finally
        {
            // 进程退出时释放 Mutex/命名事件（第二个实例的 TryAcquire 成功后才会走到这里）。
            if (singleInstanceGuard)
            {
                SingleInstance.Release();
            }
        }
    }

    /// <summary>Avalonia 入口。测试模式不会走到这里（不创建窗口）。</summary>
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();
}
