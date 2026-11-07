using PartyUp.Client.Services;

namespace PartyUp.StrictReceive.Tests;

/// <summary>
/// 可控的严格接收传输 fake：它实现生产代码使用的同一条 <see cref="IStrictReceiveTransport"/>
/// seam，并按**真实协议语义**建模服务端的接收 lease：
///   - lease 的身份是 receive_id（会话），target 只是 lease 上的属性，两者取值不重合；
///   - 撤销只能按会话 id 进行——传 sharer/target 之类从未 Open 过的字符串会被记为
///     misuse（生产 controller 的撤租一旦误参就会留下远端点，测试必须能看见）；
///   - lease 在 watch ACK 返回时才登记，因此可以精确复现「stop 的撤销先到、lease
///     后建」的迟到 ACK 时序。
/// 测试通过 TaskCompletionSource 精确控制「watch ACK 何时返回」，不靠 sleep 猜。
/// </summary>
internal sealed class FakeTransport : IStrictReceiveTransport
{
    private readonly object _lock = new();
    private readonly List<string> _opened = new();
    private readonly List<string> _closed = new();
    private readonly List<string> _revoked = new();
    private readonly List<string> _revokeStarted = new();
    private readonly List<string> _misuse = new();
    private readonly Dictionary<string, string> _leases = new(StringComparer.Ordinal);
    private uint _nextToken = 1;

    /// <summary>如果设置，WatchAsync 交给它决定何时返回；否则立即返回合法 ACK。</summary>
    public Func<string, string, Task<StrictWatchAck>>? WatchHandler { get; set; }

    /// <summary>如果设置，在 Open 内执行（用于把 watch 卡在「native 会话已建」这一刻）。</summary>
    public Action<string>? OpenHandler { get; set; }

    /// <summary>如果设置，撤销在它完成后才真正移除 lease（模拟迟到/延迟的撤租请求）。</summary>
    public Func<string, Task>? RevokeHandler { get; set; }

    public int OpenCalls { get { lock (_lock) { return _opened.Count; } } }
    public IReadOnlyList<string> Opened { get { lock (_lock) { return _opened.ToArray(); } } }
    public IReadOnlyList<string> Closed { get { lock (_lock) { return _closed.ToArray(); } } }
    /// <summary>成功按会话撤销的 receive_id（按调用顺序，可能重复——幂等重撤）。</summary>
    public IReadOnlyList<string> Revoked { get { lock (_lock) { return _revoked.ToArray(); } } }
    /// <summary>被当作会话 id 传进来、但从未 Open 过的值（旧 bug：把 sharer 当 session）。</summary>
    public IReadOnlyList<string> Misuse { get { lock (_lock) { return _misuse.ToArray(); } } }
    public int LeaseCount { get { lock (_lock) { return _leases.Count; } } }
    public IReadOnlyList<string> Leased { get { lock (_lock) { return _leases.Keys.ToArray(); } } }
    public IReadOnlyList<string> RevokeStarted { get { lock (_lock) { return _revokeStarted.ToArray(); } } }

    public bool HasLease(string sessionId)
    {
        lock (_lock)
        {
            return _leases.ContainsKey(sessionId);
        }
    }

    public uint Open(string receiveId)
    {
        lock (_lock)
        {
            _opened.Add(receiveId);
        }
        OpenHandler?.Invoke(receiveId);
        lock (_lock)
        {
            return _nextToken++;
        }
    }

    public void Close(string receiveId)
    {
        lock (_lock)
        {
            _closed.Add(receiveId);
        }
    }

    public async Task<StrictWatchAck> WatchAsync(string sharerId, string receiveId, CancellationToken ct)
    {
        var handler = WatchHandler;
        var ack = handler is not null ? await handler(sharerId, receiveId) : ValidAck(receiveId, sharerId);
        lock (_lock)
        {
            // 服务端在回 ACK 前已经建好 lease；fake 在 handler 完成后登记，正好模拟
            // 「stop 先撤（此时无 lease）→ watch 晚提交 → 迟到 ACK」的晚建顺序。
            _leases[receiveId] = sharerId;
        }
        return ack;
    }

