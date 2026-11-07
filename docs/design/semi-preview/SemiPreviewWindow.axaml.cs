using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Styling;
using Avalonia.VisualTree;

namespace PartyUp.UiPrototype;

public partial class SemiPreviewWindow : Window
{
    public static string[] ThemeChoices { get; } = ["浅色", "深色", "跟随系统"];
    public static string[] TextChoices { get; } = ["标准", "大号"];
    public static string[] DeviceChoices { get; } = ["跟随系统默认（模拟）", "头戴式耳机（模拟）"];
    public static string[] InputChoices { get; } = ["自由说话", "按住说话"];
    private readonly SemiViewModel _viewModel;
    private DemoState State => _viewModel.Session;

    public SemiPreviewWindow() : this(new SemiViewModel(new DemoState())) { }

    public SemiPreviewWindow(SemiViewModel viewModel)
    {
        _viewModel = viewModel;
        InitializeComponent();
        DataContext = viewModel;
        ApplyTheme();
        State.PropertyChanged += StateChanged;
        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Tunnel);
        Closed += (_, _) => State.PropertyChanged -= StateChanged;
    }

    private void ApplyTheme()
    {
        RequestedThemeVariant = State.ThemeChoice switch
        {
            "深色" => ThemeVariant.Dark,
            "跟随系统" => ThemeVariant.Default,
            _ => ThemeVariant.Light,
        };
        if (Application.Current is { } app) app.RequestedThemeVariant = RequestedThemeVariant;
    }

    private void StateChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(DemoState.ThemeChoice)) ApplyTheme();
        if (e.PropertyName == nameof(DemoState.TextSizeChoice)) FontSize = State.TextSizeChoice == "大号" ? 16 : 14;
        if (e.PropertyName == nameof(DemoState.IsFullScreen))
        {
            WindowState = State.IsFullScreen ? WindowState.FullScreen : WindowState.Normal;
            ChannelPane.IsVisible = !State.IsFullScreen;
            ChannelDivider.IsVisible = !State.IsFullScreen;
            AppGrid.ColumnDefinitions[0].Width = new GridLength(State.IsFullScreen ? 0 : 300);
            AppGrid.ColumnDefinitions[1].Width = new GridLength(State.IsFullScreen ? 0 : 4);
        }
        if (e.PropertyName is nameof(DemoState.IsFullScreen) or nameof(DemoState.IsChatVisible) or
            nameof(DemoState.IsWatching) or nameof(DemoState.IsWaitingForFrame))
        {
            var expanded = _viewModel.ShowWatchStage && (State.IsFullScreen || !State.IsChatVisible);
            WatchStage.Height = expanded ? double.NaN : 240;
            ContentGrid.RowDefinitions[1].Height = expanded ? new GridLength(1, GridUnitType.Star) : GridLength.Auto;
            ContentGrid.RowDefinitions[2].Height = expanded ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        }
    }

    private void JoinDoubleTapped(object? sender, TappedEventArgs e)
    {
        // Selection may capture/re-target the pointer to the ListBox itself.
        // Use the framework hit test at the gesture position, not SelectedItem alone.
        var hit = ChannelList.InputHitTest(e.GetPosition(ChannelList)) as Control;
        var item = hit as ListBoxItem ?? hit?.GetVisualAncestors().OfType<ListBoxItem>().FirstOrDefault();
        if (item?.Content is not string channel) return;
        Console.WriteLine("CHANNEL_DOUBLE_TAP targetRow=true");
        _viewModel.JoinChannelCommand.Execute(channel);
        e.Handled = true;
    }

    private void JumpLatest(object? sender, RoutedEventArgs e) => ChatHistory.ScrollToEnd();

    private void OnKey(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && State.IsFullScreen)
        {
            State.ToggleFullscreenCommand.Execute(null);
            e.Handled = true;
        }
    }
}
