using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Windows.Input;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Styling;
using Avalonia.Threading;

namespace PartyUp.UiPrototype;

/// <summary>
/// Throwaway interaction prototype window.
/// Code-behind is limited to: DataContext construction for the mock state,
/// visual theme / font size / structural layout handling, and Enter / Escape
/// command forwarding. All business transitions live in <see cref="DemoState"/>.
/// </summary>
public partial class PrototypeWindow : Window
{
    // UI-only option lists for the demo toolbar and mock settings rows.
    public static IReadOnlyList<string> VariantOptions { get; } = new[] { "A", "B", "C" };

    public static IReadOnlyList<string> ThemeOptions { get; } = new[] { "深色", "浅色", "跟随系统" };

    public static IReadOnlyList<string> TextSizeOptions { get; } = new[] { "标准", "大号" };

    public static IReadOnlyList<string> InputDeviceOptions { get; } =
        new[] { "跟随系统默认（模拟）", "演示麦克风 · 耳机（模拟）" };

    public static IReadOnlyList<string> OutputDeviceOptions { get; } =
        new[] { "跟随系统默认（模拟）", "演示扬声器 · 头戴式（模拟）" };

    public static IReadOnlyList<string> InputModeOptions { get; } = new[] { "自由说话", "按住说话" };

    private DemoState? _state;
    private IDataTemplate? _chatScreen;
    private IDataTemplate? _watchScreen;
    private IDataTemplate? _settingsScreen;
    private IDataTemplate? _callStageScreen;
    private IDataTemplate? _activeStageTemplate;

    public PrototypeWindow()
    {
        InitializeComponent();

        _chatScreen = this.FindResource("ChatScreenTemplate") as IDataTemplate;
        _watchScreen = this.FindResource("WatchScreenTemplate") as IDataTemplate;
        _settingsScreen = this.FindResource("SettingsScreenTemplate") as IDataTemplate;
        _callStageScreen = this.FindResource("CallStageScreenTemplate") as IDataTemplate;

        var state = new DemoState();
        _state = state;
        DataContext = state;
        state.PropertyChanged += OnStatePropertyChanged;

        ApplyTheme(state.ThemeChoice);
        ApplyTextSize(state.TextSizeChoice);
        ApplyLayout();

        AddHandler(KeyDownEvent, OnPreviewKeyDown, RoutingStrategies.Tunnel);
        SizeChanged += (_, _) => ApplyLayout();
    }

