namespace PartyUp.Client.Services;

/// <summary>
/// 严格接收传输（B2）的最小依赖面：native 接收会话 + screen.watch/unwatch。
///
/// 生产实现是 <c>MediaEngine</c> + <c>PartyUpConnection</c>；回归测试用可控延迟/
/// 可控失败的 fake 实现同一条 seam——两边跑的是**同一份状态机代码**，不是复制。
/// 所有方法都可能从不同线程调用（WS 接收线程、媒体泵线程、UI/异步 stop）。
/// </summary>
internal interface IStrictReceiveTransport
{
    /// <summary>建一条 receive-only 会话并返回 native token（必须在发 watch 之前）。</summary>
    uint Open(string receiveId);

    /// <summary>只关闭这条接收会话；幂等，不碰发布/语音。</summary>
    void Close(string receiveId);

    /// <summary>发 screen.watch（带 receive_id）并等待服务端 ACK。</summary>
    Task<StrictWatchAck> WatchAsync(string sharerId, string receiveId, CancellationToken ct);

    /// <summary>
    /// 按**会话**精确撤销服务端接收 lease（幂等）。
    ///
    /// 参数是会话身份，不是 sharer/target：目标相同不代表同一会话，旧 Stop 的
    /// 迟到撤销绝不能停掉同目标的新会话。客户端在发 watch 之前就持有 receive_id，
    /// 所以没拿到 ACK 时也能精确撤销。失败不得影响本地已经完成的停收。
    /// </summary>
    Task RevokeSessionAsync(StrictReceiveSession session, CancellationToken ct);
}

/// <summary>
/// 一次严格接收会话的不可混淆身份（服务端 lease 按它撤销）。用独立类型而不是
/// <see cref="string"/>，避免把 sharer_id 误当成会话 id 传给撤租接口。
/// </summary>
internal readonly record struct StrictReceiveSession(string Id)
{
    public override string ToString() => Id;
}

/// <summary>服务端对一次严格 watch 的应答（只保留状态机需要的字段）。</summary>
internal sealed record StrictWatchAck(
    bool Isolated,
    string SessionId,
    string Nonce,
    string SharerId,
    string Transport,
    ulong Generation);

internal enum StrictWatchOutcome
{
    /// <summary>接收会话已建立并 arm。</summary>
    Armed,
    /// <summary>在等 ACK 期间被 stop/新 watch 取代；已关闭会话并撤销服务端 lease。</summary>
    Superseded,
    /// <summary>建会话/ACK 失败，或服务端没有按严格合同返回。</summary>
    Failed,
}

/// <summary>一次 watch 的结果（供调用方记录日志/错误，不暴露内部状态）。</summary>
internal sealed record StrictWatchResult(
    StrictWatchOutcome Outcome,
    StrictWatchAck? Ack,
    string? SessionId,
    string? Error);

/// <summary>
/// 严格接收会话的状态机（B2）。它只做三件事：
///
///   1. 每次 watch 都推进本地代次；等 ACK 期间被 stop/新 watch 取代时，迟到的 ACK
///      必须关闭刚建的会话并撤销服务端 lease，**绝不重新 arm**；
///   2. stop 立即关闭当前接收会话（不等在飞的 watch/ACK 门），用户点停止后不再收帧；
///   3. 入站 offer/answer/ICE 与出站 answer 只有命中**当前**会话 id 才放行；旧会话
///      的迟到信令被丢弃，不会灌进新接收器。
///
/// 线程模型：所有状态（三个映射、当前 session/nonce、代次）都在 <c>_lock</c> 下读写；
/// native/WS 调用一律在锁外做，避免持锁等 ICE/网络导致 stop 或信令线程被卡住。
/// <c>_nativeGate</c> 只保护「登记会话 ↔ 关闭 native 会话」这一对本地操作，
/// <c>_watchGate</c> 只串行化 watch 往返，stop 从不等待它（这正是旧实现让用户停看
/// 后仍收帧到 ACK 超时的原因）。
/// </summary>
internal sealed class StrictReceiveController
{
    private readonly object _lock = new();
    /// <summary>native 生命周期门：见 <see cref="TryApplyToSession"/> 与 WatchAsync。</summary>
    private readonly object _nativeGate = new();
    private readonly SemaphoreSlim _watchGate = new(1, 1);
    private readonly IStrictReceiveTransport _transport;

