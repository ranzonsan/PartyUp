using Avalonia;
using PartyUp.Client.Headless;
using PartyUp.Client.Services;

namespace PartyUp.Client;

internal static class Program
{
    /// <summary>解析后的命令行参数。App/MainWindow/Headless 共用一份。</summary>
    public static ClientOptions Options { get; private set; } = new();

    [STAThread]
    public static int Main(string[] args)
    {
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
