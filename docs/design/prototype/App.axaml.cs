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
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;
            var window = new PrototypeWindow();
            window.Opened += (_, _) => Console.WriteLine(
                $"PROTOTYPE_OPENED client={window.ClientSize.Width}x{window.ClientSize.Height} renderScaling={window.RenderScaling} demoOnly=true");
            window.SizeChanged += (_, _) => Console.WriteLine(
                $"PROTOTYPE_SIZE client={window.ClientSize.Width}x{window.ClientSize.Height} renderScaling={window.RenderScaling}");
            using var iconStream = AssetLoader.Open(
                new Uri("avares://PartyUp.UiPrototype/Assets/icon.ico"));
            window.Icon = new WindowIcon(iconStream);
            desktop.MainWindow = window;
        }
        base.OnFrameworkInitializationCompleted();
    }
}