    public async Task RevokeSessionAsync(StrictReceiveSession session, CancellationToken ct)
    {
        lock (_lock)
        {
            if (!_opened.Contains(session.Id))
            {
                _misuse.Add(session.Id);
                return;
            }
            _revokeStarted.Add(session.Id);
        }
        var handler = RevokeHandler;
        if (handler is not null)
        {
            await handler(session.Id).ConfigureAwait(false);
        }
        lock (_lock)
        {
            _revoked.Add(session.Id);
            _leases.Remove(session.Id);
        }
    }

    public static StrictWatchAck ValidAck(string receiveId, string sharerId = "sharer") =>
        new(true, receiveId, "nonce-" + receiveId[..Math.Min(8, receiveId.Length)], sharerId, "relay", 1);
}

internal sealed class PendingWatch
{
    public TaskCompletionSource<StrictWatchAck> Ack { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task<StrictWatchResult> Watch { get; set; } = null!;
    public string ReceiveId { get; set; } = string.Empty;
}

internal static class Program
{
    private static int _failures;
    private static int _checks;

    private static async Task<int> Main()
    {
        await Run("stop closes the receive immediately without waiting for the in-flight ACK",
            StopClosesImmediatelyWithoutWaitingForAck);
        await Run("late ACK after stop cannot re-arm the receive session", LateAckAfterStopCannotRearm);
        await Run("stop before the ACK still revokes the lease the server creates later",
            StopBeforeAckRevokesLateLease);
        await Run("late ACK from a superseded watch does not clear the new selection",
            LateAckFromOldWatchDoesNotClearNewSelection);
        await Run("failed ACK rollback leaves no state and the next watch arms cleanly",
            FailedAckLeavesNoStateAndNextWatchArms);
        await Run("watch failure revokes the exact session and leaves no remote lease",
            WatchFailureRevokesExactSession);
        await Run("a late revoke from the old stop cannot clear a new session with the same target",
            LateRevokeDoesNotClearNewSessionWithSameTarget);
        await Run("dispose/connection loss clears the pending session and rejects its late ACK",
            DisposeClearsPendingSession);
        await Run("destroying the controller during Open still closes the new native session exactly once",
            CloseAllDuringOpenClosesNewSession);
        await Run("inbound/outbound signals are bound to the current session only",
            SignalBindingUsesCurrentSessionOnly);
        await Run("signals are applied to native only while the session is still current",
            SignalApplyIsGatedBySessionLifetime);
        await Run("concurrent stop and watch do not deadlock", ConcurrentStopAndWatchDoNotDeadlock);

        Console.WriteLine();
        var publish = DirectPublishTests.Run();
        _checks += publish.Checks;
        _failures += publish.Failures;

        Console.WriteLine();
        if (_failures == 0)
        {
            Console.WriteLine($"PASS: strict receive controller ({_checks} checks)");
            return 0;
        }
        Console.WriteLine($"FAIL: strict receive controller ({_failures} failure(s) / {_checks} checks)");
        return 1;
    }

