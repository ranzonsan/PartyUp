using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using PartyUp.Client.Services;
using PartyUp.Client.ViewModels;

namespace PartyUp.Client;

public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel = null!;
    private readonly DispatcherTimer _mediaTimer = null!;

    /// <summary>XAML 加载器要求的无参构造（设计器/热重载用）。</summary>
    public MainWindow() : this(new MainViewModel())
    {
    }

    public MainWindow(MainViewModel viewModel)
    {
        InitializeComponent();
        _viewModel = viewModel;
        DataContext = viewModel;

        LoginButton.Click += OnLoginClicked;
        RegisterButton.Click += OnRegisterClicked;
        CreateChannelButton.Click += OnCreateChannelClicked;
        SendButton.Click += OnSendClicked;
        MessageInput.KeyDown += OnMessageInputKeyDown;
        ChannelList.SelectionChanged += OnChannelSelectionChanged;
        _viewModel.Messages.CollectionChanged += (_, _) => MessageScroll.ScrollToEnd();

        // 媒体事件是轮询式的（pu_poll_event）；用 100ms 的 UI 定时器 drain，开销可忽略。
        _mediaTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _mediaTimer.Tick += (_, _) => _viewModel.PollMedia();
        _mediaTimer.Start();

        Opened += OnOpened;
        Closed += OnClosed;
    }

    private async void OnOpened(object? sender, EventArgs e)
    {
        AppLog.Info("gui: window opened");
        if (string.IsNullOrWhiteSpace(Program.Options.Token))
        {
            return;
        }
        // --token：自动登录（用于一键脚本和无头取证，不需要手点）。
        AppLog.Info("gui: auto-login with --token");
        await _viewModel.AutoLoginAsync(Program.Options.Token);
    }

    private void OnClosed(object? sender, EventArgs e) => _mediaTimer.Stop();

    private async void OnLoginClicked(object? sender, RoutedEventArgs e) => await _viewModel.LoginAsync();

    private async void OnRegisterClicked(object? sender, RoutedEventArgs e) => await _viewModel.RegisterAsync();

    private async void OnCreateChannelClicked(object? sender, RoutedEventArgs e) =>
        await _viewModel.CreateChannelAsync();

    private async void OnSendClicked(object? sender, RoutedEventArgs e) => await _viewModel.SendMessageAsync();

    private async void OnMessageInputKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter)
        {
            return;
        }
        e.Handled = true;
        await _viewModel.SendMessageAsync();
    }

    private async void OnChannelSelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (_viewModel.SelectedChannel is null)
        {
            return;
        }
        await _viewModel.OpenChannelAsync(_viewModel.SelectedChannel);
    }
}
