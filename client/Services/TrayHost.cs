using System;
using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Platform;
using Avalonia.Threading;
using PartyUp.Client.Localization;
using PartyUp.Client.ViewModels;

namespace PartyUp.Client.Services;

/// <summary>
/// 系统托盘宿主：一个 <see cref="TrayIcon"/> + 原生菜单（显示主窗口 / 麦克风静音 / 退出）。
///
/// 生命周期：由 <c>App.OnFrameworkInitializationCompleted</c> 在创建 <c>ShellWindow</c> 之后
/// 构造并 <see cref="Initialize"/>，应用 <c>Exit</c> 时 <see cref="Dispose"/>。
///
/// 为什么先建 <see cref="TrayIcons"/> 再 Add：Avalonia 的 TrayIcon 只有加入挂在
/// <c>Application</c> 上的 <see cref="TrayIcons"/> 集合才会真正 Attach 到平台，
/// 直接 new 一个 TrayIcon 不会出现托盘图标。
/// </summary>
public sealed class TrayHost : IDisposable
{
    private static readonly Uri IconUri = new("avares://PartyUp.Client/Assets/icon.ico");

    /// <summary>双击判定窗口：Win32 托盘只暴露 WM_LBUTTONUP（见 Avalonia TrayIconImpl），
    /// 双击=两次 Clicked，用间隔自己判定，避免依赖平台实现细节。</summary>
    private static readonly TimeSpan DoubleClickWindow = TimeSpan.FromMilliseconds(500);

    private readonly Window _window;
    private readonly ShellViewModel _viewModel;

    private TrayIcons? _icons;
    private TrayIcon? _trayIcon;
    private WindowIcon? _windowIcon;
    private NativeMenuItem? _muteItem;
    private DateTime _lastClickUtc = DateTime.MinValue;
    private bool _disposed;

    public TrayHost(Window window, ShellViewModel viewModel)
    {
        _window = window ?? throw new ArgumentNullException(nameof(window));
        _viewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        _viewModel.PropertyChanged += OnViewModelPropertyChanged;
    }

    /// <summary>创建托盘图标并挂上菜单；重复调用安全（幂等）。</summary>
    public void Initialize()
    {
        if (_disposed || _trayIcon is not null)
        {
            return;
        }

        var showItem = new NativeMenuItem(L.Get("Tray_Show"));
        showItem.Click += (_, _) =>
        {
            AppLog.Info("tray: menu show clicked");
            ShowWindow();
        };

        _muteItem = new NativeMenuItem(L.Get("Tray_Mute"))
        {
            ToggleType = MenuItemToggleType.CheckBox,
            IsChecked = _viewModel.IsVoiceMuted,
        };
        _muteItem.Click += (_, _) => ToggleMuteFromTray();

        var exitItem = new NativeMenuItem(L.Get("Tray_Exit"));
        exitItem.Click += (_, _) => ExitApplication();

        var menu = new NativeMenu();
        menu.Items.Add(showItem);
        menu.Items.Add(_muteItem);
        menu.Items.Add(new NativeMenuItemSeparator());
        menu.Items.Add(exitItem);

        _windowIcon = LoadIcon();
        _trayIcon = new TrayIcon
        {
            Icon = _windowIcon,
            ToolTipText = "PartyUp",
            Menu = menu,
            IsVisible = true,
        };
        _trayIcon.Clicked += OnTrayIconClicked;

        var app = Application.Current
                  ?? throw new InvalidOperationException("Application.Current is null");
        _icons = TrayIcon.GetIcons(app);
        if (_icons is null)
        {
            _icons = new TrayIcons();
            TrayIcon.SetIcons(app, _icons);
        }
        _icons.Add(_trayIcon);

        // 第二个实例通过命名事件请求「把主窗口提到前台」；窗口操作统一走 Dispatcher。
        SingleInstance.RegisterActivationHandler(ShowWindow);

        AppLog.Info("tray: icon created (menu=show/mute/exit)");
    }

    /// <summary>显示并激活主窗口（托盘双击/菜单、单实例唤醒共用这一条路径）。</summary>
    public void ShowWindow()
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (_disposed)
            {
                return;
            }
            if (!_window.IsVisible)
            {
                _window.Show();
            }
            if (_window.WindowState == WindowState.Minimized)
            {
                _window.WindowState = WindowState.Normal;
            }
            _window.Activate();
            AppLog.Info("tray: main window shown and activated");
        });
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        _viewModel.PropertyChanged -= OnViewModelPropertyChanged;
        SingleInstance.ClearActivationHandler();

        if (_trayIcon is not null)
        {
            _trayIcon.Clicked -= OnTrayIconClicked;
            _icons?.Remove(_trayIcon);
            _trayIcon.Dispose();
            _trayIcon = null;
            AppLog.Info("tray: icon disposed");
        }

        _icons = null;
        _windowIcon = null;
        _muteItem = null;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ShellViewModel.IsVoiceMuted))
        {
            // VM 的属性变化在 UI 线程触发，但这里 Post 一次更稳妥：
            // 原生菜单的勾选状态只允许在 UI 线程改。
            Dispatcher.UIThread.Post(SyncMuteCheckState);
        }
    }

    private void SyncMuteCheckState()
    {
        if (_disposed || _muteItem is null)
        {
            return;
        }
        var muted = _viewModel.IsVoiceMuted;
        if (_muteItem.IsChecked != muted)
        {
            _muteItem.IsChecked = muted;
        }
    }

    private void ToggleMuteFromTray()
    {
        // 命令内部会同步把 IsVoiceMuted 更新到 VM（点击后 SyncCallState），
        // 这里再对齐一次菜单勾选状态，保证「命令没生效（未在语音里）」时菜单不残留假勾选。
        _viewModel.ToggleMuteCommand.Execute(null);
        SyncMuteCheckState();
        AppLog.Info($"tray: toggle mute (muted={_viewModel.IsVoiceMuted})");
    }

    private void OnTrayIconClicked(object? sender, EventArgs e)
    {
        var now = DateTime.UtcNow;
        if (now - _lastClickUtc <= DoubleClickWindow)
        {
            _lastClickUtc = DateTime.MinValue;
            AppLog.Info("tray: double click -> show main window");
            ShowWindow();
            return;
        }
        _lastClickUtc = now;
    }

    private void ExitApplication()
    {
        AppLog.Info("tray: exit requested");
        // 先解除「关闭到托盘」拦截，再走桌面生命周期 Shutdown（关闭主窗口 → 触发清理 → 退出）。
        if (_window is ShellWindow shell)
        {
            shell.RequestExit();
        }

        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.Shutdown();
        }
        else
        {
            _window.Close();
        }
    }

    private static WindowIcon? LoadIcon()
    {
        try
        {
            using var stream = AssetLoader.Open(IconUri);
            return new WindowIcon(stream);
        }
        catch (Exception ex)
        {
            AppLog.Warn("tray: icon load failed: " + ex.Message);
            return null;
        }
    }
}