    private void OnStatePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_state is null)
            return;

        // A mock awaited action can complete off the UI thread; theme, font and
        // layout updates must stay on the UI thread.
        if (!Dispatcher.UIThread.CheckAccess())
        {
            Dispatcher.UIThread.Post(() => OnStatePropertyChanged(sender, e));
            return;
        }

        switch (e.PropertyName)
        {
            case nameof(DemoState.ThemeChoice):
                ApplyTheme(_state.ThemeChoice);
                break;
            case nameof(DemoState.TextSizeChoice):
                ApplyTextSize(_state.TextSizeChoice);
                break;
            case nameof(DemoState.Variant):
            case nameof(DemoState.IsLoggedIn):
            case nameof(DemoState.IsWatching):
            case nameof(DemoState.IsWaitingForFrame):
            case nameof(DemoState.ShowSettings):
            case nameof(DemoState.IsFullScreen):
                ApplyLayout();
                break;
        }
    }

    private void ApplyTheme(string? choice)
    {
        var variant = choice switch
        {
            "浅色" => ThemeVariant.Light,
            "跟随系统" => ThemeVariant.Default,
            _ => ThemeVariant.Dark,
        };

        RequestedThemeVariant = variant;

        if (Application.Current is { } application)
            application.RequestedThemeVariant = variant;
    }

    private void ApplyTextSize(string? choice)
    {
        var large = string.Equals(choice, "大号", StringComparison.Ordinal);

        Classes.Remove("prototypeLargeText");
        if (large)
            Classes.Add("prototypeLargeText");
    }

    private void ApplyLayout()
    {
        if (_state is null)
            return;

        var state = _state;

        // Login and app visibility are bound in XAML; only the structural
        // variant shells and the panel widths are handled here.
        var loggedIn = state.IsLoggedIn;
        // Two-way ComboBox initialization can briefly supply a null variant;
        // fall back to A so the prototype never renders an empty shell.
        var variant = string.IsNullOrEmpty(state.Variant) ? "A" : state.Variant;
        var variantA = loggedIn && string.Equals(variant, "A", StringComparison.Ordinal);
        var variantB = loggedIn && string.Equals(variant, "B", StringComparison.Ordinal);
        var variantC = loggedIn && string.Equals(variant, "C", StringComparison.Ordinal);

        ShellA.IsVisible = variantA;
        ShellB.IsVisible = variantB;
        ShellC.IsVisible = variantC;

        var stageTemplate = state.ShowSettings
            ? _settingsScreen
            : state.IsWatching || state.IsWaitingForFrame
                ? _watchScreen
                : variantC
                    ? _callStageScreen
                    : _chatScreen;

        if (!ReferenceEquals(_activeStageTemplate, stageTemplate))
        {
            StageA.ContentTemplate = stageTemplate;
            StageB.ContentTemplate = stageTemplate;
            StageC.ContentTemplate = stageTemplate;
            _activeStageTemplate = stageTemplate;
        }

        var width = ClientSize.Width > 0 ? ClientSize.Width : Width;
        var fullscreen = state.IsFullScreen;
        var chatScreen = !state.ShowSettings && !state.IsWatching && !state.IsWaitingForFrame;

        var railWidth = width < 1000 ? 196d : 232d;
        var sideWidth = width < 1000 ? 0d : width < 1180 ? 240d : 288d;

        // A: channel rail + stage + info side + bottom call bar.
        ShellA.ColumnDefinitions[0].Width = new GridLength(fullscreen ? 0 : railWidth);
        ShellA.ColumnDefinitions[2].Width =
            new GridLength(chatScreen && !fullscreen && sideWidth > 0 ? sideWidth : 0);

        // B: compact top channel navigation + wide stage + bottom call bar.
        ShellB.ColumnDefinitions[1].Width =
            new GridLength(chatScreen && !fullscreen && sideWidth > 0 ? sideWidth : 0);

        // C: call-stage-first main area + text side + bottom call bar.
        var callStageSide = chatScreen && !fullscreen ? (width < 1000 ? 320d : 340d) : 0d;
        ShellC.ColumnDefinitions[1].Width = new GridLength(callStageSide);

        NavRailA.IsVisible = !fullscreen;
        TopNavB.IsVisible = !fullscreen;
        TopNavC.IsVisible = !fullscreen;

        SideA.IsVisible = chatScreen && !fullscreen && sideWidth > 0;
        SideB.IsVisible = chatScreen && !fullscreen && sideWidth > 0;
        SideC.IsVisible = chatScreen && !fullscreen;

        InspectorHost.Width = width < 1100 ? 340d : 400d;
    }

    private void OnPreviewKeyDown(object? sender, KeyEventArgs e)
    {
        if (_state is null)
            return;

        if (e.Key == Key.Escape)
        {
            if (_state.IsFullScreen && _state.ToggleFullscreenCommand.CanExecute(null))
            {
                _state.ToggleFullscreenCommand.Execute(null);
                e.Handled = true;
                return;
            }

            if (_state.ShowSettings && _state.CloseSettingsCommand.CanExecute(null))
            {
                _state.CloseSettingsCommand.Execute(null);
                e.Handled = true;
                return;
            }

            if (_state.IsTransferPending && _state.CancelTransferCommand.CanExecute(null))
            {
                _state.CancelTransferCommand.Execute(null);
                e.Handled = true;
                return;
            }

            if (_state.ShowInspector)
            {
                _state.ShowInspector = false;
                e.Handled = true;
            }

            return;
        }

        if (e.Key != Key.Enter)
            return;

        if (!_state.IsLoggedIn)
        {
            ExecuteIfPossible(_state.LoginCommand);
            e.Handled = true;
            return;
        }

        if (e.Source is TextBox textBox &&
            (string.Equals(textBox.Name, "MessageInput", StringComparison.Ordinal) ||
             string.Equals(textBox.Name, "SideMessageInput", StringComparison.Ordinal) ||
             string.Equals(textBox.Name, "WatchMessageInput", StringComparison.Ordinal)))
        {
            if (ExecuteIfPossible(_state.SendCommand))
                e.Handled = true;
        }
    }

    private static bool ExecuteIfPossible(ICommand command)
    {
        if (!command.CanExecute(null))
            return false;

        command.Execute(null);
        return true;
    }
}
