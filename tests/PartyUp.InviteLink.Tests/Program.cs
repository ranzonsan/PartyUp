using System.Net;
using System.Text;
using System.Text.Json;
using PartyUp.Client.Services;

namespace PartyUp.InviteLinkTests;

/// <summary>
/// REG-03 导入端 / transport 回归。真正的被测代码是链接编译进来的生产
/// <c>client/Services/InviteLink.cs</c> 与 <c>PartyUpApi.cs</c>，不是复制品。
///
/// 覆盖：跨语言 fixture 的解析/往返、非法链接拒绝且不泄漏、解析零网络 I/O、
/// 邀请码归一化与 auth 一致、API/WS URI 在反代 base path 与 root 部署下都不丢前缀。
/// 这里不发真实网络请求：HTTP 用窄 managed handler 记录绝对 URI。
/// </summary>
internal static class Program
{
    private static int _failures;
    private static int _checks;

    private static int Main()
    {
        Run("fixture: Go-built links parse to canonical base + code", FixtureBuildLinksParse);
        Run("fixture: every accepted link round-trips through ToUri", FixtureRoundTrip);
        Run("fixture: malformed/ambiguous links are rejected without echoing secrets", FixtureRejectCases);
        Run("parser: resolving no DNS/network is needed for a .invalid host", ParseNeedsNoNetwork);
        Run("parser: invite code normalization matches server auth rules", CodeNormalizationParity);
        Run("endpoint: WebSocket URI keeps base path (http/https/port/root)", WebSocketUriKeepsBasePath);
        Run("api: HTTP paths keep reverse-proxy base path and root deployment", ApiKeepsBasePath);

        Console.WriteLine();
        if (_failures == 0)
        {
            Console.WriteLine($"PASS: invite link service ({_checks} checks)");
            return 0;
        }
        Console.WriteLine($"FAIL: invite link service ({_failures} failure(s) / {_checks} checks)");
        return 1;
    }

    // ------------------------------------------------------------------
    // fixture
    // ------------------------------------------------------------------

    private sealed record LinkCase(string Server, string Code, string Base, string Link);

    private sealed record Fixture(
        List<LinkCase> Build,
        List<string> BuildReject,
        List<LinkCase> Parse,
        List<string> ParseReject);

