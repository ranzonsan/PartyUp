using System.Collections.ObjectModel;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.ViewModels;

/// <summary>频道列表项。</summary>
public sealed record ChannelItem(string Id, string Name);

/// <summary>一条待显示的消息（已把 member_id 解析成名字、时间戳格式化成 HH:mm）。</summary>
public sealed record MessageItem(string Author, string Body, string TimeText, bool IsSelf);

/// <summary>
/// 主窗口的视图模型：登录/注册、连接、频道、聊天、presence、媒体引擎状态。
///
/// 线程约定：网络事件从接收循环线程进来，所有改集合的操作都经
/// <see cref="Dispatcher.UIThread"/> 回到 UI 线程；媒体引擎的事件由 <see cref="MediaPump"/>
/// 在专用后台线程阻塞 poll 取出（**不再用 UI 定时器轮询**，那是 Spike 1 证伪的打帧方式）。
/// </summary>
public sealed partial class MainViewModel : ObservableObject
{
    private readonly Dictionary<string, string> _memberNames = new(StringComparer.Ordinal);
    private readonly HashSet<string> _messageIds = new(StringComparer.Ordinal);
    private PartyUpConnection? _connection;
    private MediaEngine? _media;
    private ShareSession? _share;
    private MediaPump? _pump;
    private RemoteScreenPresenter? _remoteScreen;
    private string? _subscribedChannelId;
    private bool _busy;
    private bool _openInFlight;
    private bool _frameAcquireWarned;

    public MainViewModel()
    {
        // --server 同时用于登录框初值与 GUI 自动登录：同一个参数，两处不各写一份默认值。
        ServerUrl = Program.Options.Server;
        if (Program.Options.NoMedia)
        {
            // 仅用于内存归因测量：不 LoadLibrary、不 pu_engine_create，其他功能照常。
            MediaStatusText = "媒体引擎：已禁用（--no-media，仅测量用）";
            AppLog.Info("media: --no-media，跳过 DLL 加载与引擎创建");
        }
        else
        {
            TryInitMedia();
        }
        FooterText = "就绪。填服务器地址后登录，或用邀请码注册；左侧「新建频道」建第一个频道。";
    }

    public ObservableCollection<ChannelItem> Channels { get; } = new();

    public ObservableCollection<MessageItem> Messages { get; } = new();

    public ObservableCollection<MemberRow> Members { get; } = new();

    private IReadOnlyList<PresenceMemberDto> _membersSnapshot = Array.Empty<PresenceMemberDto>();

    /// <summary>可共享的源（本机显示器 + 内置合成源），来自 pu_screen_sources。</summary>
    public ObservableCollection<MediaSource> Sources { get; } = new();

    /// <summary>当前频道里正在进行的共享（screen.state 推送）。</summary>
    public ObservableCollection<ShareDto> Shares { get; } = new();

    public string SelfName { get; private set; } = string.Empty;

    [ObservableProperty]
    private bool _isLoggedIn;

    [ObservableProperty]
    private string _serverUrl = "http://127.0.0.1:8080";

    [ObservableProperty]
    private string _username = string.Empty;

    [ObservableProperty]
    private string _password = string.Empty;

    [ObservableProperty]
    private string _inviteCode = string.Empty;

    [ObservableProperty]
    private string _messageInput = string.Empty;

    [ObservableProperty]
    private string _newChannelName = string.Empty;

    [ObservableProperty]
    private ChannelItem? _selectedChannel;

    [ObservableProperty]
    private string _statusText = "未连接";

    [ObservableProperty]
    private string _mediaStatusText = "媒体引擎：初始化中…";

    // ---- 语音（docs/PROTOCOL.md §4.1；v1 只支持 direct）----

    [ObservableProperty]
    private bool _isInVoice;

    [ObservableProperty]
    private bool _isVoiceMuted;

    [ObservableProperty]
    private bool _isVoiceDeafened;

    [ObservableProperty]
    private string _voiceStatusText = "语音：未加入";

