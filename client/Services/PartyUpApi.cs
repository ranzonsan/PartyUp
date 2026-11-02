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
    {
        BaseUri = baseUri;
        _http = new HttpClient
        {
            BaseAddress = baseUri,
            Timeout = TimeSpan.FromSeconds(20),
        };
    }

    public Uri BaseUri { get; }

    /// <summary>POST /api/bootstrap：首次启动用 stdout 打印的 token 创建管理员。</summary>
    public Task<SessionDto> BootstrapAsync(string bootstrapToken, string username, string password, CancellationToken ct = default) =>
        PostAsync<SessionDto>("/api/bootstrap", new { token = bootstrapToken, username, password }, ct);

    /// <summary>POST /api/login：用户名 + 密码换 token。</summary>
    public Task<SessionDto> LoginAsync(string username, string password, CancellationToken ct = default) =>
        PostAsync<SessionDto>("/api/login", new { username, password }, ct);

    /// <summary>POST /api/invite/{code}：用邀请码注册并直接登录。</summary>
    public Task<SessionDto> RegisterWithInviteAsync(string code, string username, string password, CancellationToken ct = default) =>
        PostAsync<SessionDto>($"/api/invite/{Uri.EscapeDataString(code)}", new { username, password }, ct);

    /// <summary>GET /api/me：校验 token 并取当前成员。</summary>
    public async Task<MemberDto> MeAsync(string token, CancellationToken ct = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, "/api/me");
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
