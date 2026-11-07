using System;
using System.Collections.Generic;
using System.Globalization;
using System.Resources;

namespace PartyUp.Client.Localization;

/// <summary>语言选项。Tag 是 BCP-47 标签；DisplayName 用各语言自己的写法（语言选择器里不该被翻译）。</summary>
public sealed record LanguageOption(string Tag, string DisplayName);

/// <summary>
/// 界面文案入口。
///
/// 资源用 .NET 标准 <c>.resx</c>：中性资源 <c>Strings.resx</c> 是简体中文，英文走
/// <c>Strings.en.resx</c> 卫星程序集，解析交给 <see cref="ResourceManager"/>，不手写任何资源格式解析。
///
/// 语言在**启动时**决定（<see cref="Apply"/> 由 Program.Main 在创建 Avalonia 之前调用）；
/// 运行期改语言需要重启，设置页如实标注这一点，不做"假装立刻生效"。
/// </summary>
public static class L
{
    /// <summary>「跟随系统」的存储值。</summary>
    public const string AutoTag = "auto";

    public const string ChineseTag = "zh-Hans";
    public const string EnglishTag = "en";

    private static readonly ResourceManager Resources =
        new("PartyUp.Client.Localization.Strings", typeof(L).Assembly);

    /// <summary>可选语言（含「跟随系统」）。选择器直接绑定这个列表。</summary>
    public static IReadOnlyList<LanguageOption> Languages { get; } = new[]
    {
        new LanguageOption(AutoTag, "跟随系统 / System"),
        new LanguageOption(ChineseTag, "简体中文"),
        new LanguageOption(EnglishTag, "English"),
    };

    /// <summary>当前生效的语言标签（解析「跟随系统」之后的值）。</summary>
    public static string CurrentTag { get; private set; } = ChineseTag;

    public static CultureInfo Culture { get; private set; } = CultureInfo.GetCultureInfo(ChineseTag);

    /// <summary>
    /// 按设置值决定本次运行的语言。<paramref name="setting"/> 为 <see cref="AutoTag"/> 时跟随系统，
    /// 系统语言不在支持列表内则回落到简体中文。
    /// </summary>
    public static void Apply(string? setting)
    {
        var tag = ResolveTag(setting);
        CurrentTag = tag;
        Culture = CultureInfo.GetCultureInfo(tag);
        CultureInfo.CurrentUICulture = Culture;
        CultureInfo.DefaultThreadCurrentUICulture = Culture;
    }

    private static string ResolveTag(string? setting)
    {
        switch (string.IsNullOrWhiteSpace(setting) ? AutoTag : setting.Trim())
        {
            case ChineseTag:
            case "zh":
            case "zh-CN":
            case "zh-Hans-CN":
                return ChineseTag;
            case EnglishTag:
            case "en-US":
            case "en-GB":
                return EnglishTag;
            default:
                // 「跟随系统」或未知值：系统 UI 语言是中文就中文，是英文就英文，其余一律中文。
                var os = CultureInfo.CurrentUICulture;
                if (os.TwoLetterISOLanguageName.Equals("en", StringComparison.OrdinalIgnoreCase))
                {
                    return EnglishTag;
                }
                return ChineseTag;
        }
    }

    /// <summary>取一条文案；键缺失时返回键本身（缺文案能在界面上直接看出来，不静默变成空串）。</summary>
    public static string Get(string key)
    {
        if (string.IsNullOrEmpty(key))
        {
            return string.Empty;
        }
        return Resources.GetString(key, Culture) ?? key;
    }

    /// <summary>带占位符的文案，例如 <c>Format("Settings_MembersCount", 3)</c>。</summary>
    public static string Format(string key, params object?[] args)
    {
        var template = Get(key);
        return args.Length == 0 ? template : string.Format(Culture, template, args);
    }
}