    private static Fixture LoadFixture()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "invite-link-cases.json");
        var json = File.ReadAllText(path);
        var options = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        };
        var fixture = JsonSerializer.Deserialize<Fixture>(json, options)
                      ?? throw new InvalidOperationException("invite link fixture is empty: " + path);
        if (fixture.Build.Count == 0 || fixture.Parse.Count == 0 ||
            fixture.BuildReject.Count == 0 || fixture.ParseReject.Count == 0)
        {
            throw new InvalidOperationException("invite link fixture is missing cases: " + path);
        }
        return fixture;
    }

    // 生成端（Go）产出的链接，导入端必须解析出同一个 base 与 code。
    private static void FixtureBuildLinksParse()
    {
        var fixture = LoadFixture();
        foreach (var testCase in fixture.Build)
        {
            var link = InviteLink.Parse(testCase.Link);
            Check(link.ServerBaseUri.AbsoluteUri == testCase.Base,
                $"base of {testCase.Link} = {link.ServerBaseUri.AbsoluteUri}, want {testCase.Base}");
            Check(link.Code == testCase.Code,
                $"code of {testCase.Link} = {link.Code}, want {testCase.Code}");
        }
    }

    // 解析出的 base+code 必须能重新组装成规范链接（同一 fixture 的 parse 面）。
    private static void FixtureRoundTrip()
    {
        var fixture = LoadFixture();
        foreach (var testCase in fixture.Build)
        {
            var canonical = InviteLink.Parse(testCase.Link).ToUri();
            Check(canonical.AbsoluteUri == testCase.Link,
                $"round-trip of {testCase.Link} = {canonical.AbsoluteUri}");
        }
        foreach (var testCase in fixture.Parse)
        {
            var link = InviteLink.Parse(testCase.Link);
            var expected = new Uri($"{testCase.Base}?code={testCase.Code}", UriKind.Absolute);
            Check(link.ToUri() == expected,
                $"round-trip of {testCase.Link} = {link.ToUri().AbsoluteUri}, want {expected.AbsoluteUri}");
        }
    }

    // 拒绝面：失败、错误信息不包含整条链接/邀请码。
    private static void FixtureRejectCases()
    {
        var fixture = LoadFixture();
        foreach (var raw in fixture.ParseReject)
        {
            var ok = InviteLink.TryParse(raw, out var link, out var error);
            Check(!ok, $"accepted invalid invite link: {raw}");
            Check(link is null, $"rejected link still produced a value: {raw}");
            Check(!string.IsNullOrWhiteSpace(error), $"rejected link has no error: {raw}");
            CheckSafeError(error!, raw);
        }

        // 显式凭据不泄漏：错误里不能出现 code 值或 "://" 形式的整条链接。
        const string secret = "SECRET-CODE-XYZ";
        Check(!InviteLink.TryParse($"https://example.org/partyup/?code={secret}&ref=1", out _, out var leakError),
            "link with unknown parameter was accepted");
        CheckSafeError(leakError!, secret);
    }

    private static void ParseNeedsNoNetwork()
    {
        // .invalid 是 RFC 2606 保留域，永远不解析；解析成功即说明没有联网。
        var link = InviteLink.Parse("http://invite-link.invalid/partyup/?code=K7QM3XPD");
        Check(link.ServerBaseUri.Host == "invite-link.invalid", $"host = {link.ServerBaseUri.Host}");
        Check(link.ServerBaseUri.AbsolutePath == "/partyup/", $"path = {link.ServerBaseUri.AbsolutePath}");
        Check(link.Code == "K7QM3XPD", $"code = {link.Code}");
    }

    private static void CodeNormalizationParity()
    {
        // 与 Go auth.NormalizeInviteCode 相同：去首尾空白、大写、去掉 - 空格 _。
        Check(InviteLink.NormalizeCode("k7qm-3xpd") == "K7QM3XPD", "lowercase+dash normalization");
        Check(InviteLink.NormalizeCode(" K7QM 3XPD ") == "K7QM3XPD", "trim+space normalization");
        Check(InviteLink.NormalizeCode("k7qm_3xpd") == "K7QM3XPD", "underscore normalization");
        Check(InviteLink.NormalizeCode(" -- ") == string.Empty, "separator-only code normalizes to empty");

        var link = InviteLink.Parse("https://example.org/partyup/?code=k7qm-3xpd");
        Check(link.Code == "K7QM3XPD", $"parsed code = {link.Code}, want K7QM3XPD");
    }

    private static void WebSocketUriKeepsBasePath()
    {
        Check(ServerEndpoint.WebSocketUri(new Uri("http://127.0.0.1:8080")).AbsoluteUri ==
              "ws://127.0.0.1:8080/ws", "root http ws uri");
        Check(ServerEndpoint.WebSocketUri(new Uri("https://example.org")).AbsoluteUri ==
              "wss://example.org/ws", "root https ws uri");
        Check(ServerEndpoint.WebSocketUri(new Uri("http://127.0.0.1:8080/partyup/")).AbsoluteUri ==
              "ws://127.0.0.1:8080/partyup/ws", "prefixed http ws uri");
        Check(ServerEndpoint.WebSocketUri(new Uri("https://example.org/partyup")).AbsoluteUri ==
              "wss://example.org/partyup/ws", "prefixed https ws uri without trailing slash");
        Check(ServerEndpoint.WebSocketUri(new Uri("https://example.org:8443/partyup/")).AbsoluteUri ==
              "wss://example.org:8443/partyup/ws", "prefixed non-default port ws uri");
    }

    // ------------------------------------------------------------------
    // API base path（窄 managed HTTP handler，零真实网络）
    // ------------------------------------------------------------------

    private static async Task ApiKeepsBasePath()
    {
        var prefixed = new RecordingHandler();
        var link = InviteLink.Parse("https://example.test/partyup/?code=K7QM3XPD");
        using (var api = new PartyUpApi(link.ServerBaseUri, prefixed))
        {
            var session = await api.LoginAsync("alice", "pw");
            Check(session.Token == "t", "login did not consume the fake session response");
            await api.MeAsync("t");
            await api.RegisterWithInviteAsync("K7QM3XPD", "bob", "pw2");
        }
        Check(prefixed.Requests.Count == 3, $"prefixed request count = {prefixed.Requests.Count}");
        CheckRequest(prefixed, 0, "https://example.test/partyup/api/login");
        CheckRequest(prefixed, 1, "https://example.test/partyup/api/me");
        CheckRequest(prefixed, 2, "https://example.test/partyup/api/invite/K7QM3XPD");

        // root 部署保持原路径。
        var root = new RecordingHandler();
        using (var api = new PartyUpApi(new Uri("http://127.0.0.1:8080"), root))
        {
            await api.LoginAsync("alice", "pw");
        }
        Check(root.Requests.Count == 1, $"root request count = {root.Requests.Count}");
        CheckRequest(root, 0, "http://127.0.0.1:8080/api/login");
    }

    private static void CheckRequest(RecordingHandler handler, int index, string expected)
    {
        if (index >= handler.Requests.Count)
        {
            Check(false, $"request #{index} was never sent (got {handler.Requests.Count})");
            return;
        }
        var actual = handler.Requests[index].AbsoluteUri;
        Check(actual == expected, $"request #{index} = {actual}, want {expected}");
    }

    /// <summary>只记录绝对请求 URI 并回固定 JSON；不连任何 socket。</summary>
    private sealed class RecordingHandler : HttpMessageHandler
    {
        public List<Uri> Requests { get; } = new();

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Requests.Add(request.RequestUri ?? throw new InvalidOperationException("request has no URI"));
            var path = request.RequestUri!.AbsolutePath;
            var json = path.EndsWith("/api/me", StringComparison.Ordinal)
                ? "{\"member\":{\"id\":\"m1\",\"name\":\"alice\",\"admin\":true}}"
                : "{\"member\":{\"id\":\"m1\",\"name\":\"alice\",\"admin\":true},\"token\":\"t\"}";
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(json, Encoding.UTF8, "application/json"),
            });
        }
    }

    // ------------------------------------------------------------------
    // runner
    // ------------------------------------------------------------------

    private static void CheckSafeError(string error, string raw)
    {
        // 错误可以给出格式提示（里面会出现 "http://" 字样），但不能回显输入本身。
        if (!string.IsNullOrWhiteSpace(raw))
        {
            Check(!error.Contains(raw, StringComparison.Ordinal),
                $"error echoes the rejected input: {error}");
        }
        Check(!error.Contains("SECRET", StringComparison.OrdinalIgnoreCase),
            $"error echoes an invite code: {error}");
    }

    private static void Run(string name, Action test)
    {
        Console.WriteLine();
        Console.WriteLine("== " + name);
        try
        {
            test();
        }
        catch (Exception ex)
        {
            Fail($"unhandled exception: {ex}");
        }
    }

    private static void Run(string name, Func<Task> test)
    {
        Console.WriteLine();
        Console.WriteLine("== " + name);
        try
        {
            test().GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            Fail($"unhandled exception: {ex}");
        }
    }

    private static void Check(bool condition, string message)
    {
        _checks++;
        if (!condition)
        {
            Fail(message);
        }
    }

    private static void Fail(string message)
    {
        _failures++;
        Console.WriteLine("FAIL: " + message);
    }
}
