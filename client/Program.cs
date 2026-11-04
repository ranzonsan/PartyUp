using Avalonia;
using PartyUp.Client.Headless;
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

        AppLog.Info($"partyup client starting: mode={Options.Mode} server={Options.Server}");

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

    /// <summary>Avalonia 入口。测试模式不会走到这里（不创建窗口）。</summary>
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();
}
