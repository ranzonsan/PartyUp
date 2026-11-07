using Microsoft.AspNetCore.WebUtilities;

namespace PartyUp.Client.Services;

/// <summary>
/// REG-03 邀请链接：服务器绝对 HTTP(S) base URL + 恰好一个非空 <c>code</c> 查询参数，
/// 例如 <c>https://example.org/partyup/?code=ABCDE12345</c>。
///
/// 这不是浏览器注册页，也不是 <c>partyup://</c> 深链。解析是**纯数据操作**：
/// 不发 HTTP 请求、不打开浏览器、不消耗邀请码，也不表示该服务器已验证或可信——
/// 是否信任服务器只在用户确认并与真实服务端握手后才有结论。
///
/// URI 与 query 语法分别由 <see cref="Uri"/> 和
/// <see cref="QueryHelpers"/>（Microsoft.AspNetCore.WebUtilities）处理，
/// 不手写 Split/正则/百分号解码器。
/// </summary>
public sealed record InviteLink(Uri ServerBaseUri, string Code)
{
    /// <summary>解析失败时抛出 <see cref="ArgumentException"/>；消息不含邀请码或原链接。</summary>
    public static InviteLink Parse(string? raw)
    {
        if (!TryParse(raw, out var link, out var error))
        {
            throw new ArgumentException(error ?? "邀请链接无效。", nameof(raw));
        }
        return link!;
    }

    /// <summary>
    /// 解析邀请链接。只接受绝对 HTTP(S)、合法 host/port、无 user-info 或 fragment；
    /// query 里必须**恰好**一个非空 <c>code</c>，重复 code、空 key、未知参数一律拒绝，
    /// 不默默取第一个或最后一个。失败时 <paramref name="error"/> 是可展示的中文原因。
    /// </summary>
    public static bool TryParse(string? raw, out InviteLink? link, out string? error)
    {
        link = null;
        if (string.IsNullOrWhiteSpace(raw))
        {
            error = "邀请链接不能为空。";
            return false;
        }

        // System.Uri 负责绝对 URI、host/port 与百分号编码的合法性。
        if (!Uri.TryCreate(raw.Trim(), UriKind.Absolute, out var uri))
        {
            error = "邀请链接必须是带 http:// 或 https:// 的完整地址。";
            return false;
        }
        if (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)
        {
            error = "邀请链接只支持 HTTP/HTTPS。";
            return false;
        }
        if (string.IsNullOrEmpty(uri.Host))
        {
            error = "邀请链接缺少服务器主机名。";
            return false;
        }
        if (!string.IsNullOrEmpty(uri.UserInfo))
        {
            error = "服务器地址不能包含用户名或密码。";
            return false;
        }
        if (!string.IsNullOrEmpty(uri.Fragment))
        {
            error = "邀请链接不能包含 # 片段。";
            return false;
        }
        if (uri.Port == 0)
        {
            error = "服务器端口无效。";
            return false;
        }

        // QueryHelpers 负责 query 的百分号/加号解码，key 比较是 OrdinalIgnoreCase。
        var query = QueryHelpers.ParseQuery(uri.Query);
        string? rawCode = null;
        foreach (var pair in query)
        {
            if (pair.Key.Length == 0)
            {
                error = "邀请链接包含空查询参数。";
                return false;
            }
            if (!string.Equals(pair.Key, "code", StringComparison.OrdinalIgnoreCase))
            {
                error = "邀请链接包含未知查询参数。";
                return false;
            }
            if (pair.Value.Count != 1)
            {
                error = "邀请链接包含重复的 code 参数。";
                return false;
            }
            rawCode = pair.Value[0];
        }
        if (rawCode is null)
        {
            error = "邀请链接缺少 code 参数。";
            return false;
        }

        var code = NormalizeCode(rawCode);
        if (code.Length == 0)
        {
            error = "邀请码不能为空。";
            return false;
        }

        link = new InviteLink(ServerEndpoint.NormalizeBase(uri), code);
        error = null;
        return true;
    }

    /// <summary>
    /// 邀请码归一化，与 Go <c>auth.NormalizeInviteCode</c> 保持一致：
    /// 去首尾空白、转大写、去掉 <c>-</c>、空格与 <c>_</c>。不另外发明身份规则。
    /// </summary>
    public static string NormalizeCode(string? code)
    {
        if (string.IsNullOrEmpty(code))
        {
            return string.Empty;
        }
        return code.Trim()
            .ToUpperInvariant()
            .Replace("-", string.Empty, StringComparison.Ordinal)
            .Replace(" ", string.Empty, StringComparison.Ordinal)
            .Replace("_", string.Empty, StringComparison.Ordinal);
    }

    /// <summary>
    /// 重新组装规范链接（base 已无 query，code 由成熟 query 编码器转义）。
    /// 用于往返验证，不是发请求的入口。
    /// </summary>
    public Uri ToUri() =>
        new(QueryHelpers.AddQueryString(ServerBaseUri.AbsoluteUri, "code", Code), UriKind.Absolute);
}

/// <summary>
/// 服务器 base URI 的规范化与端点拼接。正式 <see cref="PartyUpApi"/> 与
/// <see cref="PartyUpConnection"/> 共用，保证反代前缀（<c>/partyup/</c>）不被
/// 前导 <c>/</c> 的相对路径冲掉；root 部署路径不变。
/// </summary>
public static class ServerEndpoint
{
    /// <summary>
    /// 去掉 query/fragment，并把 path 当目录处理（补尾斜杠）。
    /// HttpClient 的 BaseAddress 只有以 <c>/</c> 结尾时，相对路径才会保留前缀。
    /// </summary>
    public static Uri NormalizeBase(Uri baseUri)
    {
        ArgumentNullException.ThrowIfNull(baseUri);
        if (!baseUri.IsAbsoluteUri)
        {
            throw new ArgumentException("server base URI must be absolute", nameof(baseUri));
        }

        var builder = new UriBuilder(baseUri)
        {
            Query = string.Empty,
            Fragment = string.Empty,
        };
        if (builder.Path.Length == 0 || builder.Path[^1] != '/')
        {
            builder.Path += "/";
        }
        return builder.Uri;
    }

    /// <summary>
    /// 控制连接端点：http→ws、https→wss，并保留 base path。
    /// 例：<c>https://example.org/partyup/</c> → <c>wss://example.org/partyup/ws</c>。
    /// </summary>
    public static Uri WebSocketUri(Uri baseUri)
    {
        var normalized = NormalizeBase(baseUri);
        var builder = new UriBuilder(new Uri(normalized, "ws"))
        {
            Scheme = normalized.Scheme == Uri.UriSchemeHttps ? "wss" : "ws",
            Query = string.Empty,
            Fragment = string.Empty,
        };
        if (normalized.IsDefaultPort)
        {
            builder.Port = -1;
        }
        return builder.Uri;
    }
}
