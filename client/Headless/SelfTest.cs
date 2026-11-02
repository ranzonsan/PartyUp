using System.Diagnostics;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.Headless;

/// <summary>
/// 无头自检：一次跑完两件验收。
///
/// 1. 媒体核心：DLL 加载 → 18 个导出可解析 → pu_engine_create → pu_peer_stats →
///    pu_screen_start(synthetic) → 轮询到 PU_EVENT_STATE → stop → destroy。
/// 2. 控制面：bootstrap 建管理员 → 邀请码注册第二个成员 → 两条真实 WebSocket →
///    同频道互相收到 text.new → text.history 可读回。
///
/// 输出约定（stdout，供 scripts 用 grep 断言，不需要解析 JSON）：
///   PASS/FAIL 行          人类可读的证据
///   SELFTEST_ALICE_TOKEN=… / SELFTEST_BOB_TOKEN=… / SELFTEST_CHANNEL_ID=…
///   SELFTEST_RESULT=PASS|FAIL（进程退出码 0/1）
/// </summary>
internal static class SelfTest
{
    private static readonly TimeSpan StepTimeout = TimeSpan.FromSeconds(20);

    public static async Task<int> RunAsync(ClientOptions options)
    {
        var failures = new List<string>();
        var clock = Stopwatch.StartNew();
        AppLog.Info("selftest: start");

        var mediaOk = RunMediaChecks(failures);
        var tokens = await RunNetworkChecksAsync(options, failures);
        clock.Stop();

        if (tokens is not null)
        {
            Console.WriteLine($"SELFTEST_ALICE_TOKEN={tokens.Value.AliceToken}");
            Console.WriteLine($"SELFTEST_BOB_TOKEN={tokens.Value.BobToken}");
            Console.WriteLine($"SELFTEST_CHANNEL_ID={tokens.Value.ChannelId}");
            Console.WriteLine($"SELFTEST_CHANNEL_NAME={tokens.Value.ChannelName}");
            Console.Out.Flush();
        }

        var passed = mediaOk && failures.Count == 0;
        if (passed)
        {
            AppLog.Pass($"selftest: all checks passed in {clock.ElapsedMilliseconds} ms");
            Console.WriteLine("SELFTEST_RESULT=PASS");
            return 0;
        }

        AppLog.Fail($"selftest: {failures.Count} failure(s)");
        foreach (var failure in failures)
        {
            AppLog.Fail("  - " + failure);
        }
        Console.WriteLine("SELFTEST_RESULT=FAIL");
        return 1;
    }

    private static bool RunMediaChecks(List<string> failures)
    {
        var before = failures.Count;
        MediaEngine? engine = null;
        try
        {
            var dllPath = MediaEngine.DefaultLibraryPath;
            if (!File.Exists(dllPath))
            {
                failures.Add($"partyup_media.dll 不存在：{dllPath}");
                return false;
            }
            var size = new FileInfo(dllPath).Length;
            AppLog.Pass($"media: DLL found ({size} bytes) at {dllPath}");

            engine = MediaEngine.Load();
            var resolvedExports = MediaEngine.ExportNames.Count - engine.MissingExports.Count;
            if (engine.MissingExports.Count > 0)
            {
                failures.Add($"缺少导出：{string.Join(", ", engine.MissingExports)}");
                return false;
            }
            AppLog.Pass($"media: LoadLibrary ok, ABI v2 exports resolved {resolvedExports}/{MediaEngine.ExportNames.Count}");

            // 布局断言：C 侧 sizeof(pu_event)=256、sizeof(pu_stats)=64。
            // 这两个值错了会表现为 pu_poll_event / pu_peer_stats 的 INVALID_ARG。
            AppLog.Pass($"media: struct sizes pu_event={MediaEngine.EventStructSize} pu_stats={MediaEngine.StatsStructSize} (expect 256/64)");
            if (MediaEngine.EventStructSize != 256)
            {
                failures.Add($"pu_event 托管布局 {MediaEngine.EventStructSize} != 256");
            }
            if (MediaEngine.StatsStructSize != 64)
            {
                failures.Add($"pu_stats 托管布局 {MediaEngine.StatsStructSize} != 64");
            }

            engine.Create("partyup-selftest");
            AppLog.Pass("media: pu_engine_create -> non-NULL handle");

            var stats = engine.GetStats();
            AppLog.Pass($"media: pu_peer_stats ok (has_peer={stats.HasPeer}, peer_state={stats.PeerState}, ice_state={stats.IceState})");
            if (stats.HasPeer != 0)
            {
                failures.Add($"新建引擎 has_peer={stats.HasPeer}，期望 0");
            }

            engine.StartSynthetic();
            AppLog.Pass("media: pu_screen_start(\"synthetic\") -> PU_OK");

            MediaEvent? stateEvent = null;
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
            while (DateTime.UtcNow < deadline)
            {
                var ev = engine.Poll(200);
                if (ev is null)
                {
                    continue;
                }
                if (ev.Kind == MediaEventKind.State)
                {
                    stateEvent = ev;
                    break;
                }
            }
            if (stateEvent is null)
            {
                failures.Add("pu_screen_start 之后 5 秒内没有收到 PU_EVENT_STATE");
            }
            else
            {
                AppLog.Pass($"media: PU_EVENT_STATE received: {stateEvent.PreviousStateValue} -> {stateEvent.StateValue} (reason={stateEvent.Reason})");
            }

            engine.StopScreen();
            AppLog.Pass("media: pu_screen_stop -> PU_OK");
            return failures.Count == before;
        }
        catch (Exception ex)
        {
            failures.Add("media: " + ex.Message);
            AppLog.Fail("media: " + ex);
            return false;
        }
        finally
        {
            if (engine is not null)
            {
                try
                {
                    engine.Dispose();
                    AppLog.Pass("media: pu_engine_destroy -> done");
                }
                catch (Exception ex)
                {
                    failures.Add("media dispose: " + ex.Message);
                }
            }
        }
    }