    private static async Task Run(string name, Func<Task> test)
    {
        Console.WriteLine();
        Console.WriteLine("== " + name);
        try
        {
            await test();
        }
        catch (Exception ex)
        {
            Fail($"unhandled exception: {ex.GetType().Name}: {ex.Message}");
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

    private static async Task<T> WithTimeout<T>(Task<T> task, int ms, string what)
    {
        var finished = await Task.WhenAny(task, Task.Delay(ms));
        if (finished != task)
        {
            throw new TimeoutException($"{what} did not finish within {ms}ms");
        }
        return await task;
    }

    private static async Task WithTimeout(Task task, int ms, string what)
    {
        var finished = await Task.WhenAny(task, Task.Delay(ms));
        if (finished != task)
        {
            throw new TimeoutException($"{what} did not finish within {ms}ms");
        }
        await task;
    }

    /// <summary>
    /// 让第一次 watch 停在「已经发出、等 ACK」：后续 watch 走默认立即成功，避免
    /// 第一个 ACK 被第二个 watch 复用。
    /// </summary>
    private static PendingWatch StartPendingWatch(
        FakeTransport transport, StrictReceiveController controller, string sharer)
    {
        var pending = new PendingWatch();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var first = true;
        transport.WatchHandler = (sharerId, receiveId) =>
        {
            if (first)
            {
                first = false;
                pending.ReceiveId = receiveId;
                started.TrySetResult();
                return pending.Ack.Task;
            }
            return Task.FromResult(FakeTransport.ValidAck(receiveId, sharerId));
        };
        pending.Watch = controller.WatchAsync(sharer);
        if (!started.Task.Wait(2000))
        {
            throw new TimeoutException("watch request was not issued");
        }
        return pending;
    }

    private static async Task StopClosesImmediatelyWithoutWaitingForAck()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "sharer");

        var stop = controller.StopAsync();
        await WithTimeout(stop, 1000, "StopAsync while watch ACK is pending");
        Check(await stop, "StopAsync reported it closed an armed receive session");
        Check(transport.Closed.Count == 1, $"transport.Close calls = {transport.Closed.Count}, want 1");
        Check(transport.Closed.Count == 1 && transport.Closed[0] == pending.ReceiveId,
            "stop closed the session opened by the pending watch");
        Check(controller.SessionId is null, "controller still reports an armed session after stop");

        // ACK 现在才回来：必须判定为 Superseded，且不得重新 arm。
        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId));
        var result = await WithTimeout(pending.Watch, 2000, "watch after late ACK");
        Check(result.Outcome == StrictWatchOutcome.Superseded,
            $"late ACK outcome = {result.Outcome}, want Superseded");
        Check(controller.SessionId is null, "late ACK re-armed the receive session");
        Check(transport.Closed.Count == 1, $"late ACK closed {transport.Closed.Count} sessions, want 1 (exactly once)");
        // 远端撤销按会话精确发生：stop 时无 lease，迟到 ACK 后 lease 才建，再撤一次。
        Check(transport.LeaseCount == 0, $"late ACK left {transport.LeaseCount} remote lease(s), want 0");
        Check(transport.Revoked.Count(id => id == pending.ReceiveId) >= 1,
            "late ACK did not revoke the session-scoped lease");
        Check(transport.Misuse.Count == 0,
            "transport saw a non-session value passed as a session: " + string.Join(",", transport.Misuse));
    }

    private static async Task LateAckAfterStopCannotRearm()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "sharer");

        await controller.StopAsync();
        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId));
        var result = await WithTimeout(pending.Watch, 2000, "watch after stop+late ACK");
        Check(result.Outcome == StrictWatchOutcome.Superseded, $"outcome = {result.Outcome}, want Superseded");

        // 迟到 ACK 之后，旧 session 的信令必须被拒绝（不是只清 UI 状态）。
        Check(!controller.AcceptInbound(pending.ReceiveId, "nonce", out _),
            "signal for the stopped session was accepted");
    }

    /// <summary>
    /// stop 的撤销可能先于服务端提交/建 lease：此时本地关闭与远端撤销都发生了，
    /// 但 lease 还不存在。迟到的 ACK 证明服务端后来建了 lease —— 必须再按会话
    /// 精确撤销一次，不能因为 stillOwned=false 就跳过，否则远端点永久残留。
    /// </summary>
    private static async Task StopBeforeAckRevokesLateLease()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "target-with-late-lease");

        await WithTimeout(controller.StopAsync(), 1000, "stop before ACK");
        Check(transport.LeaseCount == 0, "fake created a lease before the ACK, test cannot model the late-lease race");
        Check(transport.RevokeStarted.Contains(pending.ReceiveId),
            "stop did not even attempt a session-scoped revoke before the ACK");

        // ACK 此时才返回（服务端在 stop 的 unwatch 之后提交了 lease）。
        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId, "target-with-late-lease"));
        var result = await WithTimeout(pending.Watch, 2000, "watch after stop+late ACK");
        Check(result.Outcome == StrictWatchOutcome.Superseded, $"outcome = {result.Outcome}, want Superseded");
        Check(transport.LeaseCount == 0, $"late ACK left {transport.LeaseCount} remote lease(s), want 0");
        Check(transport.Revoked.Count(id => id == pending.ReceiveId) >= 2,
            $"late ACK did not revoke again (revoked: {string.Join(",", transport.Revoked)})");
        Check(transport.Misuse.Count == 0,
            "revoke used a non-session value: " + string.Join(",", transport.Misuse));
    }


    private static async Task LateAckFromOldWatchDoesNotClearNewSelection()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "sharer-a");

        // 第二次 watch：会等 _watchGate（第一次还挂着）。它必须先完成，才能证明
        // 「旧 ACK 回滚」不会误清新选择。
        var second = controller.WatchAsync("sharer-b");
        await Task.Delay(100);

        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId));
        var firstResult = await WithTimeout(pending.Watch, 2000, "first watch");
        Check(firstResult.Outcome == StrictWatchOutcome.Superseded,
            $"superseded watch outcome = {firstResult.Outcome}, want Superseded");
        var secondResult = await WithTimeout(second, 2000, "second watch");
        Check(secondResult.Outcome == StrictWatchOutcome.Armed,
            $"second watch outcome = {secondResult.Outcome}, want Armed");
        Check(controller.SessionId == secondResult.SessionId,
            "old watch's rollback cleared the new selection");

        Check(controller.AcceptInbound(secondResult.SessionId!, secondResult.Ack!.Nonce, out _),
            "new selection cannot accept its own session signal");
        Check(!controller.AcceptInbound(pending.ReceiveId, "old-nonce", out _),
            "signal for the superseded session was accepted");
    }

    private static async Task FailedAckLeavesNoStateAndNextWatchArms()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        transport.WatchHandler = (_, _) =>
            Task.FromResult(new StrictWatchAck(false, "wrong", string.Empty, "sharer-a", "relay", 1));

        // 服务端没有按严格合同返回：必须回滚（关会话 + 撤销 lease），绝不 arm。
        var failed = await WithTimeout(controller.WatchAsync("sharer-a"), 2000, "invalid ACK watch");
        Check(failed.Outcome == StrictWatchOutcome.Failed,
            $"invalid ACK outcome = {failed.Outcome}, want Failed");
        Check(controller.SessionId is null, "invalid ACK armed a receive session");
        Check(transport.Closed.Count == 1, $"rollback closed {transport.Closed.Count} sessions, want 1");
        Check(transport.Revoked.Contains(failed.SessionId!),
            $"rollback did not revoke the exact session (revoked: {string.Join(",", transport.Revoked)})");
        Check(!transport.HasLease(failed.SessionId!),
            "invalid ACK rollback left the remote lease behind");
        Check(transport.Misuse.Count == 0,
            "revoke used a non-session value: " + string.Join(",", transport.Misuse));
        Check(!controller.AcceptInbound(failed.SessionId!, "n", out _),
            "signal for the failed session was accepted");

        // 下一次 watch 必须干净地 arm，且不因上一次回滚被清掉。
        transport.WatchHandler = null;
        var armed = await WithTimeout(controller.WatchAsync("sharer-b"), 2000, "next watch after failure");
        Check(armed.Outcome == StrictWatchOutcome.Armed,
            $"next watch outcome = {armed.Outcome}, want Armed");
        Check(controller.SessionId == armed.SessionId, "failed rollback left a stale selection");
        Check(transport.Closed.Count == 1, "next watch re-closed the already rolled back session");
    }

    /// <summary>
    /// watch 请求本身失败（超时/传输异常）时，服务端可能已经处理并建好 lease。
    /// 回滚必须按 open 出来的会话 id 精确撤销，不能因为本地已 Detach 就跳过远端。
    /// </summary>
    private static async Task WatchFailureRevokesExactSession()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        transport.WatchHandler = (_, _) => throw new InvalidOperationException("watch ACK timeout");

        var failed = await WithTimeout(controller.WatchAsync("sharer-a"), 2000, "failed watch");
        Check(failed.Outcome == StrictWatchOutcome.Failed, $"outcome = {failed.Outcome}, want Failed");
        Check(failed.SessionId is not null && failed.SessionId.Length > 0, "failed watch did not report its session");
        var session = failed.SessionId!;
        Check(transport.Closed.Contains(session), "failed watch did not close its native session");
        Check(transport.Revoked.Contains(session),
            $"failed watch did not revoke by session (revoked: {string.Join(",", transport.Revoked)})");
        Check(transport.LeaseCount == 0, $"failed watch left {transport.LeaseCount} remote lease(s)");
        Check(transport.Misuse.Count == 0,
            "failed watch passed a non-session value to revoke: " + string.Join(",", transport.Misuse));
        Check(controller.SessionId is null, "failed watch armed a session");
    }

    /// <summary>
    /// 旧 Stop 的撤租请求可能晚到（网络/竞态）。它只能按会话撤销自己那一条，
    /// 即使新会话的 target 与旧会话完全相同，也绝不能清掉新 lease/新选择。
    /// </summary>
    private static async Task LateRevokeDoesNotClearNewSessionWithSameTarget()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        const string target = "same-target";

        var first = await WithTimeout(controller.WatchAsync(target), 2000, "first watch");
        Check(first.Outcome == StrictWatchOutcome.Armed, $"first watch = {first.Outcome}");
        var firstSession = first.SessionId!;

        // 让旧 Stop 的撤销停在「请求已发出、尚未生效」：新 watch 借此先完成。
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        transport.RevokeHandler = async _ => await release.Task.ConfigureAwait(false);
        var stop = controller.StopAsync();
        var deadline = DateTime.UtcNow.AddSeconds(2);
        while (!transport.RevokeStarted.Contains(firstSession) && DateTime.UtcNow < deadline)
        {
            await Task.Delay(10);
        }
        Check(transport.RevokeStarted.Contains(firstSession), "old stop never issued its session revoke");

        var second = await WithTimeout(controller.WatchAsync(target), 2000, "second watch while old revoke is pending");
        Check(second.Outcome == StrictWatchOutcome.Armed, $"second watch = {second.Outcome}");
        var secondSession = second.SessionId!;
        Check(secondSession != firstSession, "new watch reused the old receive session id");
        Check(transport.HasLease(secondSession), "new watch did not create a remote lease");

        release.SetResult(); // 旧撤销现在才真正生效
        await WithTimeout(stop, 2000, "old stop finishing after release");

        Check(controller.SessionId == secondSession, "old stop's late revoke cleared the new selection");
        Check(second.Outcome == StrictWatchOutcome.Armed, "new selection was superseded by the old stop");
        Check(transport.HasLease(secondSession), "old stop's late revoke cleared the new session's lease");
        Check(transport.HasLease(firstSession) == false, "old session's lease was not revoked");
        Check(transport.Misuse.Count == 0,
            "late revoke used a non-session value: " + string.Join(",", transport.Misuse));
    }


    private static async Task DisposeClearsPendingSession()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "sharer");

        controller.CloseAll(); // Dispose / 连接丢失
        Check(controller.SessionId is null, "CloseAll left an armed session");
        Check(transport.Closed.Count == 1, $"CloseAll closed {transport.Closed.Count} sessions, want 1");
        Check(transport.Misuse.Count == 0, "CloseAll passed a non-session value somewhere");

        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId));
        var result = await WithTimeout(pending.Watch, 2000, "watch after CloseAll");
        Check(result.Outcome == StrictWatchOutcome.Superseded,
            $"watch after CloseAll = {result.Outcome}, want Superseded");
        Check(controller.SessionId is null, "late ACK re-armed after CloseAll");
        Check(transport.Closed.Count == 1, "late ACK re-closed the session after CloseAll (want exactly once)");
    }

    /// <summary>
    /// Dispose/断线可能发生在 WatchAsync 的 native Open 期间。生命周期门保证：
    /// CloseAll 会等这次 Open 完成，并把刚登记的新会话一起关掉；之后迟到的 ACK
    /// 只能被丢弃并按会话撤销它可能晚建的远端 lease —— 不会留下孤儿 native 会话。
    /// </summary>
    private static async Task CloseAllDuringOpenClosesNewSession()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var openStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var openRelease = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        transport.OpenHandler = _ =>
        {
            openStarted.TrySetResult();
            openRelease.Task.Wait(2000);
        };
        // ACK 在 CloseAll 完成之后才回来：模拟「连接已断开、服务端 ACK 还在路上」。
        var ackGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        transport.WatchHandler = async (sharerId, receiveId) =>
        {
            await ackGate.Task.ConfigureAwait(false);
            return FakeTransport.ValidAck(receiveId, sharerId);
        };

        var watch = Task.Run(() => controller.WatchAsync("sharer"));
        await WithTimeout(openStarted.Task, 2000, "native Open start");
        var close = Task.Run(controller.CloseAll);
        await Task.Delay(100);
        Check(!close.IsCompleted,
            "CloseAll returned while native Open was still running (session would escape the destroy)");

        openRelease.SetResult();
        await WithTimeout(close, 2000, "CloseAll after native Open");
        Check(transport.Closed.Count == 1, $"CloseAll closed {transport.Closed.Count} sessions, want 1");
        Check(controller.SessionId is null, "a session was armed after CloseAll");

        ackGate.SetResult(); // 断线后才到的 ACK
        var result = await WithTimeout(watch, 3000, "watch after CloseAll during Open");
        Check(result.Outcome == StrictWatchOutcome.Superseded,
            $"watch after CloseAll = {result.Outcome}, want Superseded");
        Check(transport.LeaseCount == 0, $"late watch left {transport.LeaseCount} remote lease(s) after CloseAll");
        Check(transport.Closed.Count == 1, "watch after CloseAll closed the session twice");
    }

    private static async Task SignalBindingUsesCurrentSessionOnly()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var result = await WithTimeout(controller.WatchAsync("sharer"), 2000, "watch");
        Check(result.Outcome == StrictWatchOutcome.Armed, $"watch outcome = {result.Outcome}");
        var session = result.SessionId!;
        var nonce = result.Ack!.Nonce;

        Check(controller.AcceptInbound(session, nonce, out var effective), "current session offer was dropped");
        Check(effective == nonce, $"effective nonce = {effective}, want {nonce}");

        Check(controller.TryResolveToken(1, out var outSession, out var outNonce),
            "native token for the armed session did not resolve");
        Check(outSession == session && outNonce == nonce,
            $"token resolved to ({outSession}, {outNonce}), want ({session}, {nonce})");
        Check(!controller.TryResolveToken(999, out _, out _), "unknown native token resolved");

        await controller.StopAsync();
        Check(!controller.AcceptInbound(session, nonce, out _), "signal accepted after stop");
        Check(!controller.TryResolveToken(1, out _, out _), "native token resolved after stop");
    }

    /// <summary>
    /// 归属检查（AcceptInbound）之后、真正调用 native 之前，会话可能已被 stop/Dispose
    /// 关闭。native 生命周期门必须拒绝这条已经过期的信令，不能把它灌进已关闭的会话。
    /// </summary>
    private static async Task SignalApplyIsGatedBySessionLifetime()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var result = await WithTimeout(controller.WatchAsync("sharer"), 2000, "watch");
        Check(result.Outcome == StrictWatchOutcome.Armed, $"watch outcome = {result.Outcome}");
        var session = result.SessionId!;

        var applied = false;
        Check(controller.TryApplyToSession(session, () => applied = true),
            "an armed session could not apply its signal");
        Check(applied, "native apply callback was not invoked for the armed session");

        await controller.StopAsync();
        applied = false;
        Check(!controller.TryApplyToSession(session, () => applied = true),
            "signal for a closed session passed the native lifetime gate");
        Check(!applied, "signal for a closed session reached the native callback");
    }

    private static async Task ConcurrentStopAndWatchDoNotDeadlock()
    {
        var transport = new FakeTransport();
        var controller = new StrictReceiveController(transport);
        var pending = StartPendingWatch(transport, controller, "sharer");

        // 同一时刻 stop 两次 + 新 watch 一次：都不许卡在 _watchGate 上。
        var stop1 = controller.StopAsync();
        var stop2 = controller.StopAsync();
        var watch2 = controller.WatchAsync("sharer-2");
        await WithTimeout(Task.WhenAll(stop1, stop2), 1500, "concurrent stops");
        pending.Ack.SetResult(FakeTransport.ValidAck(pending.ReceiveId));
        await WithTimeout(pending.Watch, 1500, "superseded watch");
        await WithTimeout(watch2, 3000, "watch issued while stopping");
        Check(controller.SessionId is not null, "watch issued while stopping never armed");
    }
}
