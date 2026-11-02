using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using PartyUp.Client.Services;
using PartyUp.Client.ViewModels;

namespace PartyUp.Client;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var viewModel = new MainViewModel();
            desktop.MainWindow = new MainWindow(viewModel);
            AppLog.Info("gui: main window created");
        }
        base.OnFrameworkInitializationCompleted();
    }
}
