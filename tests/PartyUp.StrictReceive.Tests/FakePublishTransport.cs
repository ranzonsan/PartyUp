using PartyUp.Client.Services;

namespace PartyUp.StrictReceive.Tests;

/// <summary>
/// 可控的 per-viewer 发送 transport fake：实现生产代码使用的同一条
/// <see cref="IDirectPublishTransport"/> seam，记录每次 open/signal/close。
/// 它让「只关这一条会话、不重启采集」这类断言可在无 native/无桌面的环境复验。
/// </summary>
internal sealed class FakePublishTransport : IDirectPublishTransport
{
    private readonly List<string> _opened = new();
    private readonly List<string> _closed = new();
    private readonly List<string> _signals = new();
    private uint _nextToken = 1;

    /// <summary>设置后 Open 交给它决定 token 或抛异常（建模 native 建会话失败）。</summary>
    public Func<string, uint>? OpenHandler { get; set; }

    public IReadOnlyList<string> Opened { get { return _opened.ToArray(); } }
    public IReadOnlyList<string> Closed { get { return _closed.ToArray(); } }
    public IReadOnlyList<string> Signaled { get { return _signals.ToArray(); } }

    public uint Open(string sessionId)
    {
        _opened.Add(sessionId);
        if (OpenHandler is not null)
        {
            return OpenHandler(sessionId);
        }
        return _nextToken++;
    }

    public int SignalIn(string sessionId, byte[] payload)
    {
        _signals.Add(sessionId);
        return 0;
    }

    public void Close(string sessionId) => _closed.Add(sessionId);
}
