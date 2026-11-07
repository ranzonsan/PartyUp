using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Text.Json;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Material.Icons;

namespace PartyUp.UiPrototype;

/// <summary>Presentation-only adapter around the existing in-memory demo state.</summary>
public partial class SemiViewModel : ObservableObject
{
    public DemoState Session { get; }
    public RegistrationFlow Registration { get; }
    public ObservableCollection<PreviewMember> CurrentMembers { get; } = [];
    public ObservableCollection<PreviewMember> OtherOnlineMembers { get; } = [];
    public ObservableCollection<PreviewMember> OfflineMembers { get; } = [];
    [ObservableProperty] private bool channelPickerOpen = true;
    [ObservableProperty] private string displayName = "演示玩家";
    [ObservableProperty] private bool selfSpeaking;
    private bool _aranShareEnded;

    public bool ShowLogin => !Session.IsLoggedIn && !Registration.IsActive;
    public bool ShowRegistration => !Session.IsLoggedIn && Registration.IsActive;
    public bool ShowWatchStage => Session.IsWatching || Session.IsWaitingForFrame;
    public bool ShowChat => Session.IsChatVisible && !Session.IsFullScreen;
    public string CurrentGroupTitle => $"浏览频道语音 · {CurrentMembers.Count}";
    public string OnlineGroupTitle => $"其他在线 · {OtherOnlineMembers.Count}";
    public string OfflineGroupTitle => $"离线 · {OfflineMembers.Count}";
    public string SafeInspector => Registration.SafeState + "\n" + Session.InspectorText;

    public SemiViewModel(DemoState session)
    {
        Session = session;
        Registration = new RegistrationFlow((server, username, name) =>
        {
            Session.ServerAddress = server;
            Session.Username = username;
            Session.Password = string.Empty;
            DisplayName = name;
        });
        Session.PropertyChanged += SessionChanged;
        Session.Shares.CollectionChanged += (_, _) => RefreshMembers();
        Registration.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(RegistrationFlow.IsActive)) NotifyViews();
            if (e.PropertyName == nameof(RegistrationFlow.SafeState)) OnPropertyChanged(nameof(SafeInspector));
        };
        RefreshMembers();
    }

    [RelayCommand]
    private void ChooseChannel() => ChannelPickerOpen = true;

    [RelayCommand]
    private void ShowMembers() => ChannelPickerOpen = false;

    [RelayCommand]
    private void JoinChannel(string? channel)
    {
        if (string.IsNullOrWhiteSpace(channel) || !Session.Channels.Contains(channel)) return;
        Session.BrowseChannel = channel;
        if (Session.CallChannel == channel && Session.IsCallActive)
        {
            ChannelPickerOpen = false;
            return;
        }
        Session.JoinCommand.Execute(null);
        if (Session.CallChannel == channel && Session.IsCallActive) ChannelPickerOpen = false;
    }

    [RelayCommand]
    private void OpenSettings() => Session.OpenSettingsCommand.Execute(null);

    [RelayCommand]
    private void CloseSettings() => Session.CloseSettingsCommand.Execute(null);

    [RelayCommand]
    private void ToggleTheme() => Session.ThemeChoice = Session.ThemeChoice == "深色" ? "浅色" : "深色";

    [RelayCommand]
    private void SimulateSpeech()
    {
        SelfSpeaking = !SelfSpeaking && Session.IsCallActive && !Session.IsMuted;
        RefreshMembers();
    }

    [RelayCommand]
    private void EndAranShare()
    {
        _aranShareEnded = true;
        if (Session.SelectedShare == "阿岚 · 桌面") Session.EndShareCommand.Execute(null);
        RefreshMembers();
    }

    partial void OnDisplayNameChanged(string value) => RefreshMembers();

    private void SessionChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(DemoState.IsLoggedIn))
        {
            ChannelPickerOpen = true;
            if (!Session.IsLoggedIn) Registration.CancelCommand.Execute(null);
            NotifyViews();
        }
        if (e.PropertyName == nameof(DemoState.CallChannel) &&
            Session.CallChannel == Session.BrowseChannel && Session.IsCallActive)
            ChannelPickerOpen = false;
        if (e.PropertyName is nameof(DemoState.IsWatching) or nameof(DemoState.IsWaitingForFrame))
        {
            OnPropertyChanged(nameof(ShowWatchStage));
            if (!ShowWatchStage) Session.IsChatVisible = true;
        }
        if (e.PropertyName is nameof(DemoState.IsChatVisible) or nameof(DemoState.IsFullScreen))
            OnPropertyChanged(nameof(ShowChat));
        if (e.PropertyName is nameof(DemoState.BrowseChannel) or nameof(DemoState.CallChannel) or
            nameof(DemoState.IsMuted) or nameof(DemoState.IsDeafened) or nameof(DemoState.IsPublishing) or
            nameof(DemoState.Username) or nameof(DemoState.IsLoggedIn) or nameof(DemoState.IsConnected) or nameof(DemoState.IsTransferPending))
        {
            if (Session.IsMuted || !Session.IsCallActive) SelfSpeaking = false;
            RefreshMembers();
        }
        if (e.PropertyName == nameof(DemoState.InspectorText)) OnPropertyChanged(nameof(SafeInspector));
    }

    private void NotifyViews()
    {
        OnPropertyChanged(nameof(ShowLogin));
        OnPropertyChanged(nameof(ShowRegistration));
        OnPropertyChanged(nameof(SafeInspector));
    }

    private void RefreshMembers()
    {
        // Seeded presence and media indicators are explicit synthetic data, not device measurements.
        var people = new List<PreviewMember>
        {
            new("阿岚", "开黑大厅", true, true, true, true, !_aranShareEnded, "阿岚 · 桌面", Session.WatchCommand),
            new("小满", "开黑大厅", true, false, false, false, true, "小满 · 窗口", Session.WatchCommand),
            new("青木", "", true, false, false, true, false, "", Session.WatchCommand),
            new("可乐", "攻略与杂谈", true, true, false, true, false, "", Session.WatchCommand),
            new("老陈", "", false, false, false, false, false, "", Session.WatchCommand),
            new("星河", "", false, false, false, false, false, "", Session.WatchCommand),
        };
        if (Session.IsLoggedIn)
            people.Insert(0, new PreviewMember(DisplayName + "（你）", Session.CallChannel,
                true, !Session.IsMuted, SelfSpeaking, !Session.IsDeafened, Session.IsPublishing,
                "", Session.WatchCommand, true));

        CurrentMembers.Clear();
        OtherOnlineMembers.Clear();
        OfflineMembers.Clear();
        foreach (var person in people)
        {
            person.WatchAllowed = Session.IsCallActive && Session.IsConnected && !Session.IsTransferPending &&
                person.Channel == Session.CallChannel && Session.Shares.Contains(person.ShareSource);
            if (!person.Online) OfflineMembers.Add(person);
            else if (person.Channel == Session.BrowseChannel && person.VoiceJoined) CurrentMembers.Add(person);
            else OtherOnlineMembers.Add(person);
        }
        OnPropertyChanged(nameof(CurrentGroupTitle));
        OnPropertyChanged(nameof(OnlineGroupTitle));
        OnPropertyChanged(nameof(OfflineGroupTitle));
    }
}