    [ObservableProperty]
    private string _voiceModeText = "模式：—";

    public string VoiceJoinButtonText => IsInVoice ? "离开语音" : "加入语音";

    /// <summary>relay 中继需要后端 SFU 中继，尚未实现——UI 置灰并给说明，不静默假装可用。</summary>
    public bool IsRelayAvailable => false;

    public string RelayUnavailableNote { get; } =
        "中继模式（relay）暂不可用：需要后端 SFU 中继，v1 只支持 direct 直连。";

    partial void OnIsInVoiceChanged(bool value) => OnPropertyChanged(nameof(VoiceJoinButtonText));

    [ObservableProperty]
    private string _footerText = string.Empty;

    /// <summary>共享区选中的源。</summary>
    [ObservableProperty]
    private MediaSource? _selectedSource;

    [ObservableProperty]
    private bool _isSharing;

    [ObservableProperty]
    private string _shareStatusText = "共享：未开始";

    [ObservableProperty]
    private string _watchStatusText = "观看：—";

    [ObservableProperty]
    private string _remoteFramesText = "远端帧：0";

    /// <summary>远端画面区是否可见（收到并渲染过帧后才显示，避免空白黑框）。</summary>
    [ObservableProperty]
    private bool _isRemoteScreenVisible;

    /// <summary>画面渲染帧率文本（由 <see cref="RemoteScreenPresenter.FpsChanged"/> 每秒更新一次）。</summary>
    [ObservableProperty]
    private string _remoteFpsText = "画面：等待远端帧";

    // ------------------------------------------------------------------
    // 媒体引擎
    // ------------------------------------------------------------------

    /// <summary>加载 DLL 并创建引擎；失败只降级状态文本，不影响聊天功能。</summary>
    private void TryInitMedia()
    {
        try
        {
            _media = MediaEngine.Load();
            if (_media.MissingExports.Count > 0)
            {
                MediaStatusText = $"媒体引擎：缺 {_media.MissingExports.Count}/18 个导出";
                AppLog.Fail($"media: missing exports: {string.Join(", ", _media.MissingExports)}");
                return;
            }
            _media.Create("partyup-ui");
            MediaStatusText = "媒体引擎：已就绪（ABI v2，18/18 导出）";
            AppLog.Pass($"media: pu_engine_create ok, dll={Path.GetFileName(_media.LibraryPath)}, exports=18/18");
            LoadSources();
        }
        catch (Exception ex)
        {
            MediaStatusText = "媒体引擎不可用：" + ex.Message;
            AppLog.Warn("media: " + ex.Message);
        }
    }

    /// <summary>
    /// 启动媒体事件泵。**渲染必须由解码回调驱动**（Spike 1 结论）：所以事件不再由 UI
    /// 定时器轮询，而是后台线程阻塞等 <c>pu_poll_event</c>，事件到就处理。UI 定时器打帧
    /// 那条路径已彻底删除，不保留回退分支。
    /// </summary>
    public void StartMediaPump()
    {
        if (_media is not { IsCreated: true })
        {
            AppLog.Warn("media pump: 引擎未就绪，未启动");
            return;
        }
        if (_pump is not null)
        {
            return;
        }
        _pump = new MediaPump(_media, OnMediaEvent);
        _pump.Start();
    }

    /// <summary>停止媒体事件泵（窗口关闭时调用）。</summary>
    public void StopMediaPump()
    {
        _pump?.Dispose();
        _pump = null;
    }

    /// <summary>把远端画面渲染器接到 ViewModel（窗口在 XAML 加载后调用，仅 UI 线程）。</summary>
    public void AttachRemoteScreen(RemoteScreenPresenter presenter)
    {
        _remoteScreen = presenter;
        presenter.FpsChanged += fps =>
        {
            RemoteFpsText = $"画面：{fps:F1} fps（{presenter.Width}x{presenter.Height}）";
        };
        AppLog.Info("remote screen: presenter attached");
    }

