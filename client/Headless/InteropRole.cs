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
            ClientMode.ShareHost => await RunShareHostAsync(options),
            ClientMode.ShareView => await RunShareViewAsync(options),
            ClientMode.Sources => RunSources(),
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

    /// <summary>
    /// --share host：当共享方。拉起观看方子进程，等它订阅进来后开始共享合成源，
    /// 把媒体核心吐出的信令经服务端转发，跑若干秒后汇总两边的结果。
    /// </summary>
    private static async Task<int> RunShareHostAsync(ClientOptions options)
    {
        Process? child = null;
        MediaEngine? engine = null;
        try
        {
            var token = options.TokenA ?? options.Token;
            if (string.IsNullOrWhiteSpace(token) || string.IsNullOrWhiteSpace(options.TokenB))
            {
                AppLog.Fail("share-host: 需要 --token-a（或 --token）与 --token-b");
                return 2;
            }

            var baseUri = new Uri(options.Server);
            await using var connection = await PartyUpConnection.ConnectAsync(baseUri, token);
            var channel = await EnsureChannelAsync(connection, options.Channel);
            await connection.SubscribeAsync(channel.Id);

            engine = MediaEngine.Load();
            if (engine.MissingExports.Count > 0)
            {
                AppLog.Fail($"share-host: DLL 缺 {engine.MissingExports.Count} 个导出");
                return 1;
            }
            engine.Create("partyup-share-host");

            await using var session = new ShareSession(engine, connection, channel.Id);

            // --share-no-child：观看方是外部进程（例如已经打开、自动登录的 GUI），
            // 此时不拉子进程，只等它订阅进来。
            if (!options.ShareNoChild)
            {
                child = StartShareChildAsync(options, viewer: true);
            }
            // 等观看方订阅进来：offer 是广播，晚订阅就收不到。
            await connection.WaitForPresenceAsync(channel.Id, 2, PresenceTimeout);

            // 用 --share-source 指定的源；元数据（标题/真实分辨率/类型）从枚举结果里取，
            // 不要硬编码——否则 screen.state 会把 2560×1440 的真实显示器报成 640×360 合成源。
            var sources = engine.EnumerateSources();
            var source = sources.FirstOrDefault(s => string.Equals(s.Id, options.ShareSourceId, StringComparison.Ordinal))
                         ?? sources.FirstOrDefault(s => s.Kind == 0)
                         ?? new MediaSource(options.ShareSourceId, options.ShareSourceId, 0, 0, 2);
            AppLog.Pass($"share-host: source id={source.Id} title=\"{source.Title}\" " +
                        $"{source.Width}x{source.Height} kind={source.Kind}");
            await session.StartSharingAsync(source);

            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(options.ShareSeconds);
            var nextReport = DateTime.UtcNow;
            while (DateTime.UtcNow < deadline)
            {
                while (engine.Poll(0) is { } ev)
                {
                    session.HandleMediaEvent(ev);
                }
                if (DateTime.UtcNow >= nextReport)
                {
                    var stats = engine.GetStats();
                    AppLog.Info($"share-host: encoded={stats.FramesEncoded} sent={stats.FramesSent} " +
                                $"signals_out={session.SignalsOut} signals_in={session.SignalsIn} " +
                                $"peer={stats.PeerState} ice={stats.IceState}");
                    nextReport = DateTime.UtcNow.AddSeconds(2);
                }
                await Task.Delay(50);
            }

            await session.StopSharingAsync();
            var finalStats = engine.GetStats();
            AppLog.Pass($"share-host: encoded={finalStats.FramesEncoded} signals_out={session.SignalsOut} " +
                        $"signals_in={session.SignalsIn} encoder={finalStats.Reserved}");

            // 没有子进程时（--share-no-child，观看方是 GUI），只要自己编码出帧就算通过。
            var childOk = child is null || (child.WaitForExit(20_000) && child.ExitCode == 0);
            if (child is { HasExited: false })
            {
                child.Kill(entireProcessTree: true);
            }

            var ok = finalStats.FramesEncoded > 0 && childOk;
            Console.WriteLine($"SHARE_HOST_ENCODED={finalStats.FramesEncoded}");
            Console.WriteLine($"SHARE_HOST_SIGNALS_OUT={session.SignalsOut} SHARE_HOST_SIGNALS_IN={session.SignalsIn}");
            Console.WriteLine($"SHARE_HOST_CHILD_OK={childOk}");
            Console.WriteLine($"SHARE_HOST_RESULT={(ok ? "PASS" : "FAIL")}");
            Console.WriteLine($"SHARE_RESULT={(ok ? "PASS" : "FAIL")}");
            return ok ? 0 : 1;
        }
        catch (Exception ex)
        {
            AppLog.Fail("share-host: " + ex);
            Console.WriteLine("SHARE_RESULT=FAIL");
            return 1;
        }
        finally
        {
            if (child is { HasExited: false })
            {
                child.Kill(entireProcessTree: true);
            }
            engine?.Dispose();
        }
    }

    /// <summary>
    /// --sources：只枚举可共享的源并打印（显示器 id 用于 `--share-source`）。
    /// 不连服务端、不建窗口，纯读媒体核心。
    /// </summary>
    private static int RunSources()
    {
        MediaEngine? engine = null;
        try
        {
            engine = MediaEngine.Load();
            if (engine.MissingExports.Count > 0)
            {
                AppLog.Fail($"sources: DLL 缺 {engine.MissingExports.Count} 个导出");
                Console.WriteLine("SOURCES_RESULT=FAIL");
                return 1;
            }
            engine.Create("partyup-sources");
            var sources = engine.EnumerateSources();
            Console.WriteLine($"SOURCES_COUNT={sources.Count}");
            foreach (var source in sources)
            {
                Console.WriteLine($"SOURCE id={source.Id} kind={source.Kind} {source.Width}x{source.Height} title=\"{source.Title}\"");
            }
            Console.WriteLine("SOURCES_RESULT=PASS");
            return 0;
        }
        catch (Exception ex)
        {
            AppLog.Fail("sources: " + ex);
            Console.WriteLine("SOURCES_RESULT=FAIL");
            return 1;
        }
        finally
        {
            engine?.Dispose();
        }
    }

    /// <summary>
    /// --share view：当观看方。等 offer → 交给媒体核心应答（answer 自动经 SIGNAL_OUT
    /// 回到共享方）→ 统计远端帧，够数就提前成功退出。
    /// </summary>
    private static async Task<int> RunShareViewAsync(ClientOptions options)
    {
        MediaEngine? engine = null;
        try
        {
            var token = options.TokenB ?? options.Token;
            if (string.IsNullOrWhiteSpace(token))
            {
                AppLog.Fail("share-view: 需要 --token-b 或 --token");
                return 2;
            }

            var baseUri = new Uri(options.Server);
            await using var connection = await PartyUpConnection.ConnectAsync(baseUri, token);
            var channel = await EnsureChannelAsync(connection, options.Channel);

            // 顺序很重要：先建引擎与 ShareSession（它挂上 media.signal 处理器），
            // 最后才 subscribe。否则 offer 可能在本进程订阅之后、处理器挂上之前到达，
            // 被服务端推过来却没人接（实测丢第一次 offer 就是这么来的）。
            engine = MediaEngine.Load();
            if (engine.MissingExports.Count > 0)
            {
                AppLog.Fail($"share-view: DLL 缺 {engine.MissingExports.Count} 个导出");
                return 1;
            }
            engine.Create("partyup-share-view");

            await using var session = new ShareSession(engine, connection, channel.Id);
            await connection.SubscribeAsync(channel.Id);
            AppLog.Pass("share-view: waiting for an offer");

            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(options.ShareSeconds + 45);
            var startedAt = DateTime.UtcNow;
            var nextReport = DateTime.UtcNow;
            var lastCount = 0;
            DateTime? firstFrameAt = null;
            DateTime? lastFrameAt = null;
            while (DateTime.UtcNow < deadline)
            {
                while (engine.Poll(0) is { } ev)
                {
                    session.HandleMediaEvent(ev);
                }
                if (session.RemoteFrames != lastCount)
                {
                    lastCount = session.RemoteFrames;
                    firstFrameAt ??= DateTime.UtcNow;
                    lastFrameAt = DateTime.UtcNow;
                }
                var runningFor = (DateTime.UtcNow - startedAt).TotalSeconds;
                // 共享方停推 4 秒以上视为共享结束；否则至少看满 18 秒，帧率/CPU 统计才稳定。
                if (session.RemoteFrames >= 10 && lastFrameAt is { } idleSince &&
                    (DateTime.UtcNow - idleSince).TotalSeconds > 4)
                {
                    break;
                }
                if (session.RemoteFrames >= 30 && runningFor >= 18)
                {
                    break;
                }
                if (DateTime.UtcNow >= nextReport)
                {
                    var stats = engine.GetStats();
                    AppLog.Info($"share-view: frames={session.RemoteFrames} " +
                                $"size={session.RemoteWidth}x{session.RemoteHeight} " +
                                $"stats_received={stats.FramesReceived} signals_in={session.SignalsIn} " +
                                $"signals_out={session.SignalsOut} ice={stats.IceState}");
                    nextReport = DateTime.UtcNow.AddSeconds(2);
                }
                await Task.Delay(50);
            }

            var ok = session.RemoteFrames >= 10;
            var span = firstFrameAt is { } ff && lastFrameAt is { } lf ? (lf - ff).TotalSeconds : 0;
            var fps = span > 0.5 ? (session.RemoteFrames - 1) / span : 0;
            Console.WriteLine($"SHARE_VIEW_FRAMES={session.RemoteFrames}");
            Console.WriteLine($"SHARE_VIEW_SIZE={session.RemoteWidth}x{session.RemoteHeight}");
            Console.WriteLine($"SHARE_VIEW_FPS={fps:F1} SHARE_VIEW_WINDOW={span:F1}s");
            Console.WriteLine($"SHARE_VIEW_SIGNALS_IN={session.SignalsIn} SHARE_VIEW_SIGNALS_OUT={session.SignalsOut}");
            Console.WriteLine($"SHARE_VIEW_RESULT={(ok ? "PASS" : "FAIL")}");
            return ok ? 0 : 1;
        }
        catch (Exception ex)
        {
            AppLog.Fail("share-view: " + ex);
            Console.WriteLine("SHARE_VIEW_RESULT=FAIL");
            return 1;
        }
        finally
        {
            engine?.Dispose();
        }
    }

    /// <summary>拉起观看方子进程（--share view），转发它的输出并标记是否看到 PASS 行。</summary>
    private static Process StartShareChildAsync(ClientOptions options, bool viewer)
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
                     "--share", viewer ? "view" : "host",
                     "--server", options.Server,
                     "--channel", options.Channel,
                     "--token", options.TokenB ?? options.Token ?? string.Empty,
                     "--share-seconds", options.ShareSeconds.ToString(),
                 })
        {
            startInfo.ArgumentList.Add(argument);
        }

        var process = new Process { StartInfo = startInfo };
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                AppLog.Info("[share-view] " + e.Data);
            }
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is not null)
            {
                AppLog.Warn("[share-view] " + e.Data);
            }
        };
        if (!process.Start())
        {
            throw new InvalidOperationException("无法启动观看方子进程");
        }
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        AppLog.Pass($"share-host: viewer child started (pid={process.Id})");
        return process;
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
