using PartyUp.Client.Services;

namespace PartyUp.StrictReceive.Tests;

/// <summary>
/// B2b direct 共享者侧状态机的纯逻辑回归（无 native、无桌面、无网络）。
///
/// 断言的都是「严格隔离」的实质条件：
///   - 每个 viewer 一条独立发送会话，重复 arm 不重建传输；
///   - 关一条会话只关那一条（其它 viewer 的会话与 token 映射保留）；
///   - native 建会话失败时不登记任何映射（不能假装成功）；
///   - 旧会话的 token/信令不会落到新会话上；
///   - 服务端 receive_close 只关本地接收传输，不撤服务端 lease（避免双重撤销）。
/// </summary>
internal static class DirectPublishTests
{
    private static int _checks;
    private static int _failures;

    public static (int Checks, int Failures) Run()
    {
        ArmIsIdempotentPerSession();
        ArmForSameViewerReplacesOnlyThatViewer();
        CloseOnlyClosesThatSession();
        CloseAllClosesEveryTransportAndClearsTokens();
        FailedOpenRegistersNothing();
        InboundSignalsAreBoundToKnownSessions();
        ServerCloseOnlyClosesTheLocalReceive();
        Console.WriteLine($"  direct publish: {_checks} checks, {_failures} failure(s)");
        return (_checks, _failures);
    }

