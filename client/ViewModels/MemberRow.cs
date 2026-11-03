using CommunityToolkit.Mvvm.ComponentModel;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 在线成员列表的一行。服务端 presence 只给 member_id / name / state，
/// 语音标识（是否在语音、是否自己静音）由客户端 <c>ShareSession</c> 的语音状态补齐。
/// </summary>
public sealed partial class MemberRow : ObservableObject
{
    public MemberRow(string id, string name)
    {
        Id = id;
        Name = name;
    }

    public string Id { get; }

    public string Name { get; }

    /// <summary>该成员是否在语音（自己 + 从 media.signal voice 通道观察到的对端）。</summary>
    [ObservableProperty]
    private bool _isInVoice;

    /// <summary>是否处于静音状态（来自服务端 voice.state.muted）。</summary>
    [ObservableProperty]
    private bool _isMuted;

    /// <summary>「在语音」图标只在未静音时显示——静音时只留 🔇，避免两个图标并排。</summary>
    public bool IsSpeakingIconVisible => IsInVoice && !IsMuted;

    partial void OnIsInVoiceChanged(bool value) => OnPropertyChanged(nameof(IsSpeakingIconVisible));

    partial void OnIsMutedChanged(bool value) => OnPropertyChanged(nameof(IsSpeakingIconVisible));
}
