using Avalonia;

namespace PartyUp.UiPrototype;

internal static class Program
{
    // THROWAWAY UI PROTOTYPE: no production assemblies or media engine are loaded.
    [STAThread]
    public static int Main(string[] args)
    {
        return AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace()
            .StartWithClassicDesktopLifetime(args);
    }
}
