using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using Material.Icons;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 成员列表的一行（全服名册 MEM-01/02）。
///
/// 数据口径：
///   * 在线/离线、语音归属来自服务端 <c>member.list</c> / <c>members.state</c>；
///   * 麦克风/扬声器开关来自 <c>voice.state</c>（服务端权威），不从本地猜测；
///   * 「正在说话」只在本端有该成员的音频电平证据时点亮（当前仅通话频道有），
///     没有信号时**不伪造**状态。
/// </summary>
public sealed partial class ShellMemberRow : ObservableObject
{
    public ShellMemberRow(string id, string displayName, ICommand? watchCommand)
    {
        Id = id;
        DisplayName = displayName;
        WatchCommand = watchCommand;
    }

    /// <summary>身份键：任何去重/归属/命令参数都用它，不用显示名。</summary>
    public string Id { get; }

    public string DisplayName { get; }

    public bool IsSelf { get; init; }

    public bool Admin { get; init; }

    /// <summary>服务端名册里的语音频道集合（可能多个）。</summary>
    public IReadOnlyList<string> VoiceChannelIds { get; init; } = Array.Empty<string>();

    /// <summary>该成员当前所在的语音频道名（多个用「、」连接）；空 = 未入语音。</summary>
    [ObservableProperty]
    private string _voiceChannelLabel = string.Empty;

    [ObservableProperty]
    private bool _isOnline;

    [ObservableProperty]
    private bool _isInVoice;

    [ObservableProperty]
    private bool _microphoneEnabled = true;

    [ObservableProperty]
    private bool _isMuted;

    [ObservableProperty]
    private bool _isDeafened;

    /// <summary>本端有真实电平证据且超过阈值/保持窗。没有证据一律 false。</summary>
    [ObservableProperty]
    private bool _isSpeaking;

    [ObservableProperty]
    private bool _isSharing;

    /// <summary>共享源标题（如「显示器 1」），用于观看按钮提示。</summary>
    [ObservableProperty]
    private string _sharingTitle = string.Empty;

    /// <summary>观看入口可点：共享者在本端通话频道里，且本端已在该频道语音中。</summary>
    [ObservableProperty]
    private bool _canWatch;

    /// <summary>该成员所在频道内是否有本端可观看的共享（决定按钮是否出现）。</summary>
    [ObservableProperty]
    private bool _showWatchButton;

    /// <summary>宿主提供的观看命令（参数是本行自身）。</summary>
    public ICommand? WatchCommand { get; }

    /// <summary>「空闲」= 在线但没在任何频道语音里（不是键盘无活动）。</summary>
    public string PresenceLabel =>
        !IsOnline ? "离线" : IsInVoice ? "在语音" : "空闲";

    public MaterialIconKind MicrophoneIcon =>
        MicrophoneEnabled ? MaterialIconKind.Microphone : MaterialIconKind.MicrophoneOff;

    public MaterialIconKind SpeakerIcon =>
        IsDeafened ? MaterialIconKind.VolumeOff : MaterialIconKind.VolumeHigh;

    public string MicrophoneLabel => IsMuted ? "麦克风已静音（服务端状态）" : "麦克风已开启（服务端状态）";

    public string SpeakerLabel => IsDeafened ? "扬声器已关闭（服务端状态）" : "扬声器已开启（服务端状态）";

    public string SpeakingLabel => IsSpeaking ? "当前有声音（本端电平证据）" : "无发声电平证据";

    public string ShareLabel => IsSharing ? $"正在共享：{SharingTitle}" : "未共享屏幕";

    public string WatchLabel => CanWatch ? "观看共享" : "需先加入该频道语音";

    partial void OnIsOnlineChanged(bool value) => OnPropertyChanged(nameof(PresenceLabel));

    partial void OnIsInVoiceChanged(bool value) => OnPropertyChanged(nameof(PresenceLabel));

    partial void OnMicrophoneEnabledChanged(bool value)
    {
        OnPropertyChanged(nameof(MicrophoneIcon));
        OnPropertyChanged(nameof(MicrophoneLabel));
    }

    partial void OnIsMutedChanged(bool value) => OnPropertyChanged(nameof(MicrophoneLabel));

    partial void OnIsDeafenedChanged(bool value)
    {
        OnPropertyChanged(nameof(SpeakerIcon));
        OnPropertyChanged(nameof(SpeakerLabel));
    }

    partial void OnIsSpeakingChanged(bool value) => OnPropertyChanged(nameof(SpeakingLabel));

    partial void OnIsSharingChanged(bool value) => OnPropertyChanged(nameof(ShareLabel));

    partial void OnSharingTitleChanged(string value) => OnPropertyChanged(nameof(ShareLabel));

    partial void OnCanWatchChanged(bool value) => OnPropertyChanged(nameof(WatchLabel));
}
