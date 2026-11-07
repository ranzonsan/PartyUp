using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using PartyUp.Client.Models;

namespace PartyUp.Client.Services;

/// <summary>
/// 服务器管理（/api/admin/*）的 HTTP 客户端。
///
/// 为什么单独一个类而不是塞进 <see cref="PartyUpApi"/>：管理面只在设置里用到，
/// 且**所有请求都带 Bearer**；分开能让登录路径保持「无凭据也能调」的清晰边界。
/// 注意：这里的任何"能否操作"判断都只是界面提示，真正的授权由服务端按角色拦截
/// （危险设置 owner-only、owner 不可被管理、admin 不能改 owner 等）。
/// </summary>
public sealed class AdministrationApi : IDisposable
{
    private readonly HttpClient _http;
    private readonly string _token;

    public AdministrationApi(Uri baseUri, string token)
    {
        BaseUri = ServerEndpoint.NormalizeBase(baseUri);
        _token = token;
        _http = new HttpClient
        {
            BaseAddress = BaseUri,
            Timeout = TimeSpan.FromSeconds(15),
        };
    }

    public Uri BaseUri { get; }

    public Task<AdminOverviewDto> GetOverviewAsync(CancellationToken ct = default) =>
        GetAsync<AdminOverviewDto>("api/admin/overview", ct);

    public async Task<IReadOnlyList<AdminMemberDto>> GetMembersAsync(CancellationToken ct = default) =>
        (await GetAsync<MembersEnvelope>("api/admin/members", ct)).Members ?? new List<AdminMemberDto>();

    public Task KickAsync(string memberId, CancellationToken ct = default) =>
        PostAsync($"api/admin/members/{Uri.EscapeDataString(memberId)}/kick", null, ct);

    public Task<AdminBanResultDto> BanAsync(string memberId, string? reason, CancellationToken ct = default) =>
        PostAsync<AdminBanResultDto>(
            $"api/admin/members/{Uri.EscapeDataString(memberId)}/ban",
            new { reason = reason ?? string.Empty },
            ct);

    public Task UnbanAsync(string memberId, CancellationToken ct = default) =>
        PostAsync($"api/admin/members/{Uri.EscapeDataString(memberId)}/unban", null, ct);

    /// <summary>改角色：只能是 user ↔ admin，owner 不可被改（服务端拦截）。</summary>
    public Task SetRoleAsync(string memberId, string role, CancellationToken ct = default) =>
        PostAsync($"api/admin/members/{Uri.EscapeDataString(memberId)}/role", new { role }, ct);

    /// <summary>重置密码：返回一次性临时密码，界面必须提示「只显示这一次」。</summary>
    public Task<AdminPasswordResetDto> ResetPasswordAsync(string memberId, CancellationToken ct = default) =>
        PostAsync<AdminPasswordResetDto>(
            $"api/admin/members/{Uri.EscapeDataString(memberId)}/password", null, ct);

    public async Task<IReadOnlyList<AdminInviteDto>> GetInvitesAsync(CancellationToken ct = default) =>
        (await GetAsync<InvitesEnvelope>("api/admin/invites", ct)).Invites ?? new List<AdminInviteDto>();

    public async Task<AdminInviteCreateDto> CreateInviteAsync(CancellationToken ct = default) =>
        (await PostAsync<InviteEnvelope>("api/admin/invites", null, ct)).Invite
        ?? throw new PartyUpApiException("INTERNAL", "empty invite response");

    public async Task RevokeInviteAsync(string code, CancellationToken ct = default)
    {
        using var request = new HttpRequestMessage(
            HttpMethod.Delete, $"api/admin/invites?code={Uri.EscapeDataString(code)}");
        request.Headers.Authorization = Bearer();
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
    }

    public Task<ServerSettingsDto> GetSettingsAsync(CancellationToken ct = default) =>
        GetAsync<ServerSettingsDto>("api/admin/settings", ct);

    /// <summary>按字段白名单 PATCH 设置；危险项由服务端按角色拒绝。</summary>
    public async Task<ServerSettingsDto> PatchSettingsAsync(object patch, CancellationToken ct = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Patch, "api/admin/settings")
        {
            Content = JsonContent.Create(patch, options: JsonDefaults.Options),
        };
        request.Headers.Authorization = Bearer();
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
        return JsonSerializer.Deserialize<ServerSettingsDto>(body, JsonDefaults.Options)
               ?? throw new PartyUpApiException("INTERNAL", "empty settings response");
    }

    public async Task<IReadOnlyList<AdminAuditEventDto>> GetAuditAsync(
        int limit = 100, long before = 0, CancellationToken ct = default)
    {
        var path = $"api/admin/audit?limit={limit}";
        if (before > 0)
        {
            path += $"&before={before}";
        }
        return (await GetAsync<AuditEnvelope>(path, ct)).Events ?? new List<AdminAuditEventDto>();
    }

    // 服务端列表端点是「带壳对象」而不是裸数组（members / invites / events），
    // 直接按 List<T> 反序列化会报 "JSON value could not be converted..."（真机抓到）。
    private sealed record MembersEnvelope(List<AdminMemberDto>? Members);
    private sealed record InvitesEnvelope(List<AdminInviteDto>? Invites);
    private sealed record AuditEnvelope(List<AdminAuditEventDto>? Events);
    private sealed record InviteEnvelope(AdminInviteCreateDto? Invite);

    /// <summary>所有权转移（仅 owner）。</summary>
    public Task TransferOwnershipAsync(string memberId, CancellationToken ct = default) =>
        PostAsync("api/admin/owner/transfer", new { member_id = memberId }, ct);

    private async Task<T> GetAsync<T>(string path, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, path);
        request.Headers.Authorization = Bearer();
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
        return JsonSerializer.Deserialize<T>(body, JsonDefaults.Options)
               ?? throw new PartyUpApiException("INTERNAL", "empty admin response: " + path);
    }

    private async Task PostAsync(string path, object? payload, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, path);
        request.Headers.Authorization = Bearer();
        if (payload is not null)
        {
            request.Content = JsonContent.Create(payload, options: JsonDefaults.Options);
        }
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
    }

    private async Task<T> PostAsync<T>(string path, object? payload, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, path);
        request.Headers.Authorization = Bearer();
        if (payload is not null)
        {
            request.Content = JsonContent.Create(payload, options: JsonDefaults.Options);
        }
        using var response = await _http.SendAsync(request, ct);
        var body = await response.Content.ReadAsStringAsync(ct);
        if (!response.IsSuccessStatusCode)
        {
            throw BuildError(response.StatusCode, body);
        }
        return JsonSerializer.Deserialize<T>(body, JsonDefaults.Options)
               ?? throw new PartyUpApiException("INTERNAL", "empty admin response: " + path);
    }

    private System.Net.Http.Headers.AuthenticationHeaderValue Bearer() => new("Bearer", _token);

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
            // 非 JSON 错误体：退回状态码，保留原始文本便于排查。
        }
        return new PartyUpApiException("HTTP_" + (int)status, body.Trim(), status);
    }

    public void Dispose() => _http.Dispose();
}