    /// <summary>媒体泵线程回调。帧路径在 UI 线程取帧+拷贝；非帧事件 Post 回 UI 线程。</summary>
    private void OnMediaEvent(MediaEvent ev)
    {
        if (ev.Kind == MediaEventKind.RemoteFrame)
        {
            // 用 Invoke（阻塞）而不是 Post：UI 跟不上时让泵线程等，native 队列自然丢旧帧，
            // 不会在托管堆里堆一串待渲染帧。
            Dispatcher.UIThread.Invoke(() =>
            {
                _share?.HandleMediaEvent(ev);
                PresentRemoteFrame();
            });
            return;
        }

        Dispatcher.UIThread.Post(() =>
        {
            _share?.HandleMediaEvent(ev);
            switch (ev.Kind)
            {
                case MediaEventKind.State:
                    MediaStatusText = $"媒体引擎：{ev.PreviousStateValue} → {ev.StateValue}";
                    AppLog.Info($"media: state {ev.PreviousStateValue} -> {ev.StateValue} (reason={ev.Reason})");
                    break;
                case MediaEventKind.Error:
                    MediaStatusText = "媒体引擎错误：" + ev.ErrorMessage;
                    AppLog.Fail("media: " + ev.ErrorMessage);
                    break;
            }
        });
    }

    /// <summary>
    /// 取最近一帧远端画面并渲染。native 的 <c>pu_frame_acquire</c> 还没实现时必须优雅跳过
    /// （返回 NOT_IMPLEMENTED，不能死循环也不能崩），界面保持空白并继续等下一帧。
    /// </summary>
    private void PresentRemoteFrame()
    {
        if (_media is not { IsCreated: true } || _remoteScreen is not { } presenter)
        {
            return;
        }

        var status = _media.FrameAcquire(MediaEngine.RemoteStreamId, out var frame);
        if (status == (int)PuStatus.Timeout)
        {
            // 没有比上次 acquire 更新的帧：属于正常的「还没好」，不是错误。
            return;
        }
        if (status != (int)PuStatus.Ok || frame.Pixels == IntPtr.Zero)
        {
            if (!_frameAcquireWarned)
            {
                _frameAcquireWarned = true;
                AppLog.Warn($"remote screen: 帧未就绪（pu_frame_acquire={status}，{_media.LastErrorText()}）；" +
                            "native 侧尚未实现时属预期，界面继续等待。");
            }
            return;
        }

        try
        {
            if (presenter.Present(frame.Pixels, (int)frame.Width, (int)frame.Height, (int)frame.Stride)
                && !IsRemoteScreenVisible)
            {
                IsRemoteScreenVisible = true;
                _frameAcquireWarned = false;
                AppLog.Pass($"remote screen: 首帧上屏 {frame.Width}x{frame.Height} stride={frame.Stride} " +
                            $"pixels=0x{frame.Pixels.ToInt64():X}");
            }
        }
        finally
        {
            _media.FrameRelease(ref frame);
        }
    }

    // ------------------------------------------------------------------
    // 登录 / 注册
    // ------------------------------------------------------------------

    public async Task LoginAsync()
    {
        await ConnectAsync(async api =>
        {
            var session = await api.LoginAsync(Username.Trim(), Password);
            await AttachSessionAsync(api.BaseUri, session.Token, session.Member);
        });
    }

    public async Task RegisterAsync()
    {
        if (string.IsNullOrWhiteSpace(InviteCode))
        {
            StatusText = "请填写邀请码";
            return;
        }
        await ConnectAsync(async api =>
        {
            var session = await api.RegisterWithInviteAsync(InviteCode.Trim(), Username.Trim(), Password);
            await AttachSessionAsync(api.BaseUri, session.Token, session.Member);
        });
    }

    /// <summary>GUI 自动登录（--token）：先 /api/me 校验，再建 WS。</summary>
    public async Task AutoLoginAsync(string token)
    {
        await ConnectAsync(async api =>
        {
            var member = await api.MeAsync(token);
            await AttachSessionAsync(api.BaseUri, token, member);
        });
    }

