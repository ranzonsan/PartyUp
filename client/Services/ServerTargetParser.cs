using System;
using System.Text.RegularExpressions;

namespace PartyUp.Client.Services;

/// <summary>用户在第一屏输入的东西是哪一类。</summary>
public enum ServerTargetKind
{
    /// <summary>服务器地址，例如 <c>http://192.168.1.10:8080</c>。</summary>
    Url,

    /// <summary>邀请链接，例如 <c>https://example.org/?code=ABCDE12345</c>。</summary>
    InviteLink,

    /// <summary>只有邀请码（需要配合已选服务器）。</summary>
    InviteCode,
}

/// <summary>
/// 第一屏「添加服务器」输入的解析结果。
/// <see cref="BaseUri"/> 在 <see cref="ServerTargetKind.InviteCode"/> 时可能为 null——
/// 裸邀请码本身不含服务器信息，调用方要用当前选中/上次使用的服务器补上。
/// </summary>
public sealed record ServerTarget(ServerTargetKind Kind, Uri? BaseUri, string? InviteCode)
{
    /// <summary>带邀请信息的输入意味着「注册/加入」，而不是「登录已有账号」。</summary>
    public bool IsJoinFlow => Kind != ServerTargetKind.Url;

    /// <summary>用一个已知服务器补全裸邀请码。</summary>
    public ServerTarget WithBase(Uri baseUri) => this with { BaseUri = baseUri };
}

/// <summary>
/// 把第一屏的输入识别成「服务器地址 / 邀请链接 / 邀请码」。
///
/// 复用既有的 <see cref="InviteLink"/>（与 Go 端共用 tests/fixtures/invite-link-cases.json
/// 做跨语言往返校验），不在这里另写一套链接解析规则。
/// </summary>
public static class ServerTargetParser
{
    private static readonly Regex InviteCodePattern = new(
        "^[A-Za-z0-9][A-Za-z0-9_-]{5,63}$", RegexOptions.CultureInvariant);

    public static bool TryParse(string? raw, out ServerTarget? target, out string? error)
    {
        target = null;
        error = null;

        var text = (raw ?? string.Empty).Trim().Trim('"', '\'', '<', '>').Trim();
        if (text.Length == 0)
        {
            error = "请输入服务器地址、邀请链接或邀请码。";
            return false;
        }

        // 1) 完整邀请链接（?code=...）
        if (InviteLink.TryParse(text, out var link, out _) && link is not null)
        {
            target = new ServerTarget(
                ServerTargetKind.InviteLink,
                ServerEndpoint.NormalizeBase(link.ServerBaseUri),
                InviteLink.NormalizeCode(link.Code));
            return true;
        }

        // 2) 省略协议的邀请链接 / 地址（example.org:8080/?code=XXX）
        if (!text.Contains("://", StringComparison.Ordinal))
        {
            if (Uri.TryCreate("http://" + text, UriKind.Absolute, out var withScheme) &&
                InviteLink.TryParse(withScheme.ToString(), out var schemelessLink, out _) &&
                schemelessLink is not null)
            {
                target = new ServerTarget(
                    ServerTargetKind.InviteLink,
                    ServerEndpoint.NormalizeBase(schemelessLink.ServerBaseUri),
                    InviteLink.NormalizeCode(schemelessLink.Code));
                return true;
            }
        }

        // 3) 普通服务器地址（必须 http/https 且带主机名）
        if (Uri.TryCreate(text, UriKind.Absolute, out var absolute) &&
            (absolute.Scheme == Uri.UriSchemeHttp || absolute.Scheme == Uri.UriSchemeHttps) &&
            !string.IsNullOrWhiteSpace(absolute.Host))
        {
            target = new ServerTarget(ServerTargetKind.Url, ServerEndpoint.NormalizeBase(absolute), null);
            return true;
        }

        // 4) 省略协议的地址：只有形如 host / host:port / IP 才补 http://
        if (!text.Contains("://", StringComparison.Ordinal) &&
            (text.Contains('.', StringComparison.Ordinal) ||
             text.Contains(':') ||
             text.Equals("localhost", StringComparison.OrdinalIgnoreCase)) &&
            Uri.TryCreate("http://" + text, UriKind.Absolute, out var bare) &&
            !string.IsNullOrWhiteSpace(bare.Host))
        {
            target = new ServerTarget(ServerTargetKind.Url, ServerEndpoint.NormalizeBase(bare), null);
            return true;
        }

        // 5) 裸邀请码
        if (InviteCodePattern.IsMatch(text))
        {
            target = new ServerTarget(ServerTargetKind.InviteCode, null, InviteLink.NormalizeCode(text));
            return true;
        }

        error = "无法识别：请粘贴服务器地址（http://主机:端口）、邀请链接或邀请码。";
        return false;
    }
}
