using System.Diagnostics;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.Headless;

/// <summary>
/// 两个真实进程互通验证。
///
/// 调试桥同一时刻只跑一个任务，所以「两个客户端各跑一个 wdb 任务」行不通。
/// 做法：一个 wdb 任务里用 <c>--duo</c> 启动本进程，它再拉起一个同 exe 的
/// <c>--role b</c> 子进程——两个独立 OS 进程、各自独立的 WebSocket 连接与 token，
/// 仍然能拿到「两个客户端」的证据。
///
/// 时序：A 先建/选频道并订阅 → 等 presence 出现 2 人 → 发消息；
///       B 等频道出现 → 订阅 → 等 presence 2 人 → 收到 A 的消息 → 回一条。
/// </summary>
internal static class InteropRole
{
    private static readonly TimeSpan PresenceTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan TextTimeout = TimeSpan.FromSeconds(30);

    public static async Task<int> RunAsync(ClientOptions options)
    {
        return options.Mode switch
        {
            ClientMode.Duo => await RunDuoAsync(options),
            ClientMode.RoleB => await RunSingleRoleAsync(options, isA: false),
            ClientMode.Send => await RunSendAsync(options),
            _ => await RunSingleRoleAsync(options, isA: true),
        };
    }

    /// <summary>--role send：连上→订阅→发一条就退出。用于给已经打开的 GUI 造实时消息。</summary>
    private static async Task<int> RunSendAsync(ClientOptions options)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(options.Token))
            {
                AppLog.Fail("send: 需要 --token");
                return 2;
            }
            var body = string.IsNullOrWhiteSpace(options.Text) ? "hello from --role send" : options.Text;

            var baseUri = new Uri(options.Server);
            await using var connection = await PartyUpConnection.ConnectAsync(baseUri, options.Token);
            var channel = await EnsureChannelAsync(connection, options.Channel);
            await connection.SubscribeAsync(channel.Id);
            var sent = await connection.SendTextAsync(channel.Id, body);
            await connection.WaitForTextAsync(m => m.Id == sent.Id, TextTimeout);
            AppLog.Pass($"send: {connection.Member.Name} -> #{channel.Name} seq={sent.Seq} body=\"{body}\"");
            Console.WriteLine("SEND_RESULT=PASS");
            return 0;
        }
        catch (Exception ex)
        {
            AppLog.Fail("send: " + ex.Message);
            Console.WriteLine("SEND_RESULT=FAIL");
            return 1;
        }
    }

    /// <summary>--duo：本进程当 A，拉一个 B 子进程，两边都断言成功后退出。</summary>
    private static async Task<int> RunDuoAsync(ClientOptions options)
    {
        if (string.IsNullOrWhiteSpace(options.TokenA) || string.IsNullOrWhiteSpace(options.TokenB))
        {
            AppLog.Fail("duo: 需要 --token-a 与 --token-b");
            return 2;
        }

        AppLog.Info("duo: starting child process B");
        Process? child = null;
        var ownOk = false;
        try
        {
            child = StartChildAsync(options);
            ownOk = await RunSingleRoleAsync(options with { Mode = ClientMode.RoleA, Token = options.TokenA }, isA: true) == 0;
        }
        catch (Exception ex)
        {
            AppLog.Fail("duo: " + ex);
        }

        var childOk = false;
        if (child is not null)
        {
            try
            {
                if (!child.WaitForExit(120_000))
                {
                    AppLog.Fail("duo: child B did not exit within 120s, killing it");
                    child.Kill(entireProcessTree: true);
                }
                else
                {
                    childOk = child.ExitCode == 0;
                    AppLog.Pass($"duo: child process B exited with code {child.ExitCode}");
                }
            }
            catch (Exception ex)
            {
                AppLog.Fail("duo: waiting for child: " + ex.Message);
            }
            finally
            {
                child.Dispose();
            }
        }

        if (ownOk && childOk)
        {
            AppLog.Pass("duo: PASS — two OS processes exchanged messages through the server");
            Console.WriteLine("INTEROP_RESULT=PASS");
            Console.WriteLine("INTEROP_PROCESSES=2");
            return 0;
        }

        AppLog.Fail($"duo: FAIL (own={ownOk}, child={childOk})");
        Console.WriteLine("INTEROP_RESULT=FAIL");
        return 1;
    }

    /// <summary>启动一个 --role b 子进程，把它的输出带前缀转发到本进程 stdout。</summary>
    private static Process StartChildAsync(ClientOptions options)
    {
        var exe = Environment.ProcessPath
                  ?? throw new InvalidOperationException("无法取得自身可执行文件路径（Environment.ProcessPath 为空）");
        var startInfo = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = AppContext.BaseDirectory,
        };
        foreach (var argument in new[]
                 {
                     "--role", "b",
                     "--server", options.Server,
                     "--channel", options.Channel,
                     "--token", options.TokenB!,
                 })
        {
            startInfo.ArgumentList.Add(argument);
        }

        var process = new Process { StartInfo = startInfo };
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                AppLog.Info("[child-B] " + e.Data);
            }
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                AppLog.Warn("[child-B] " + e.Data);
            }
        };
        if (!process.Start())
        {
            throw new InvalidOperationException("无法启动 B 子进程");
        }
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        AppLog.Pass($"duo: child process B started (pid={process.Id})");
        return process;
    }

    /// <summary>单角色流程（A 或 B），返回进程退出码。</summary>
    private static async Task<int> RunSingleRoleAsync(ClientOptions options, bool isA)
    {
        var role = isA ? "A" : "B";
        try
        {
            if (string.IsNullOrWhiteSpace(options.Token))
            {
                AppLog.Fail($"role {role}: 需要 --token");
                return 2;
            }

            var baseUri = new Uri(options.Server);
            await using var connection = await PartyUpConnection.ConnectAsync(baseUri, options.Token);
            AppLog.Pass($"role {role}: websocket authenticated as {connection.Member.Name}");

            var channel = await EnsureChannelAsync(connection, options.Channel);
            await connection.SubscribeAsync(channel.Id);
            AppLog.Pass($"role {role}: subscribed #{channel.Name} ({channel.Id})");

            var presence = await connection.WaitForPresenceAsync(channel.Id, 2, PresenceTimeout);
            AppLog.Pass($"role {role}: presence has {presence.Members.Count} members ({string.Join(", ", presence.Members.Select(m => m.Name))})");

            if (isA)
            {
                var sent = await connection.SendTextAsync(channel.Id, $"interop: hello from A ({connection.Member.Name})");
                AppLog.Pass($"role A: sent message seq={sent.Seq} id={sent.Id}");

                var reply = await connection.WaitForTextAsync(m => m.MemberId != connection.Member.Id, TextTimeout);
                AppLog.Pass($"role A: received reply from B seq={reply.Seq} body=\"{reply.Body}\"");
            }
            else
            {
                var incoming = await connection.WaitForTextAsync(m => m.MemberId != connection.Member.Id, TextTimeout);
                AppLog.Pass($"role B: received message from A seq={incoming.Seq} body=\"{incoming.Body}\"");

                var reply = await connection.SendTextAsync(channel.Id, $"interop: reply from B ({connection.Member.Name})");
                AppLog.Pass($"role B: replied with seq={reply.Seq} id={reply.Id}");
            }

            Console.WriteLine($"INTEROP_ROLE_{role}=PASS");
            return 0;
        }
        catch (Exception ex)
        {
            AppLog.Fail($"role {role}: {ex.Message}");
            AppLog.Fail($"role {role}: {ex}");
            Console.WriteLine($"INTEROP_ROLE_{role}=FAIL");
            return 1;
        }
    }

    /// <summary>找到名字匹配的频道；都没有就建一个。并发创建撞名时重试列表。</summary>
    private static async Task<ChannelDto> EnsureChannelAsync(PartyUpConnection connection, string name)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            var list = await connection.ListChannelsAsync();
            var existing = list.Channels.FirstOrDefault(c => string.Equals(c.Name, name, StringComparison.Ordinal));
            if (existing is not null)
            {
                return existing;
            }
            if (list.Channels.Count > 0)
            {
                return list.Channels[0];
            }
            try
            {
                return await connection.CreateChannelAsync(name);
            }
            catch (PartyUpApiException ex) when (ex.Code == "CONFLICT")
            {
                // 对手进程刚建好，下一轮列表就能看到。
                await Task.Delay(200);
            }
        }
        throw new TimeoutException($"20 秒内没有等到频道 #{name}");
    }
}
