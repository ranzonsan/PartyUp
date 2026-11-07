using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using PartyUp.Client.Services;
using PartyUp.Client.ViewModels;

namespace PartyUp.Client;

public partial class App : Application
{
    /// <summary>托盘宿主。只在 v1.1 外壳路径创建（classic 回退通道保持无托盘）。</summary>
    private TrayHost? _tray;

    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            if (Program.Options.ClassicUi)
            {
                // 回退通道：旧骨架窗口（v1 布局）。只用于新外壳出问题时保住既有链路，
                // 不再往里加功能；两条路径共用同一套 Services/媒体核心，不维护第二套传输栈。
                var legacy = new MainViewModel();
                desktop.MainWindow = new MainWindow(legacy);
                AppLog.Info("gui: classic (v1) window created");
            }
            else
            {
                var shell = new ShellViewModel();
                var window = new ShellWindow(shell);
                window.ApplyIcon();
                desktop.MainWindow = window;
                AppLog.Info("gui: v1.1 shell window created");

                // 托盘必须在窗口之后创建：菜单「显示主窗口」和单实例唤醒都指向这个窗口实例。
                _tray = new TrayHost(window, shell);
                _tray.Initialize();

                // 应用生命周期退出（含托盘「退出」）时移除托盘图标。
                desktop.Exit += OnDesktopExit;
            }
        }
        base.OnFrameworkInitializationCompleted();
    }

    private void OnDesktopExit(object? sender, ControlledApplicationLifetimeExitEventArgs e)
    {
        _tray?.Dispose();
        _tray = null;
    }
}