    private async Task ConnectAsync(Func<PartyUpApi, Task> flow)
    {
        if (_busy)
        {
            return;
        }
        _busy = true;
        try
        {
            StatusText = "连接中…";
            var baseUri = new Uri(ServerUrl.Trim());
            using var api = new PartyUpApi(baseUri);
            await flow(api);
            IsLoggedIn = true;
            StatusText = $"已连接：{SelfName}";
            AppLog.Pass($"gui: connected as {SelfName} @ {baseUri}");
        }
        catch (Exception ex)
        {
            StatusText = "连接失败：" + ex.Message;
            AppLog.Fail("gui: connect failed: " + ex.Message);
        }
        finally
        {
            _busy = false;
        }
    }

    private async Task AttachSessionAsync(Uri baseUri, string token, MemberDto member)
    {
        if (_connection is not null)
        {
            await _connection.DisposeAsync();
            _connection = null;
        }
        var connection = await PartyUpConnection.ConnectAsync(baseUri, token);
        connection.TextReceived += OnTextReceived;
        connection.PresenceChanged += OnPresenceChanged;
        connection.Closed += OnConnectionClosed;
        _connection = connection;

        SelfName = member.Name;
        _memberNames[member.Id] = member.Name;
        OnPropertyChanged(nameof(SelfName));

        await RefreshChannelsAsync();
    }

    private async Task RefreshChannelsAsync()
    {
        if (_connection is null)
        {
            return;
        }
        var list = await _connection.ListChannelsAsync();
        Channels.Clear();
        foreach (var channel in list.Channels)
        {
            Channels.Add(new ChannelItem(channel.Id, channel.Name));
        }
        FooterText = $"已加载 {Channels.Count} 个频道";
        AppLog.Pass($"gui: channel list loaded ({Channels.Count} channels)");
        if (Channels.Count > 0)
        {
            await OpenChannelAsync(Channels[0]);
        }
    }

    // ------------------------------------------------------------------
    // 频道与消息
    // ------------------------------------------------------------------

    public async Task CreateChannelAsync()
    {
        if (_connection is null)
        {
            StatusText = "请先登录";
            return;
        }
        var name = NewChannelName.Trim();
        if (name.Length == 0)
        {
            StatusText = "频道名不能为空";
            return;
        }
        try
        {
            var created = await _connection.CreateChannelAsync(name);
            Channels.Add(new ChannelItem(created.Id, created.Name));
            NewChannelName = string.Empty;
            FooterText = $"已创建频道 #{created.Name}";
            AppLog.Pass($"gui: channel created #{created.Name} ({created.Id})");
            await OpenChannelAsync(new ChannelItem(created.Id, created.Name));
        }
        catch (Exception ex)
        {
            StatusText = "创建频道失败：" + ex.Message;
            AppLog.Fail("gui: create channel failed: " + ex.Message);
        }
    }

