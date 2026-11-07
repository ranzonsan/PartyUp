using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PartyUp.Client.Services;
using PartyUp.Client.ViewModels;
using PartyUp.Client.Views;

namespace PartyUp.Client;

/// <summary>
/// v1.1 重构外壳的窗口代码：只做纯视图职责（主题/字号/布局/命中测试/滚动），
/// 业务编排全部在 <see cref="ShellViewModel"/>。
/// </summary>
public partial class ShellWindow : Window
{
    private readonly ShellViewModel _viewModel = null!;
    private readonly RemoteScreenPresenter _remoteScreen = null!;

    /// <summary>进入全屏前的窗口状态与左栏宽度（退出全屏时恢复，不写死 300）。</summary>
    private WindowState _preFullscreenState = WindowState.Normal;
    private double _savedChannelWidth = 360;

    /// <summary>
    /// 是否已请求真正退出。为 true 时 <see cref="OnClosing"/> 不再把关闭拦截成「隐藏到托盘」；
    /// 托盘「退出」与 <c>IClassicDesktopStyleApplicationLifetime.Shutdown()</c> 走 <see cref="RequestExit"/>。
    /// 绝不能让退出请求被关闭到托盘逻辑吞掉（那会让进程永远退不出去）。
    /// </summary>
    public bool ExitRequested { get; private set; }

    /// <summary>询问框是否已弹出：用户连点关闭时不要叠出多个对话框。</summary>
    private bool _closePromptOpen;

    /// <summary>是否已订阅 <see cref="Screens.Changed"/>（显示器 DPI/工作区变化时重新夹窗口尺寸）。</summary>
    private bool _screenClampHooked;

    /// <summary>请求真正退出：置位后关闭窗口即走正常退出路径（StopMediaPump/断连 → 生命周期退出）。</summary>
    public void RequestExit()
    {
        ExitRequested = true;
        AppLog.Info("gui: exit requested (close-to-tray bypassed)");
    }

    /// <summary>XAML 加载器要求的无参构造（设计器/热重载用）。</summary>
    public ShellWindow() : this(new ShellViewModel())
    {
    }

    public ShellWindow(ShellViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;

        _remoteScreen = new RemoteScreenPresenter(RemoteScreen);
        _viewModel.AttachRemoteScreen(_remoteScreen);
        _viewModel.ScrollToLatestRequested += () => ChatHistory.ScrollToEnd();
        _viewModel.ThemeChanged += ApplyTheme;
        _viewModel.TextSizeChanged += ApplyTextSize;
        _viewModel.PropertyChanged += OnViewModelPropertyChanged;
        _viewModel.Messages.CollectionChanged += OnMessagesChanged;

        ChatHistory.ScrollChanged += OnChatScrollChanged;
        AddHandler(KeyDownEvent, OnKeyDownTunnel, RoutingStrategies.Tunnel);
        Opened += OnOpened;
        Closing += OnClosing;
        Closed += OnClosed;

        ApplyTheme(_viewModel.ThemeChoice);
        ApplyTextSize(_viewModel.TextSizeChoice);
        ApplyLayout();
        RestoreWindowPlacement();

        AppLog.Info($"gui: shell xaml loaded (channelList={(ChannelList is null ? "missing" : "ok")}, " +
                    $"watchStage={(WatchStage is null ? "missing" : "ok")}, window={Width}x{Height})");
    }

    // ------------------------------------------------------------------
    // 窗口位置与尺寸：多屏用户下次启动应回到同一块屏；那块屏不在了就回主屏居中
    // （否则窗口落在屏幕外，用户会以为"程序打不开"）。
    // ------------------------------------------------------------------