    private long _generation;
    private string? _sessionId;
    private string? _sharerId;
    private string? _nonce;
    private ulong _serverGeneration;
    private readonly Dictionary<uint, string> _sessionByToken = new();
    private readonly Dictionary<string, uint> _tokenBySession = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _nonceBySession = new(StringComparer.Ordinal);

    public StrictReceiveController(IStrictReceiveTransport transport)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
    }

    /// <summary>当前已 arm 的接收会话 id（没有则 null）。</summary>
    public string? SessionId
    {
        get { lock (_lock) { return _sessionId; } }
    }

    /// <summary>当前正在观看的共享者（没有则 null）。</summary>
    public string? SharerId
    {
        get { lock (_lock) { return _sharerId; } }
    }

    /// <summary>当前会话的 nonce（answer/ICE 回带用；没有则 null）。</summary>
    public string? Nonce
    {
        get { lock (_lock) { return _nonce; } }
    }

    /// <summary>当前会话的服务端代次（direct 信令回带用；没有则 0）。</summary>
    public ulong ServerGeneration
    {
        get { lock (_lock) { return _serverGeneration; } }
    }

    /// <summary>本地代次（每次 watch/stop/clear 递增；测试可据此断言迟到 ACK 被丢弃）。</summary>
    public long Generation
    {
        get { lock (_lock) { return _generation; } }
    }

    /// <summary>
    /// 建立一次严格 watch。返回 Armed 时接收会话已可用；Superseded/Failed 时
    /// 不会留下任何 arm 状态（会话与映射都已清掉）。
    /// </summary>
    public async Task<StrictWatchResult> WatchAsync(string sharerId, CancellationToken ct = default)
    {
        long gen;
        lock (_lock)
        {
            gen = ++_generation;
        }
        await _watchGate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            lock (_lock)
            {
                if (gen != _generation)
                {
                    // 在等本地门时已被 stop/新 watch 取代：连 watch 都不发。
                    return new StrictWatchResult(StrictWatchOutcome.Superseded, null, null, null);
                }
            }

            var receiveId = Guid.NewGuid().ToString("N");
            uint token;
            // native 生命周期门：Open + 登记必须与 stop/CloseAll 的「推进代次 + 摘除
            // 状态 + 关 native」互斥。stop 先完成时这里看到新代次，不会再把会话建到
            // 已销毁的 native 状态上；本段先完成时 stop 会等它登记完并把会话一起关掉
            //（本地 exactly-once 由映射保证）。
            lock (_nativeGate)
            {
                lock (_lock)
                {
                    if (gen != _generation)
                    {
                        // 在等门期间已被 stop/新 watch 取代：连 native 会话都不建。
                        return new StrictWatchResult(StrictWatchOutcome.Superseded, null, null, null);
                    }
                }
                try
                {
                    token = _transport.Open(receiveId);
                }
                catch (Exception ex)
                {
                    return new StrictWatchResult(StrictWatchOutcome.Failed, null, null,
                        "pu_screen_receive_open failed: " + ex.Message);
                }
                lock (_lock)
                {
                    _sessionByToken[token] = receiveId;
                    _tokenBySession[receiveId] = token;
                }
            }

            StrictWatchAck ack;
            try
            {
                ack = await _transport.WatchAsync(sharerId, receiveId, ct).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                string? closed = Detach(receiveId);
                if (closed is not null)
                {
                    CloseOutsideLock(closed);
                }
                // 请求可能已经到达服务端（只是 ACK 丢/超时）：无论本地是否已被
                // stop 摘走，都要按会话精确撤销一次；服务端对未知会话幂等。
                await RevokeSessionAsync(receiveId).ConfigureAwait(false);
                return new StrictWatchResult(StrictWatchOutcome.Failed, null, receiveId, ex.Message);
            }

            bool superseded;
            bool invalid;
            bool stillOwned;
            lock (_lock)
            {
                superseded = gen != _generation;
                invalid = !ack.Isolated ||
                          string.IsNullOrEmpty(ack.SessionId) ||
                          !string.Equals(ack.SessionId, receiveId, StringComparison.Ordinal) ||
                          string.IsNullOrEmpty(ack.Nonce);
                stillOwned = _tokenBySession.ContainsKey(receiveId);
                if (superseded || invalid)
                {
                    DetachLocked(receiveId);
                }
                else
                {
                    _sessionId = receiveId;
                    _sharerId = ack.SharerId;
                    _nonce = ack.Nonce;
                    _serverGeneration = ack.Generation;
                    _nonceBySession[receiveId] = ack.Nonce;
                }
            }
            if (superseded || invalid)
            {
                // 本地 exactly-once 关闭与远端幂等撤租是两件事：
                // - stillOwned 只决定本地是否还需要关（stop/CloseAll 可能已经关过）；
                // - 远端必须**始终**按 receiveId 精确撤销。stop 的 unwatch 可能先于
                //   服务端提交 lease（或干脆没建），迟到的 ACK 之后不再撤一次就会留下
                //   无人关闭的远端点；重复撤销在服务端是幂等的。
                if (stillOwned)
                {
                    CloseOutsideLock(receiveId);
                }
                await RevokeSessionAsync(receiveId).ConfigureAwait(false);
                if (superseded)
                {
                    return new StrictWatchResult(StrictWatchOutcome.Superseded, ack, receiveId, null);
                }
                return new StrictWatchResult(StrictWatchOutcome.Failed, ack, receiveId,
                    "server did not return an isolated receive session (session_id/nonce mismatch)");
            }
            return new StrictWatchResult(StrictWatchOutcome.Armed, ack, receiveId, null);
        }
        finally
        {
            _watchGate.Release();
        }
    }

    /// <summary>
    /// 立即停止当前观看：先关闭本地接收会话（用户 stop 后立刻停止收帧），再通知服务端
    /// 撤销 lease。不等待在飞的 watch/ACK 门，因此不会出现「stop 后还收到 ACK 超时
    /// 那么久的帧」。
    /// </summary>
    public async Task<bool> StopAsync(CancellationToken ct = default)
    {
        string[] sessions;
        bool hadArmed;
        lock (_nativeGate)
        {
            lock (_lock)
            {
                _generation++;
                hadArmed = _sessionId is not null;
                // 连「已建好但 ACK 未回」的接收会话一起关：身份先于信令，用户 stop 后
                // 不能等 ACK 再停收。全部映射一次摘干净，之后迟到的 ACK 只会判定 Superseded。
                sessions = new string[_tokenBySession.Count];
                _tokenBySession.Keys.CopyTo(sessions, 0);
                _sessionId = null;
                _sharerId = null;
                _nonce = null;
                _serverGeneration = 0;
                _sessionByToken.Clear();
                _tokenBySession.Clear();
                _nonceBySession.Clear();
            }
            foreach (var session in sessions)
            {
                CloseNative(session);
            }
        }
        // 本地关闭已经完成；远端按会话逐个精确撤销（目标相同的新会话不受影响）。
        foreach (var session in sessions)
        {
            await RevokeSessionAsync(session).ConfigureAwait(false);
        }
        // 返回「本次是否真的关掉了一个接收传输」：包括已 arm 的与还在等 ACK 的。
        return hadArmed || sessions.Length > 0;
    }

    /// <summary>
    /// 入站信令归属：只有当前会话 id 放行；顺带把服务端下发的 nonce 记下来（answer
    /// 回带用）。返回 false 表示这是旧/未知会话，调用方必须丢弃。
    /// </summary>
    public bool AcceptInbound(string sessionId, string nonce, out string effectiveNonce)
    {
        lock (_lock)
        {
            if (!_tokenBySession.ContainsKey(sessionId))
            {
                effectiveNonce = string.Empty;
                return false;
            }
            if (!string.IsNullOrEmpty(nonce))
            {
                _nonceBySession[sessionId] = nonce;
            }
            effectiveNonce = _nonceBySession.TryGetValue(sessionId, out var known) ? known : string.Empty;
            return true;
        }
    }

    /// <summary>native SIGNAL_OUT 的 token → (session, nonce)；未知 token 必须丢弃。</summary>
    public bool TryResolveToken(uint token, out string sessionId, out string nonce)
    {
        lock (_lock)
        {
            if (!_sessionByToken.TryGetValue(token, out var knownSession))
            {
                sessionId = string.Empty;
                nonce = string.Empty;
                return false;
            }
            sessionId = knownSession;
            nonce = _nonceBySession.TryGetValue(knownSession, out var knownNonce) ? knownNonce : string.Empty;
            return true;
        }
    }

    /// <summary>
    /// 清掉所有接收状态并关闭 PC（Dispose/连接丢失/重连）：服务端会话已经失效，
    /// 本地不能留着 arm 状态或映射。
    ///
    /// 这里只做本地 exactly-once 关闭，不发撤销请求：断线/重连时服务端已随连接清理
    /// lease；正常停看走 <see cref="StopAsync"/>（它会在本地关闭后按会话精确撤租）。
    /// </summary>
    public void CloseAll()
    {
        string[] sessions;
        lock (_nativeGate)
        {
            lock (_lock)
            {
                _generation++;
                sessions = new string[_tokenBySession.Count];
                _tokenBySession.Keys.CopyTo(sessions, 0);
                _sessionId = null;
                _sharerId = null;
                _nonce = null;
                _serverGeneration = 0;
                _sessionByToken.Clear();
                _tokenBySession.Clear();
                _nonceBySession.Clear();
            }
            foreach (var session in sessions)
            {
                CloseNative(session);
            }
        }
    }

    /// <summary>
    /// 在 native 生命周期门内应用一条按会话的入站信令：先确认会话仍是当前会话，
    /// 再调用 <paramref name="apply"/>。stop/CloseAll 的本地关闭走同一道门，因此
    /// 「归属检查已过、但会话刚被销毁」的迟到信令不会落到已关闭的 native 会话上。
    /// 返回 false 表示会话已关闭，调用方必须丢弃这条信令。
    /// </summary>
    public bool TryApplyToSession(string sessionId, Action apply)
    {
        lock (_nativeGate)
        {
            lock (_lock)
            {
                if (!_tokenBySession.ContainsKey(sessionId))
                {
                    return false;
                }
            }
            apply();
            return true;
        }
    }

    private string? Detach(string sessionId)
    {
        lock (_lock)
        {
            if (!_tokenBySession.ContainsKey(sessionId))
            {
                return null;
            }
            DetachLocked(sessionId);
            return sessionId;
        }
    }

    private void DetachLocked(string sessionId)
    {
        if (_tokenBySession.Remove(sessionId, out var token))
        {
            _sessionByToken.Remove(token);
        }
        _nonceBySession.Remove(sessionId);
        if (string.Equals(_sessionId, sessionId, StringComparison.Ordinal))
        {
            _sessionId = null;
            _nonce = null;
            _sharerId = null;
            _serverGeneration = 0;
        }
    }

    /// <summary>
    /// 服务端通知的会话结束（B2 的 receive_close：共享者停发/断线/会话被撤销）。
    /// 只关闭本地接收传输并清掉映射，**不发撤销请求**（服务端已经清理了 lease），
    /// 也不碰发布、采集或语音。返回是否真的关掉了一个会话。
    /// </summary>
    public bool CloseRemote(string sessionId)
    {
        if (string.IsNullOrEmpty(sessionId))
        {
            return false;
        }
        lock (_nativeGate)
        {
            bool owned;
            lock (_lock)
            {
                owned = _tokenBySession.ContainsKey(sessionId);
                if (owned)
                {
                    DetachLocked(sessionId);
                }
            }
            if (owned)
            {
                CloseNative(sessionId);
            }
            return owned;
        }
    }

    /// <summary>native close 在 native 生命周期门内调用；失败只影响诊断，不改变状态机已经做出的决定。</summary>
    private void CloseOutsideLock(string sessionId)
    {
        lock (_nativeGate)
        {
            CloseNative(sessionId);
        }
    }

    /// <summary>调用方必须持有 <c>_nativeGate</c>（Stop/CloseAll 批量关闭时复用）。</summary>
    private void CloseNative(string sessionId)
    {
        try
        {
            _transport.Close(sessionId);
        }
        catch
        {
            // 关闭失败不能把刚清掉的状态又写回去；交给上层日志/诊断。
        }
    }

    /// <summary>
    /// 按会话精确撤销远端 lease。幂等；失败不得影响本地已经完成的停收，
    /// 连接断开时服务端也会随连接清理。
    /// </summary>
    private async Task RevokeSessionAsync(string sessionId)
    {
        try
        {
            await _transport.RevokeSessionAsync(new StrictReceiveSession(sessionId), CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // 回滚尽力而为；服务端 lease 超时/断线也会随连接清理。
        }
    }
}