    public async Task OpenChannelAsync(ChannelItem? channel)
    {
        if (channel is null || _connection is null)
        {
            return;
        }
        // SelectedChannel 的赋值会触发 ListBox.SelectionChanged → 再调一次本方法；
        // 已经切到该频道或正在切时直接返回，否则会把历史消息加载两遍。
        if (channel.Id == _subscribedChannelId || _openInFlight)
        {
            return;
        }

        _openInFlight = true;
        try
        {
            // 切频道：旧共享会话先收尾（它的后台线程与事件订阅都绑在旧频道上）。
            if (_share is not null)
            {
                await _share.DisposeAsync();
                _share = null;
                Shares.Clear();
                IsSharing = false;
                ShareStatusText = "共享：未开始";
                // 语音会话随频道一起收尾，UI 状态复位。
                IsInVoice = false;
                IsVoiceMuted = false;
                IsVoiceDeafened = false;
                VoiceStatusText = "语音：未加入";
                VoiceModeText = "模式：—";
            }
            if (_subscribedChannelId is not null && _subscribedChannelId != channel.Id)
            {
                await _connection.UnsubscribeAsync(_subscribedChannelId);
            }
            // 服务端在 subscribe 的响应之后立刻推 presence；先把频道 id 记下来，
            // 否则这条推送会在 _subscribedChannelId 还没赋值时被守卫丢掉（成员列表就空着）。
            _subscribedChannelId = channel.Id;
            await _connection.SubscribeAsync(channel.Id);
            SelectedChannel = channel;

            // 这个频道的共享会话：负责信令转发与远端帧统计。
            if (_media is { IsCreated: true })
            {
                _share = new ShareSession(_media, _connection, channel.Id);
                _share.Changed += OnShareChanged;

                // 自动化验收用：进频道后自动加入语音（--voice-join / --voice-passive）。
                if (Program.Options.VoiceAutoJoin)
                {
                    var passive = Program.Options.VoicePassive;
                    await _share.JoinVoiceAsync(passive);
                    SyncVoiceFromShare();
                    AppLog.Pass($"gui: voice auto-join passive={passive} ok={_share.IsInVoice}");
                }
            }

            Messages.Clear();
            Members.Clear();
            _messageIds.Clear();
            var history = await _connection.HistoryAsync(channel.Id, 0, 50);
            foreach (var message in history)
            {
                AppendMessage(message);
            }
            AppLog.Pass($"gui: opened #{channel.Name} (history={history.Count})");
            StatusText = $"频道 #{channel.Name}（{history.Count} 条历史消息）";
            FooterText = $"已进入 #{channel.Name}；输入消息回车发送";
            // 成员列表按最新快照重建（presence 可能在上面的 await 之前就到了）。
            RebuildMemberRows();
        }
        catch (Exception ex)
        {
            StatusText = "进入频道失败：" + ex.Message;
            AppLog.Fail("gui: open channel failed: " + ex.Message);
        }
        finally
        {
            _openInFlight = false;
        }
    }

    public async Task SendMessageAsync()
    {
        if (_connection is null || SelectedChannel is null)
        {
            StatusText = "请先进入一个频道";
            return;
        }
        var body = MessageInput.Trim();
        if (body.Length == 0)
        {
            return;
        }
        try
        {
            MessageInput = string.Empty;
            await _connection.SendTextAsync(SelectedChannel.Id, body);
        }
        catch (Exception ex)
        {
            StatusText = "发送失败：" + ex.Message;
            AppLog.Fail("gui: send failed: " + ex.Message);
        }
    }

