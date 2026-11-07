using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Platform;

namespace PartyUp.UiPrototype;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Throwaway composition root: seed a local scene, not a real login/call.
            var state = new DemoState
            {
                IsLoggedIn = true,
                BrowseChannel = "开黑大厅",
                CallChannel = string.Empty,
                ThemeChoice = "浅色",
                StatusText = "本地演示 · 未连接服务器，也未访问音视频设备。",
            };
            var window = new SemiPreviewWindow(new SemiViewModel(state));
            using var icon = AssetLoader.Open(new Uri("avares://PartyUp.UiPrototype/Assets/icon.ico"));
            window.Icon = new WindowIcon(icon);
            window.Opened += (_, _) => Console.WriteLine(
                $"SEMI_PREVIEW client={window.ClientSize} scale={window.RenderScaling} theme=Semi.Avalonia/12.1.0.1 mock=true");
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;
            desktop.MainWindow = window;
        }
        base.OnFrameworkInitializationCompleted();
    }
}