    private static void ArmIsIdempotentPerSession()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: arm is idempotent per session");
        var transport = new FakePublishTransport();
        var controller = new DirectPublishController(transport);
        var first = controller.Arm("viewer-1", "s-1", "nonce-1", 7);
        Check(first is not null, "Arm returned null for a valid viewer/session");
        var second = controller.Arm("viewer-1", "s-1", "nonce-1", 7);
        Check(second is not null && second.Token == first!.Token, "duplicate arm changed the token");
        Check(transport.Opened.Count == 1, $"duplicate arm opened {transport.Opened.Count} transports, want 1");
        Check(transport.Closed.Count == 0, "duplicate arm closed a transport");
        Check(controller.Count == 1, $"session count = {controller.Count}, want 1");
    }

    private static void ArmForSameViewerReplacesOnlyThatViewer()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: a new session replaces the old one for the same viewer");
        var transport = new FakePublishTransport();
        var controller = new DirectPublishController(transport);
        var other = controller.Arm("viewer-2", "s-other", "nonce-o", 3);
        var first = controller.Arm("viewer-1", "s-1", "nonce-1", 1);
        var second = controller.Arm("viewer-1", "s-2", "nonce-2", 2);
        Check(first is not null && second is not null && other is not null, "an Arm unexpectedly failed");
        Check(transport.Opened.Count == 3, $"transport opens = {transport.Opened.Count}, want 3");
        Check(transport.Closed.Count == 1 && transport.Closed[0] == "s-1",
            $"closing the superseded session: closed=[{string.Join(",", transport.Closed)}], want [s-1]");
        Check(!controller.TryResolveToken(first!.Token, out _), "old session token still resolves");
        Check(controller.TryResolveToken(second!.Token, out var resolved) && resolved.Id == "s-2",
            "new session token does not resolve to the new session");
        Check(controller.TryResolveToken(other!.Token, out var kept) && kept.Id == "s-other",
            "replacing one viewer's session dropped another viewer's session");
        Check(controller.Count == 2, $"session count = {controller.Count}, want 2");
    }

    private static void CloseOnlyClosesThatSession()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: close only closes the addressed session");
        var transport = new FakePublishTransport();
        var controller = new DirectPublishController(transport);
        var one = controller.Arm("viewer-1", "s-1", "n1", 1)!;
        var two = controller.Arm("viewer-2", "s-2", "n2", 2)!;
        Check(controller.Close("s-1"), "Close(s-1) reported nothing was closed");
        Check(transport.Closed.Count == 1 && transport.Closed[0] == "s-1",
            $"close calls = [{string.Join(",", transport.Closed)}], want [s-1]");
        Check(!controller.Close("s-1"), "second Close(s-1) was not idempotent");
        Check(transport.Closed.Count == 1, "idempotent close called the transport twice");
        Check(!controller.TryResolveToken(one.Token, out _), "closed session token still resolves");
        Check(controller.TryResolveToken(two.Token, out var kept) && kept.Id == "s-2",
            "closing one session dropped another session");
        Check(!controller.Close("never-opened"), "Close of an unknown session reported success");
    }

    private static void CloseAllClosesEveryTransportAndClearsTokens()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: CloseAll closes every transport exactly once");
        var transport = new FakePublishTransport();
        var controller = new DirectPublishController(transport);
        var one = controller.Arm("viewer-1", "s-1", "n1", 1)!;
        var two = controller.Arm("viewer-2", "s-2", "n2", 2)!;
        controller.CloseAll();
        Check(transport.Closed.Count == 2, $"CloseAll closed {transport.Closed.Count} transports, want 2");
        Check(controller.Count == 0, $"session count after CloseAll = {controller.Count}, want 0");
        Check(!controller.TryResolveToken(one.Token, out _) && !controller.TryResolveToken(two.Token, out _),
            "CloseAll left token mappings behind");
        controller.CloseAll();
        Check(transport.Closed.Count == 2, "second CloseAll re-closed transports");
    }

    private static void FailedOpenRegistersNothing()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: native open failure registers nothing");
        var transport = new FakePublishTransport
        {
            OpenHandler = _ => throw new InvalidOperationException("native refused"),
        };
        var controller = new DirectPublishController(transport);
        var session = controller.Arm("viewer-1", "s-1", "n1", 1);
        Check(session is null, "Arm returned a session even though the native open failed");
        Check(controller.Count == 0, $"session count = {controller.Count}, want 0");
        Check(transport.Closed.Count == 0, "failed open resulted in a close call");
    }

    private static void InboundSignalsAreBoundToKnownSessions()
    {
        Console.WriteLine();
        Console.WriteLine("== direct publish: inbound signals are bound to known sessions");
        var transport = new FakePublishTransport();
        var controller = new DirectPublishController(transport);
        controller.Arm("viewer-1", "s-1", "n1", 1);
        var payload = new byte[] { 1, 2, 3 };
        Check(!controller.TryApplyInbound("s-unknown", payload, out _),
            "signal for an unknown session was accepted");
        Check(transport.Signaled.Count == 0, "unknown session signal reached the native transport");
        Check(controller.TryApplyInbound("s-1", payload, out var rc) && rc == 0,
            "signal for a live session was rejected");
        Check(transport.Signaled.Count == 1 && transport.Signaled[0] == "s-1",
            "live session signal was not routed to that session");
        controller.Close("s-1");
        Check(!controller.TryApplyInbound("s-1", payload, out _),
            "signal for a closed session was accepted (would leak into a closed native session)");
    }

    private static void ServerCloseOnlyClosesTheLocalReceive()
    {
        Console.WriteLine();
        Console.WriteLine("== strict receive: receive_close closes locally without revoking the lease");
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var watch = controller.WatchAsync("sharer").GetAwaiter().GetResult();
        Check(watch.Outcome == StrictWatchOutcome.Armed, $"watch outcome = {watch.Outcome}");
        var session = watch.SessionId!;
        Check(controller.CloseRemote(session), "CloseRemote reported no session to close");
        Check(transport.Closed.Count == 1 && transport.Closed[0] == session,
            $"native close calls = [{string.Join(",", transport.Closed)}], want [{session}]");
        Check(transport.Revoked.Count == 0,
            $"CloseRemote revoked {transport.Revoked.Count} lease(s); the server already closed it");
        Check(controller.SessionId is null, "CloseRemote left the session id set");
        Check(!controller.AcceptInbound(session, "nonce", out _),
            "a closed receive session still accepts inbound signals");
        Check(!controller.CloseRemote(session), "second CloseRemote was not idempotent");
        Check(transport.Closed.Count == 1, "idempotent CloseRemote closed the native session twice");
    }

    private static void Check(bool condition, string message)
    {
        _checks++;
        if (!condition)
        {
            _failures++;
            Console.WriteLine("FAIL: " + message);
        }
    }
}