public sealed class PreviewMember
{
    public PreviewMember(string name, string channel, bool online, bool microphone, bool speaking,
        bool speakers, bool sharing, string shareSource, ICommand watch, bool self = false, bool watchAllowed = false)
    {
        Name = name; Channel = channel; Online = online; MicrophoneEnabled = microphone;
        IsSpeaking = speaking && microphone && online && !string.IsNullOrEmpty(channel);
        SpeakersEnabled = speakers; IsSharing = sharing; ShareSource = shareSource;
        WatchCommand = watch; IsSelf = self; WatchAllowed = watchAllowed;
    }
    public string Name { get; }
    public string Channel { get; }
    public bool Online { get; }
    public bool IsSelf { get; }
    public bool VoiceJoined => Online && !string.IsNullOrEmpty(Channel);
    public bool MicrophoneEnabled { get; }
    public bool IsSpeaking { get; }
    public bool SpeakersEnabled { get; }
    public bool IsSharing { get; }
    public bool WatchAllowed { get; internal set; }
    public bool ShowWatchButton => IsSharing && !IsSelf;
    public bool CanWatch => ShowWatchButton && WatchAllowed;
    public string ShareSource { get; }
    public ICommand WatchCommand { get; }
    public MaterialIconKind MicrophoneIcon => MicrophoneEnabled ? MaterialIconKind.Microphone : MaterialIconKind.MicrophoneOff;
    public MaterialIconKind SpeakerIcon => SpeakersEnabled ? MaterialIconKind.VolumeHigh : MaterialIconKind.VolumeOff;
    public string PresenceLabel => !Online ? "离线" : string.IsNullOrEmpty(Channel) ? "空闲" : "在 " + Channel;
    public string MicrophoneLabel => MicrophoneEnabled ? "麦克风已开启" : "麦克风已关闭";
    public string SpeakingLabel => IsSpeaking ? "麦克风当前有声音（模拟）" : "麦克风当前无声音（模拟）";
    public string SpeakerLabel => SpeakersEnabled ? "扬声器已开启" : "扬声器已关闭";
    public string ShareLabel => IsSharing ? "正在共享屏幕（模拟）" : "未共享屏幕";
    public string ActivityLabel => IsSpeaking ? "正在说话（模拟）" : "未在说话";
}
