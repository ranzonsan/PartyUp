namespace PartyUp.Client.Services;

/// <summary>
/// 共享者侧 per-viewer 发送会话（B2b direct）的最小依赖面：native 发布会话。
///
/// 生产实现是 <see cref="MediaEngine"/>；回归测试可用 fake 实现同一条 seam。
/// 每个方法都可能从不同线程调用（WS 接收线程、媒体事件泵、stop/Dispose）。
/// </summary>
internal interface IDirectPublishTransport
{
    /// <summary>为单个 viewer 建一条独立发送会话并返回 native token（复用采集源/视频轨）。</summary>
    uint Open(string sessionId);

    /// <summary>把对端 answer/candidate 灌进该会话；返回 native 状态码。</summary>
    int SignalIn(string sessionId, byte[] payload);

    /// <summary>只关闭该会话；幂等，不碰采集/发布状态/其它会话。</summary>
    void Close(string sessionId);
}

/// <summary>一条 per-viewer 发送会话的不可混淆身份（与接收会话的 id 空间相同）。</summary>
internal sealed record DirectPublishSession(
    string Id, string ViewerId, string Nonce, ulong Generation, uint Token);

/// <summary>
/// direct 严格观看的**共享者侧**状态机：服务端 arm 一次 → 为这个 viewer 建一条
/// 独立发送传输；服务端 close/停共享/断线 → 只关这一条。
///
/// 它不做以下事情（这些是严格隔离的反例）：
///   * 不调用 pu_screen_pause_send（那只是同一条 PC 上换轨）；
///   * 不重启采集、不改 screen.state、不全局 screen.stop；
///   * 不把旧会话的 token 映射保留到新会话上（native token → 会话的映射在关闭时摘掉）。
///
/// 线程模型：映射在 <c>_lock</c> 下；native 调用在 <c>_nativeGate</c> 下串行，
/// 与「关闭会话 ↔ 注销映射」互斥，避免把信令灌进刚销毁的 native 会话。
/// </summary>
internal sealed class DirectPublishController
{
    private readonly object _lock = new();
    private readonly object _nativeGate = new();
    private readonly IDirectPublishTransport _transport;
    private readonly Dictionary<uint, DirectPublishSession> _byToken = new();
    private readonly Dictionary<string, DirectPublishSession> _bySession = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _sessionByViewer = new(StringComparer.Ordinal);

    public DirectPublishController(IDirectPublishTransport transport)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
    }

    /// <summary>当前活动的发送会话数（诊断/断言用）。</summary>
    public int Count
    {
        get { lock (_lock) { return _bySession.Count; } }
    }

    /// <summary>
    /// 服务端 viewer_arm：为 viewer 建立发送会话。sessionId 已存在时幂等返回既有会话
    /// （重复 arm 不重建传输）。native 建失败返回 null，调用方记录错误——绝不能
    /// 假装成功或退回旧的 pause 路径。
    /// </summary>
    public DirectPublishSession? Arm(string viewerId, string sessionId, string nonce, ulong generation)
    {
        if (string.IsNullOrEmpty(viewerId) || string.IsNullOrEmpty(sessionId))
        {
            return null;
        }
        // 同一 viewer 的旧会话先关（服务端保证旧 lease 已撤销；这里只是资源清理）。
        string? previous;
        lock (_lock)
        {
            if (_bySession.TryGetValue(sessionId, out var existing))
            {
                return existing;
            }
            previous = _sessionByViewer.TryGetValue(viewerId, out var oldId) ? oldId : null;
        }
        if (previous is not null)
        {
            Close(previous);
        }

        uint token;
        lock (_nativeGate)
        {
            try
            {
                token = _transport.Open(sessionId);
            }
            catch
            {
                return null;
            }
            var session = new DirectPublishSession(sessionId, viewerId, nonce, generation, token);
            lock (_lock)
            {
                _byToken[token] = session;
                _bySession[sessionId] = session;
                _sessionByViewer[viewerId] = sessionId;
            }
            return session;
        }
    }

    /// <summary>服务端 viewer_close / 停共享 / 断线：只关该会话。幂等，返回是否真的关了。</summary>
    public bool Close(string sessionId)
    {
        if (string.IsNullOrEmpty(sessionId))
        {
            return false;
        }
        lock (_nativeGate)
        {
            DirectPublishSession? session;
            lock (_lock)
            {
                if (!_bySession.Remove(sessionId, out session))
                {
                    return false;
                }
                _byToken.Remove(session.Token);
                if (_sessionByViewer.TryGetValue(session.ViewerId, out var current) &&
                    string.Equals(current, sessionId, StringComparison.Ordinal))
                {
                    _sessionByViewer.Remove(session.ViewerId);
                }
            }
            try
            {
                _transport.Close(sessionId);
            }
            catch
            {
                // 关闭失败不能把映射写回；native 侧销毁也会回收该 PC。
            }
            return true;
        }
    }

    /// <summary>关闭全部发送会话（Dispose/断线/停共享）。不发远端请求。</summary>
    public void CloseAll()
    {
        List<string> sessions;
        lock (_nativeGate)
        {
            lock (_lock)
            {
                sessions = new List<string>(_bySession.Keys);
                _byToken.Clear();
                _bySession.Clear();
                _sessionByViewer.Clear();
            }
            foreach (var session in sessions)
            {
                try
                {
                    _transport.Close(session);
                }
                catch
                {
                }
            }
        }
    }

    /// <summary>native SIGNAL_OUT token → 发送会话；未知 token 必须丢弃（旧会话信令）。</summary>
    public bool TryResolveToken(uint token, out DirectPublishSession session)
    {
        lock (_lock)
        {
            if (_byToken.TryGetValue(token, out var known))
            {
                session = known;
                return true;
            }
            session = null!;
            return false;
        }
    }

    /// <summary>把该会话的入站 answer/candidate 灌进 native；未知/已关闭会话返回 false。</summary>
    public bool TryApplyInbound(string sessionId, byte[] payload, out int rc)
    {
        rc = 0;
        if (string.IsNullOrEmpty(sessionId) || payload.Length == 0)
        {
            return false;
        }
        lock (_nativeGate)
        {
            lock (_lock)
            {
                if (!_bySession.ContainsKey(sessionId))
                {
                    return false;
                }
            }
            rc = _transport.SignalIn(sessionId, payload);
            return true;
        }
    }
}
