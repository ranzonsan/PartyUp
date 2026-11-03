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

    /// <summary>是否处于静音状态（只有自己可靠：媒体核心本地 mute 不外发）。</summary>
    [ObservableProperty]
    private bool _isMuted;
}
