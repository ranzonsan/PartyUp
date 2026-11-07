using Avalonia;

namespace PartyUp.UiPrototype;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args) => AppBuilder.Configure<App>()
        .UsePlatformDetect()
        .LogToTrace()
        .StartWithClassicDesktopLifetime(args);
}
