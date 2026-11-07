using CommunityToolkit.Mvvm.ComponentModel;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 左栏频道列表的一行。频道本身仍是统一的三区频道（文字/语音/共享），
/// 这里只标记「当前浏览」与「当前通话」两个**互不等价**的归属：
/// 单击频道只改 <see cref="IsBrowsing"/>，双击才可能改 <see cref="IsCallChannel"/>。
/// </summary>
public sealed partial class ShellChannelItem : ObservableObject
{
    public ShellChannelItem(string id, string name)
    {
        Id = id;
        Name = name;
    }

    public string Id { get; }

    public string Name { get; }

    /// <summary>当前文字浏览频道（决定读消息与草稿）。</summary>
    [ObservableProperty]
    private bool _isBrowsing;

    /// <summary>当前语音/共享归属频道（与浏览频道可以不同）。</summary>
    [ObservableProperty]
    private bool _isCallChannel;

    public string NameWithMarkers =>
        IsCallChannel ? $"{Name} · 通话中" : Name;

    partial void OnIsCallChannelChanged(bool value) => OnPropertyChanged(nameof(NameWithMarkers));
}