    private void OnTextReceived(TextMessageDto message)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (message.ChannelId != _subscribedChannelId)
            {
                return;
            }
            AppendMessage(message);
        });
    }

    private void OnPresenceChanged(PresenceDto presence)
    {
        // 先同步留一份快照：subscribe 之后 presence 可能早于成员列表初始化完成到达，
        // OpenChannelAsync 末尾会用这份快照重建，避免被随后的 Clear() 清空。
        _membersSnapshot = presence.Members;
        Dispatcher.UIThread.Post(() =>
        {
            if (presence.ChannelId != _subscribedChannelId)
            {
                return;
            }
            foreach (var member in presence.Members)
            {
                _memberNames[member.MemberId] = member.Name;
            }
            _membersSnapshot = presence.Members;
            Members.Clear();
            foreach (var member in presence.Members)
            {
                var inVoice = _share?.VoiceMembers.ContainsKey(member.MemberId) == true;
                var selfMuted = _share is { IsVoiceMuted: true } && member.MemberId == _connection?.Member.Id;
                Members.Add(new MemberRow(member.MemberId, member.Name)
                {
                    IsInVoice = inVoice,
                    IsMuted = selfMuted,
                });
            }
            AppLog.Info($"gui: presence {presence.Members.Count} member(s): {string.Join(", ", presence.Members.Select(m => m.Name))}");
            FooterText = $"频道内 {presence.Members.Count} 人在线";
        });
    }

    private void OnConnectionClosed(string reason)
    {
        Dispatcher.UIThread.Post(() =>
        {
            StatusText = "连接已断开：" + reason;
            IsLoggedIn = false;
            AppLog.Warn("gui: connection closed: " + reason);
        });
    }

    // ------------------------------------------------------------------
    // 屏幕共享（docs/PROTOCOL.md §4.2 / §5）
    // ------------------------------------------------------------------

    /// <summary>枚举可共享的源（本机显示器 + 内置合成源）。</summary>
    private void LoadSources()
    {
        if (_media is not { IsCreated: true })
        {
            return;
        }
        try
        {
            Sources.Clear();
            foreach (var source in _media.EnumerateSources())
            {
                Sources.Add(source);
            }
            SelectedSource ??= Sources.FirstOrDefault();
            AppLog.Pass($"share: sources = {Sources.Count} ({string.Join(", ", Sources.Select(s => s.Title))})");
        }
        catch (Exception ex)
        {
            AppLog.Warn("share: enumerate sources failed: " + ex.Message);
        }
    }

    public async Task StartSharingAsync()
    {
        if (_share is null)
        {
            ShareStatusText = "共享：请先进入频道";
            return;
        }
        if (SelectedSource is null)
        {
            ShareStatusText = "共享：请选择要共享的屏幕";
            return;
        }
        try
        {
            ShareStatusText = $"共享：正在启动「{SelectedSource.Title}」…";
            await _share.StartSharingAsync(SelectedSource);
            IsSharing = true;
            ShareStatusText = $"共享中：{SelectedSource.Title}（{SelectedSource.Width}x{SelectedSource.Height}）";
        }
        catch (Exception ex)
        {
            IsSharing = false;
            ShareStatusText = "共享失败：" + ex.Message;
            AppLog.Fail("gui: start sharing failed: " + ex.Message);
        }
    }

    public async Task StopSharingAsync()
    {
        if (_share is null || !IsSharing)
        {
            return;
        }
        // 记录停止时下拉框还选着什么：第二次点开始若 id 变空，就是这里之后被清掉的。
        AppLog.Info($"share: stop, selected={SelectedSource?.Id ?? "(null)"}");
        try
        {
            await _share.StopSharingAsync();
            IsSharing = false;
            ShareStatusText = "共享：已停止";
        }
        catch (Exception ex)
        {
            ShareStatusText = "停止共享失败：" + ex.Message;
            AppLog.Fail("gui: stop sharing failed: " + ex.Message);
        }
    }

    // ------------------------------------------------------------------
    // 语音
    // ------------------------------------------------------------------

    /// <summary>加入/离开语音。后加入的一方自动走 passive（避免双方并发 offer 的 glare）。</summary>
    public async Task ToggleVoiceAsync()
    {
        if (_share is not { } share)
        {
            VoiceStatusText = "语音：请先进入频道";
            return;
        }
        if (share.IsInVoice)
        {
            await share.LeaveVoiceAsync();
            AppLog.Info("gui: voice left");
        }
        else
        {
            var passive = share.HasOtherVoiceMembers;
            await share.JoinVoiceAsync(passive);
            if (share.IsInVoice)
            {
                AppLog.Pass($"gui: voice joined passive={passive}");
            }
            else
            {
                VoiceStatusText = "语音：加入失败（" + (share.LastError ?? "未知错误") + "）";
            }
        }
        SyncVoiceFromShare();
    }

    public void ToggleVoiceMute()
    {
        if (_share is not { } share || !share.IsInVoice)
        {
            VoiceStatusText = "语音：未加入语音，无法静音";
            return;
        }
        var rc = share.ToggleVoiceMute();
        VoiceStatusText = rc == 0
            ? (share.IsVoiceMuted ? "语音：麦克风已静音" : "语音：麦克风已打开")
            : $"语音：静音失败 rc={rc}";
        SyncVoiceFromShare();
    }

    public void ToggleVoiceDeafen()
    {
        if (_share is not { } share || !share.IsInVoice)
        {
            VoiceStatusText = "语音：未加入语音，无法关闭扬声器";
            return;
        }
        var rc = share.ToggleVoiceDeafen();
        VoiceStatusText = rc == 0
            ? (share.IsVoiceDeafened ? "语音：扬声器已关闭" : "语音：扬声器已打开")
            : $"语音：关闭扬声器失败 rc={rc}";
        SyncVoiceFromShare();
    }

    /// <summary>把 ShareSession 的语音状态同步到可绑定属性（UI 线程调用）。</summary>
    private void SyncVoiceFromShare()
    {
        if (_share is not { } share)
        {
            return;
        }
        IsInVoice = share.IsInVoice;
        IsVoiceMuted = share.IsVoiceMuted;
        IsVoiceDeafened = share.IsVoiceDeafened;
        VoiceModeText = "模式：" + share.VoiceModeText;
        VoiceStatusText = !share.IsInVoice
            ? "语音：未加入"
            : $"语音：已加入（{share.VoiceMembers.Count} 人）" +
              (share.IsVoiceMuted ? "，麦克风静音" : string.Empty) +
              (share.IsVoiceDeafened ? "，扬声器关闭" : string.Empty);
        RebuildMemberRows();
    }

    /// <summary>按最近一次 presence 快照 + 语音状态重建成员行（语音/静音图标绑在这些属性上）。</summary>
    private void RebuildMemberRows()
    {
        var selfId = _connection?.Member.Id;
        var voice = _share?.VoiceMembers;
        Members.Clear();
        foreach (var member in _membersSnapshot)
        {
            Members.Add(new MemberRow(member.MemberId, member.Name)
            {
                IsInVoice = voice?.ContainsKey(member.MemberId) == true,
                IsMuted = _share is { IsVoiceMuted: true } && member.MemberId == selfId,
            });
        }
    }

    private void OnShareChanged()
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (_share is not { } share)
            {
                return;
            }
            Shares.Clear();
            foreach (var item in share.Shares)
            {
                Shares.Add(item);
            }
            WatchStatusText = share.IsWatching ? $"观看：{share.WatchingName} 正在共享" : "观看：—";
            RemoteFramesText = $"远端帧：{share.RemoteFrames}" +
                               (share.RemoteWidth > 0 ? $"（{share.RemoteWidth}x{share.RemoteHeight}）" : string.Empty);
            // 语音状态也挂在同一个 Changed 事件上：成员语音标识、静音图标、按钮文案一起刷新。
            SyncVoiceFromShare();
            if (!share.IsWatching)
            {
                // 对方停止共享：清掉画面并让画面区收起（位图保留待复用，不重建）。
                if (IsRemoteScreenVisible)
                {
                    _remoteScreen?.Clear();
                    IsRemoteScreenVisible = false;
                    RemoteFpsText = "画面：等待远端帧";
                }
            }
            if (share.LastError is { } error)
            {
                ShareStatusText = "共享错误：" + error;
            }
            if (share.IsSharing && !IsSharing)
            {
                IsSharing = true;
            }
        });
    }

    private void AppendMessage(TextMessageDto message)
    {
        // 同一条消息可能来自历史加载 + 实时 text.new 两条路径，按消息 id 去重。
        if (!_messageIds.Add(message.Id))
        {
            return;
        }
        // 作者名优先用服务端随消息下发的 member_name（历史里离线成员也能显示名字）；
        // 再退化到 presence 见过的名字，最后才用 id 前缀兜底。
        var author = !string.IsNullOrWhiteSpace(message.MemberName)
            ? message.MemberName
            : _memberNames.TryGetValue(message.MemberId, out var name)
                ? name
                : message.MemberId.Length >= 6 ? message.MemberId[..6] : message.MemberId;
        var time = DateTimeOffset.FromUnixTimeMilliseconds(message.Ts).ToLocalTime().ToString("HH:mm");
        Messages.Add(new MessageItem(author, message.Body, time, message.MemberId == _connection?.Member.Id));
        AppLog.Pass($"gui: message seq={message.Seq} author={author} body=\"{message.Body}\"");
    }
}