    private static async Task<SessionTokens?> RunNetworkChecksAsync(ClientOptions options, List<string> failures)
    {
        var before = failures.Count;
        try
        {
            if (string.IsNullOrWhiteSpace(options.BootstrapToken))
            {
                failures.Add("缺少 --bootstrap-token");
                return null;
            }
            if (string.IsNullOrWhiteSpace(options.InviteCode))
            {
                failures.Add("缺少 --invite-code");
                return null;
            }

            var baseUri = new Uri(options.Server);
            using var api = new PartyUpApi(baseUri);

            var alice = await api.BootstrapAsync(options.BootstrapToken, "alice", "selftest-pass-1");
            AppLog.Pass($"net: bootstrap admin alice ok (member={alice.Member.Id})");

            var bob = await api.RegisterWithInviteAsync(options.InviteCode, "bob", "selftest-pass-2");
            AppLog.Pass($"net: invite registration bob ok (member={bob.Member.Id})");

            await using var aliceConn = await PartyUpConnection.ConnectAsync(baseUri, alice.Token);
            await using var bobConn = await PartyUpConnection.ConnectAsync(baseUri, bob.Token);
            AppLog.Pass($"net: two websocket connections authenticated (alice={aliceConn.Member.Name}, bob={bobConn.Member.Name})");

            var channels = await aliceConn.ListChannelsAsync();
            var channel = channels.Channels.Count == 0
                ? await aliceConn.CreateChannelAsync(options.Channel)
                : channels.Channels[0];
            AppLog.Pass($"net: channel #{channel.Name} ({channel.Id}) ready");

            await aliceConn.SubscribeAsync(channel.Id);
            await bobConn.SubscribeAsync(channel.Id);
            AppLog.Pass("net: both clients subscribed to the channel");

            var presence = await aliceConn.WaitForPresenceAsync(channel.Id, 2, StepTimeout);
            AppLog.Pass($"net: presence shows {presence.Members.Count} member(s): {string.Join(", ", presence.Members.Select(m => m.Name))}");

            var aliceAck = await aliceConn.SendTextAsync(channel.Id, "hello from alice (selftest)");
            var bobSaw = await bobConn.WaitForTextAsync(m => m.Id == aliceAck.Id, StepTimeout);
            AppLog.Pass($"net: B received A's message seq={bobSaw.Seq} body=\"{bobSaw.Body}\"");
            var aliceEcho = await aliceConn.WaitForTextAsync(m => m.Id == aliceAck.Id, StepTimeout);
            AppLog.Pass($"net: A also received its own broadcast seq={aliceEcho.Seq}");

            var bobAck = await bobConn.SendTextAsync(channel.Id, "hello from bob (selftest)");
            var aliceSaw = await aliceConn.WaitForTextAsync(m => m.Id == bobAck.Id, StepTimeout);
            AppLog.Pass($"net: A received B's reply seq={aliceSaw.Seq} body=\"{aliceSaw.Body}\"");

            if (aliceAck.Seq != 1 || bobAck.Seq != 2)
            {
                failures.Add($"seq 不是 1/2 而是 {aliceAck.Seq}/{bobAck.Seq}");
            }

            var history = await bobConn.HistoryAsync(channel.Id, 0, 10);
            AppLog.Pass($"net: history returned {history.Count} message(s), seqs={string.Join(",", history.Select(m => m.Seq))}");
            if (history.Count < 2)
            {
                failures.Add($"history 只有 {history.Count} 条，期望 >= 2");
            }

            if (failures.Count != before)
            {
                return null;
            }
            return new SessionTokens(alice.Token, bob.Token, channel.Id, channel.Name);
        }
        catch (Exception ex)
        {
            failures.Add("net: " + ex.Message);
            AppLog.Fail("net: " + ex);
            return null;
        }
    }

    private readonly record struct SessionTokens(string AliceToken, string BobToken, string ChannelId, string ChannelName);
}
