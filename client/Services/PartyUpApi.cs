using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using PartyUp.Client.Models;

namespace PartyUp.Client.Services;

/// <summary>服务端返回的结构化错误。Code 与 docs/PROTOCOL.md §0 的错误码一致。</summary>
public sealed class PartyUpApiException : Exception
{
    public PartyUpApiException(string code, string message, HttpStatusCode? status = null)
        : base($"{code}: {message}")
    {
        Code = code;
        Status = status;
    }

    public string Code { get; }

    public HttpStatusCode? Status { get; }
}

/// <summary>
/// HTTP 侧 API（PROTOCOL.md §1：bootstrap / login / invite / me）。
/// 请求与响应一律 System.Text.Json，命名映射由 <see cref="JsonDefaults"/> 统一做。
/// </summary>
public sealed class PartyUpApi : IDisposable
{
    private readonly HttpClient _http;

    public PartyUpApi(Uri baseUri)
        : this(baseUri, handler: null)
    {
    }

    /// <summary>
    /// 测试 seam：允许注入窄 <see cref="HttpMessageHandler"/> 观察最终请求 URI，
    /// 不需要真实网络。生产调用走上面的公开构造函数。
    /// </summary>
    internal PartyUpApi(Uri baseUri, HttpMessageHandler? handler)
    {
        // base URI 统一按目录规范化（补尾斜杠、去 query/fragment）；
        // 下面的相对路径不带前导 "/"，反代前缀（/partyup/）才不会被冲掉。
        BaseUri = ServerEndpoint.NormalizeBase(baseUri);
        _http = handler is null
            ? new HttpClient
            {
                BaseAddress = BaseUri,
                Timeout = TimeSpan.FromSeconds(20),
            }
            : new HttpClient(handler, disposeHandler: false)
            {
                BaseAddress = BaseUri,
                Timeout = TimeSpan.FromSeconds(20),
            };
    }

    public Uri BaseUri { get; }

    /// <summary>POST api/bootstrap：首次启动用 stdout 打印的 token 创建管理员。</summary>
    public Task<SessionDto> BootstrapAsync(string bootstrapToken, string username, string password, CancellationToken ct = default) =>
        PostAsync<SessionDto>("api/bootstrap", new { token = bootstrapToken, username, password }, ct);

    /// <summary>POST api/login：用户名 + 密码换 token。</summary>
    public Task<SessionDto> LoginAsync(string username, string password, CancellationToken ct = default) =>
        PostAsync<SessionDto>("api/login", new { username, password }, ct);

    /// <summary>
    /// POST api/invite/validate：**非消费**的邀请码预检（REG-01）。
    /// 200 = 现在可用；404/409 由 <see cref="PartyUpApiException"/> 的 Code 区分
    /// （NOT_FOUND / CONFLICT）。预检成功不占码，最终注册仍由服务端事务重新裁决。
    /// 用 POST + JSON body 而不是 GET 路径，避免邀请码出现在访问/代理日志里。
    /// </summary>
    public async Task<bool> ValidateInviteAsync(string code, CancellationToken ct = default)
    {
        using var response = await _http.PostAsJsonAsync(
            "api/invite/validate", new { code }, JsonDefaults.Options, ct);
        var text = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, text);
        }
        // 成功体是 {"valid": true}；这里只关心「是不是 200」，不把字段当协议契约。
        return true;
    }

    /// <summary>
    /// POST api/invite/{code}：用邀请码注册并直接登录。
    /// <paramref name="displayName"/> 可选（REG-02）：空白时由服务端回退到唯一登录名，
    /// 客户端不自己拼显示名。
    /// </summary>
    public Task<SessionDto> RegisterWithInviteAsync(
        string code, string username, string password, string? displayName = null,
        CancellationToken ct = default) =>
        PostAsync<SessionDto>(
            $"api/invite/{Uri.EscapeDataString(code)}",
            displayName is null
                ? new { username, password }
                : new { username, password, display_name = displayName },
            ct);

    /// <summary>
    /// GET api/server-info：匿名服务器信息（是否已初始化、注册策略、传输模式）。
    /// 第一屏「添加服务器」靠它判断该进登录、进注册、还是进 owner 创建向导；
    /// 返回值只用于选界面，真正的授权仍由对应写入端点裁决。
    /// </summary>
    public async Task<ServerPublicInfoDto> GetServerInfoAsync(CancellationToken ct = default)
    {
        using var response = await _http.GetAsync("api/server-info", ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
        return JsonSerializer.Deserialize<ServerPublicInfoDto>(body, JsonDefaults.Options)
               ?? throw new PartyUpApiException("INTERNAL", "empty /api/server-info response");
    }

    /// <summary>
    /// POST api/logout：撤销当前 token 并让服务端关闭它的所有 WS 连接（204 无 body）。
    /// 客户端主动退出时必须先调它：否则服务端会话要等 TTL 才失效，且"退出登录"
    /// 不会真正断开别处的连接。失败不阻塞本地退出（本地状态必须清干净）。
    /// </summary>
    public async Task LogoutAsync(string token, CancellationToken ct = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, "api/logout");
        request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", token);
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
    }

    /// <summary>GET api/me：校验 token 并取当前成员。</summary>
    public async Task<MemberDto> MeAsync(string token, CancellationToken ct = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, "api/me");
        request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", token);
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
        var payload = JsonSerializer.Deserialize<MeDto>(body, JsonDefaults.Options);
        return payload?.Member ?? throw new PartyUpApiException("INTERNAL", "empty /api/me response");
    }

    public void Dispose() => _http.Dispose();

    private async Task<T> PostAsync<T>(string path, object body, CancellationToken ct)
    {
        using var response = await _http.PostAsJsonAsync(path, body, JsonDefaults.Options, ct);
        var text = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, text);
        }
        return JsonSerializer.Deserialize<T>(text, JsonDefaults.Options)
               ?? throw new PartyUpApiException("INTERNAL", $"empty response from {path}");
    }

    /// <summary>把服务端的 {"code","message"} 错误体转成异常；解析失败时退回 HTTP 状态。</summary>
    private static PartyUpApiException BuildError(HttpStatusCode status, string body)
    {
        try
        {
            var error = JsonSerializer.Deserialize<ErrorDto>(body, JsonDefaults.Options);
            if (error is not null && !string.IsNullOrWhiteSpace(error.Code))
            {
                return new PartyUpApiException(error.Code, error.Message, status);
            }
        }
        catch (JsonException)
        {
            // 非 JSON 错误页（代理/网关），落到下面的兜底。
        }
        return new PartyUpApiException("INTERNAL", $"HTTP {(int)status} {status}", status);
    }

    private sealed record MeDto(MemberDto Member);
}
