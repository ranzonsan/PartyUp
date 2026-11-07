using System;
using Avalonia.Markup.Xaml;

namespace PartyUp.Client.Localization;

/// <summary>
/// XAML 里的文案引用：<c>Text="{loc:Tr Settings_Title}"</c>。
///
/// 它只是把 <see cref="L.Get"/> 搬到 XAML，本身不做解析。求值发生在 XAML 载入时，
/// 因此语言切换在重启后生效（与 <see cref="L"/> 的约定一致）。
/// </summary>
public sealed class TrExtension : MarkupExtension
{
    public TrExtension()
    {
    }

    /// <summary>位置参数写法：<c>Text="{loc:Tr Settings_Title}"</c> 时 Avalonia 按参数名匹配到 key。</summary>
    public TrExtension(string key) => Key = key;

    /// <summary>资源键，必须是 <c>Strings.resx</c> 里存在的键。</summary>
    public string Key { get; set; } = string.Empty;

    public override object ProvideValue(IServiceProvider serviceProvider) => L.Get(Key);
}