    private void RestoreWindowPlacement()
    {
        try
        {
            var saved = AppSettings.Instance;
            if (saved.WindowWidth is double width && saved.WindowHeight is double height &&
                width >= MinWidth && height >= MinHeight)
            {
                Width = width;
                Height = height;
            }

            var screens = Screens.All;
            if (screens.Count == 0)
            {
                return;
            }

            if (Program.Options.WindowScreen is int screenNumber)
            {
                if (screenNumber <= screens.Count)
                {
                    CenterOnScreen(screens[screenNumber - 1]);
                    AppLog.Info($"gui: window placed on screen {screenNumber}/{screens.Count} (--screen)");
                }
                else
                {
                    AppLog.Warn($"gui: --screen {screenNumber} out of range (count={screens.Count}); using primary");
                    CenterOnScreen(Screens.Primary ?? screens[0]);
                }
                return;
            }

            if (saved.WindowX is int x && saved.WindowY is int y)
            {
                // 判据是「与某块屏的工作区有足够重叠」，而不是「某个点在里面」：
                // 窗口被拖到屏幕边缘（甚至标题栏探出工作区一点）是合法状态，
                // 用单点判定会把它误判成丢屏，然后平白无故跳回主屏。
                var logicalWidth = saved.WindowWidth ?? Width;
                var logicalHeight = saved.WindowHeight ?? Height;
                foreach (var screen in screens)
                {
                    var area = screen.WorkingArea;
                    var widthPx = (int)Math.Round(logicalWidth * screen.Scaling);
                    var heightPx = (int)Math.Round(logicalHeight * screen.Scaling);
                    var overlapX = Math.Min(x + widthPx, area.Right) - Math.Max(x, area.X);
                    var overlapY = Math.Min(y + heightPx, area.Bottom) - Math.Max(y, area.Y);
                    if (overlapX < 120 || overlapY < 60)
                    {
                        continue;
                    }

                    // 还在这块屏上：把可能探出工作区的部分夹回来，保证标题栏可点、窗口可拖动。
                    var clampedX = Math.Clamp(x, area.X - widthPx + 200, area.Right - 200);
                    var clampedY = Math.Clamp(y, area.Y, area.Bottom - 120);
                    Position = new PixelPoint(clampedX, clampedY);
                    AppLog.Info($"gui: window restored at {clampedX},{clampedY} " +
                                $"(screen={screen.DisplayName ?? saved.WindowScreenName ?? "?"})");
                    return;
                }

                AppLog.Warn($"gui: saved window position {x},{y} is off-screen; centering on primary");
            }

            CenterOnScreen(Screens.Primary ?? screens[0]);
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: window placement failed: " + ex.Message);
        }
    }

    private void CenterOnScreen(Avalonia.Platform.Screen screen)
    {
        var area = screen.WorkingArea;
        var widthPx = (int)Math.Round(Width * screen.Scaling);
        var heightPx = (int)Math.Round(Height * screen.Scaling);
        Position = new PixelPoint(
            area.X + Math.Max(0, (area.Width - widthPx) / 2),
            area.Y + Math.Max(0, (area.Height - heightPx) / 2));
    }

    private void SaveWindowPlacement()
    {
        try
        {
            var saved = AppSettings.Instance;
            saved.WindowX = Position.X;
            saved.WindowY = Position.Y;
            saved.WindowWidth = Width;
            saved.WindowHeight = Height;

            // 屏幕名只用于诊断日志；用窗口矩形与各屏工作区的重叠来找，
            // ScreenFromPoint 对副屏/负坐标返回 null（实测），不能拿它当唯一来源。
            var widthPx = (int)Math.Round(Width * RenderScaling);
            var heightPx = (int)Math.Round(Height * RenderScaling);
            string? screenName = null;
            foreach (var screen in Screens.All)
            {
                var area = screen.WorkingArea;
                var overlapX = Math.Min(Position.X + widthPx, area.Right) - Math.Max(Position.X, area.X);
                var overlapY = Math.Min(Position.Y + heightPx, area.Bottom) - Math.Max(Position.Y, area.Y);
                if (overlapX >= 120 && overlapY >= 60)
                {
                    screenName = screen.DisplayName;
                    break;
                }
            }
            saved.WindowScreenName = screenName;
            saved.Save();
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: saving window placement failed: " + ex.Message);
        }
    }

    private void OnViewModelPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ShellViewModel.IsFullScreen)
            or nameof(ShellViewModel.ShowWatchStage)
            or nameof(ShellViewModel.IsChatVisible))
        {
            ApplyLayout();
        }
    }

    private void OnMessagesChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
        => ScrollChatToEndIfFollowing();

    private void ScrollChatToEndIfFollowing()
    {
        if (!_viewModel.ChatAtEnd)
        {
            return;
        }
        // 等一次布局再滚：刚加入的消息还没完成测量，立刻 ScrollToEnd 会停在旧底部。
        Dispatcher.UIThread.Post(() => ChatHistory.ScrollToEnd(), DispatcherPriority.Background);
    }

    private void ApplyTheme(string choice)
    {
        // 用索引而不是比较中文显示名：显示名随界面语言变化，比较字符串会在英文界面下失效。
        var index = Array.IndexOf(ShellViewModel.ThemeChoices, choice);
        var variant = index switch
        {
            1 => ThemeVariant.Dark,
            2 => ThemeVariant.Default,
            _ => ThemeVariant.Light,
        };
        RequestedThemeVariant = variant;
        if (Application.Current is { } app)
        {
            app.RequestedThemeVariant = variant;
        }
    }

    private void ApplyTextSize(string choice) =>
        FontSize = Array.IndexOf(ShellViewModel.TextSizeChoices, choice) == 1 ? 16 : 14;

    /// <summary>全屏/观看布局：与独立预览同一套规则（频道栏收起、舞台占满）。</summary>
    private void ApplyLayout()
    {
        var fullscreen = _viewModel.IsFullScreen;
        if (fullscreen)
        {
            if (WindowState != WindowState.FullScreen)
            {
                // 记住进入全屏前的窗口状态与用户拖过的左栏宽度，退出时恢复（不要固定成 300）。
                _preFullscreenState = WindowState;
                _savedChannelWidth = AppGrid.ColumnDefinitions[0].ActualWidth;
            }
            WindowState = WindowState.FullScreen;
        }
        else
        {
            if (WindowState == WindowState.FullScreen)
            {
                WindowState = _preFullscreenState;
            }
            if (double.IsNaN(_savedChannelWidth) || _savedChannelWidth <= 0)
            {
                // 与 ShellWindow.axaml 第 0 列的默认宽（360）保持一致：300 装不下底部媒体卡片
                // 那一行的四个按钮（实测连卡片/栏内边距约需 276 DIP，300 只剩 24 DIP 余量）。
                _savedChannelWidth = 360;
            }
        }

        ChannelPane.IsVisible = !fullscreen;
        ChannelDivider.IsVisible = !fullscreen;
        // 左栏内容的最小宽度（底部四按钮那一行的硬底线）在全屏时必须归零：
        // 下面会把第 0 列宽设成 0，若这里还留着 340 DIP 的 MinWidth，列会被撑回来、全屏左侧留一条空带。
        ChannelPane.MinWidth = fullscreen ? 0 : 340;
        AppGrid.ColumnDefinitions[0].Width = new GridLength(fullscreen ? 0 : _savedChannelWidth);
        AppGrid.ColumnDefinitions[1].Width = new GridLength(fullscreen ? 0 : 4);

        var expanded = _viewModel.ShowWatchStage && (fullscreen || !_viewModel.IsChatVisible);
        WatchStage.Height = expanded ? double.NaN : 240;
        ContentGrid.RowDefinitions[1].Height =
            expanded ? new GridLength(1, GridUnitType.Star) : GridLength.Auto;
        ContentGrid.RowDefinitions[2].Height =
            expanded ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
    }

    private async void OnOpened(object? sender, EventArgs e)
    {
        AppLog.Info($"gui: shell window opened client={ClientSize.Width}x{ClientSize.Height} " +
                    $"scale={RenderScaling} theme={_viewModel.ThemeChoice}");

        // 窗口首次显示：先按目标屏工作区夹一次布局尺寸，再启动会触发重排的媒体泵。
        // Avalonia 的 Width/Height 是「布局尺寸」——工作区放不下时 Windows 只把 HWND 压小，
        // 根布局仍按原尺寸排版，右侧/底部内容被裁（小屏、高 DPI 副屏上必现）。
        ClampWindowToScreenWorkArea();
        if (!_screenClampHooked)
        {
            _screenClampHooked = true;
            Screens.Changed += OnScreensChanged;
        }
        _viewModel.StartMediaPump();

        if (!string.IsNullOrWhiteSpace(Program.Options.Token))
        {
            AppLog.Info("gui: auto-login with --token");
            await _viewModel.AutoLoginWithTokenAsync(Program.Options.Token);
            return;
        }

        // 没有命令行 token 时，尝试「记住我」：只有本机安全存过、且服务端仍认它才自动进。
        await _viewModel.TryRestoreRememberedSessionAsync();
    }

    // ------------------------------------------------------------------
    // 窗口尺寸夹到工作区（task-7）。只在「首次显示」与「显示器配置变化」时生效；
    // 不挂 LayoutUpdated/PositionChanged —— 那会在用户拖拽或普通布局回流时反复改尺寸（抖动）。
    // ------------------------------------------------------------------

    /// <summary>
    /// 把 <see cref="Window.Width"/> / <see cref="Window.Height"/> 夹到目标屏工作区能容纳的 DIP 尺寸。
    /// 低于 <see cref="Window.MinWidth"/> / <see cref="Window.MinHeight"/> 时仍以 Min 为准
    /// （此时裁切不可避免，配合 <see cref="AlignPositionToWorkArea"/> 至少保证标题栏与左栏可见）。
    /// </summary>
    private void ClampWindowToScreenWorkArea()
    {
        try
        {
            if (WindowState is WindowState.FullScreen or WindowState.Maximized)
            {
                return;
            }

            var screen = ResolveTargetScreen();
            if (screen is null)
            {
                return;
            }

            var scaling = screen.Scaling > 0 ? screen.Scaling : 1.0;
            var area = screen.WorkingArea;
            // WorkingArea 是物理像素且已扣掉任务栏；Width/Height 是客户区 DIP，
            // 还要留出边框/标题栏（不属客户区）的占位，否则夹完仍会被 OS 压掉一截。
            const double frameWidthDip = 16;
            const double frameHeightDip = 40;
            var maxWidthDip = area.Width / scaling - frameWidthDip;
            var maxHeightDip = area.Height / scaling - frameHeightDip;

            var requestedWidth = Width;
            var requestedHeight = Height;
            if (double.IsNaN(requestedWidth) || requestedWidth <= 0)
            {
                requestedWidth = MinWidth;
            }
            if (double.IsNaN(requestedHeight) || requestedHeight <= 0)
            {
                requestedHeight = MinHeight;
            }

            var targetWidth = Math.Max(MinWidth, Math.Min(requestedWidth, maxWidthDip));
            var targetHeight = Math.Max(MinHeight, Math.Min(requestedHeight, maxHeightDip));

            if (Math.Abs(targetWidth - Width) > 0.5 || Math.Abs(targetHeight - Height) > 0.5)
            {
                Width = targetWidth;
                Height = targetHeight;
                AppLog.Info($"gui: window clamped to workarea {targetWidth:0}x{targetHeight:0} dip " +
                            $"(requested {requestedWidth:0}x{requestedHeight:0}, screen={DescribeScreen(screen)}, " +
                            $"workarea={area.Width}x{area.Height}px @{scaling:0.##}x)");
                AlignPositionToWorkArea(screen);
                // 等布局跑完再把真实客户区尺寸打出来：这一行是「布局尺寸 == 实际客户区」的直接证据。
                // 旧行为是布局仍按 800 高排版、HWND 已被 OS 压到约 718，所以底部卡片被裁。
                Dispatcher.UIThread.Post(
                    () => AppLog.Info($"gui: window clamped client={ClientSize.Width:0}x{ClientSize.Height:0} dip " +
                                      $"(layout={Width:0}x{Height:0}, screen={DescribeScreen(screen)})"),
                    DispatcherPriority.Loaded);
            }
            else
            {
                AppLog.Info($"gui: window size fits workarea {targetWidth:0}x{targetHeight:0} dip " +
                            $"(requested {requestedWidth:0}x{requestedHeight:0}, screen={DescribeScreen(screen)}, " +
                            $"workarea={area.Width}x{area.Height}px @{scaling:0.##}x)");
            }
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: clamping window to workarea failed: " + ex.Message);
        }
    }

    /// <summary>显示器配置变化（分辨率/DPI/任务栏）后重新夹一次；Post 让当前布局先跑完再改尺寸。</summary>
    private void OnScreensChanged(object? sender, EventArgs e)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (!IsVisible || WindowState is WindowState.FullScreen or WindowState.Maximized)
            {
                return;
            }
            ClampWindowToScreenWorkArea();
        }, DispatcherPriority.Background);
    }

    /// <summary>
    /// 目标屏：<c>--screen</c> 优先（与 <see cref="RestoreWindowPlacement"/> 的语义一致），
    /// 否则取与窗口矩形重叠面积最大的屏，再退回主屏。
    /// </summary>
    private Avalonia.Platform.Screen? ResolveTargetScreen()
    {
        var screens = Screens.All;
        if (screens.Count == 0)
        {
            return null;
        }
        if (Program.Options.WindowScreen is int screenNumber && screenNumber >= 1 && screenNumber <= screens.Count)
        {
            return screens[screenNumber - 1];
        }

        var scaling = RenderScaling > 0 ? RenderScaling : 1.0;
        var widthPx = (int)Math.Round(ClientSize.Width * scaling);
        var heightPx = (int)Math.Round(ClientSize.Height * scaling);
        if (widthPx <= 0 || heightPx <= 0)
        {
            widthPx = (int)Math.Round(Width * scaling);
            heightPx = (int)Math.Round(Height * scaling);
        }

        Avalonia.Platform.Screen? best = null;
        long bestOverlap = 0;
        foreach (var screen in screens)
        {
            var area = screen.WorkingArea;
            var overlapX = Math.Min(Position.X + widthPx, area.Right) - Math.Max(Position.X, area.X);
            var overlapY = Math.Min(Position.Y + heightPx, area.Bottom) - Math.Max(Position.Y, area.Y);
            if (overlapX <= 0 || overlapY <= 0)
            {
                continue;
            }
            var overlap = (long)overlapX * overlapY;
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = screen;
            }
        }
        return best ?? Screens.Primary ?? screens[0];
    }

    /// <summary>
    /// 夹完尺寸后把窗口挪回工作区内：装得下就夹到边界内；装不下（工作区小于 Min 尺寸）则贴左上，
    /// 保证标题栏/左栏可见、只切右侧/底部，而不是两头都被切掉。
    /// </summary>
    private void AlignPositionToWorkArea(Avalonia.Platform.Screen screen)
    {
        var scaling = screen.Scaling > 0 ? screen.Scaling : 1.0;
        var area = screen.WorkingArea;
        var widthPx = (int)Math.Round(Width * scaling);
        var heightPx = (int)Math.Round(Height * scaling);

        var x = widthPx >= area.Width ? area.X : Math.Clamp(Position.X, area.X, area.Right - widthPx);
        var y = heightPx >= area.Height ? area.Y : Math.Clamp(Position.Y, area.Y, area.Bottom - heightPx);
        if (x == Position.X && y == Position.Y)
        {
            return;
        }

        Position = new PixelPoint(x, y);
        AppLog.Info($"gui: window repositioned to {x},{y} after clamp (screen={DescribeScreen(screen)})");
    }

    /// <summary>日志里的屏幕标识：Windows 上 DisplayName 常为 null，退回 <c>#序号</c>（1 基，与 --screen 一致）。</summary>
    private string DescribeScreen(Avalonia.Platform.Screen screen)
    {
        if (!string.IsNullOrWhiteSpace(screen.DisplayName))
        {
            return screen.DisplayName;
        }

        var screens = Screens.All;
        for (var i = 0; i < screens.Count; i++)
        {
            if (ReferenceEquals(screens[i], screen))
            {
                return "#" + (i + 1);
            }
        }
        return screen.IsPrimary ? "primary" : "?";
    }

    /// <summary>
    /// 点窗口关闭（X）的处理：
    /// <list type="bullet">
    /// <item>已请求真退出 / 系统注销关机 / 生命周期关闭 → 直接放行（托盘「退出」、系统关机不弹框）。</item>
    /// <item>设置里已明确选择过 → 按选择直接执行（tray=隐藏，exit=关门退出），不弹框。</item>
    /// <item>否则先把本次关闭取消，弹询问框：最小化到托盘 / 退出 PartyUp / 记住我的选择。</item>
    /// </list>
    /// 「退出」分支会先置位 <see cref="ExitRequested"/> 再重新 Close()，否则会被本方法再次拦下。
    /// </summary>
    private void OnClosing(object? sender, WindowClosingEventArgs e)
    {
        // 位置/尺寸在任何关闭路径上都要记（包括「关闭到托盘」——下次从托盘显示时仍应对齐）。
        SaveWindowPlacement();

        if (ExitRequested)
        {
            return;
        }
        if (e.CloseReason is WindowCloseReason.OSShutdown or WindowCloseReason.ApplicationShutdown)
        {
            return;
        }

        switch (AppSettings.Instance.CloseAction)
        {
            case AppSettings.CloseActionTray:
                e.Cancel = true;
                Hide();
                AppLog.Info("gui: close to tray (remembered)");
                return;
            case AppSettings.CloseActionExit:
                AppLog.Info("gui: close -> exit (remembered)");
                return;
        }

        // 未决定（ask）：本次关闭先取消，从调用栈里退出去再弹模态框，
        // 避免在 Closing 事件内部走 ShowDialog 的重入问题。
        e.Cancel = true;
        if (_closePromptOpen)
        {
            return;
        }
        _closePromptOpen = true;
        Dispatcher.UIThread.Post(async () =>
        {
            try
            {
                await PromptCloseActionAsync();
            }
            finally
            {
                _closePromptOpen = false;
            }
        });
    }

    /// <summary>弹「关闭到托盘 / 退出」询问框并执行用户选择（取消则保持窗口不动）。</summary>
    private async Task PromptCloseActionAsync()
    {
        var dialog = new CloseToTrayDialog();
        var result = await dialog.ShowDialog<string>(this);
        if (result == CloseToTrayDialog.ResultExit)
        {
            if (dialog.RememberChoice)
            {
                SetCloseAction(AppSettings.CloseActionExit);
            }
            RequestExit();
            Close();
            return;
        }
        if (result == CloseToTrayDialog.ResultTray)
        {
            if (dialog.RememberChoice)
            {
                SetCloseAction(AppSettings.CloseActionTray);
            }
            Hide();
            AppLog.Info("gui: close to tray (asked)");
            return;
        }
        AppLog.Info("gui: close cancelled by user");
    }

    /// <summary>记住用户的选择：写设置并通知设置页刷新下拉（不重启也显示最新值）。</summary>
    private void SetCloseAction(string action)
    {
        var settings = AppSettings.Instance;
        settings.CloseAction = action;
        settings.Save();
        AppLog.Info($"gui: close action remembered as {action}");
    }

    private async void OnClosed(object? sender, EventArgs e)
    {
        AppLog.Info("gui: shell window closed");
        try
        {
            // 有界等待：清理（停泵/停共享/断连）最多 3 秒，窗口关闭不能被它无限拖住。
            await _viewModel.ShutdownAsync().WaitAsync(TimeSpan.FromSeconds(3));
            AppLog.Info("gui: shutdown complete");
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: shutdown incomplete: " + ex.Message);
        }
    }

    /// <summary>双击频道条目 = 浏览该频道并显式加入其语音（CR-02）。</summary>
    private async void ChannelDoubleTapped(object? sender, TappedEventArgs e)
    {
        // 选中态可能把指针捕获/重定向到 ListBox 本身：用框架命中测试，而不是只看 SelectedItem。
        var hit = ChannelList.InputHitTest(e.GetPosition(ChannelList)) as Control;
        var item = hit as ListBoxItem ?? hit?.GetVisualAncestors().OfType<ListBoxItem>().FirstOrDefault();
        if (item?.DataContext is not ShellChannelItem channel)
        {
            return;
        }
        AppLog.Info($"gui: channel double-tap {channel.Name}");
        // 浏览交给 SelectedItem 变化（避免同一次双击触发两条 browse 往返）；
        // 只有当前未浏览该频道时才补一次浏览，之后显式加入语音。
        if (_viewModel.BrowseChannel?.Id != channel.Id)
        {
            await _viewModel.BrowseChannelAsync(channel, alsoJoinVoice: false);
        }
        await _viewModel.JoinVoiceAsync(channel.Id);
        e.Handled = true;
    }

    private void OnChatScrollChanged(object? sender, ScrollChangedEventArgs e)
    {
        // 只有用户停在底部时才自动跟随；读旧消息时不强制滚底（Q14）。
        var atEnd = ChatHistory.Offset.Y >= ChatHistory.Extent.Height - ChatHistory.Viewport.Height - 4;
        _viewModel.NotifyChatAtEnd(atEnd);
    }

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && _viewModel.IsFullScreen)
        {
            _viewModel.ToggleFullScreenCommand.Execute(null);
            e.Handled = true;
        }
    }

    /// <summary>窗口图标来自与打包相同的 assets/icon.ico（作为 AvaloniaResource 内嵌）。</summary>
    public void ApplyIcon()
    {
        try
        {
            using var stream = Avalonia.Platform.AssetLoader.Open(
                new Uri("avares://PartyUp.Client/Assets/icon.ico"));
            Icon = new WindowIcon(stream);
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: 窗口图标加载失败：" + ex.Message);
        }
    }
}
