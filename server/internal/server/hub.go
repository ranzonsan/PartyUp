// WebSocket 会话中心。
//
// 一条 WS 连接对应一个成员（PROTOCOL.md §0）。消息分两类：
//   - 请求/响应：客户端发 <type>，服务端回 <type>.ok / <type>.err，响应 id 与请求 id 相同（配对用）；
//   - 推送：text.new / presence / ping，带服务端自己生成的 id。
//
// 会话状态（订阅集合、成员身份）全在内存里；重启即断开，客户端重连重订阅。
// 这正是 v1 的语义（PROTOCOL.md §8 的断线重连细节仍是待定项）。
package server

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/sfu"
	"partyup/internal/store"
)

const (
	// 与服务端心跳节奏（PROTOCOL.md §1.3）：20 秒发 ping，60 秒没任何消息就断开。
	pingInterval = 20 * time.Second
	pongWait     = 60 * time.Second
	// 建立连接后必须在此时间内完成 auth（PROTOCOL.md §1.1：30 秒）。
	authWait = 30 * time.Second
	// 单次写的超时。
	writeWait = 10 * time.Second
	// 每连接允许积压的出站消息数；满了说明客户端读不动，直接断开。
	sendBuffer = 64
)

// Hub 跟踪所有连接与每频道的订阅者。
type Hub struct {
	store   *store.Store
	log     *slog.Logger
	name    string
	version string

	mu    sync.RWMutex
	conns map[*conn]struct{}
	subs  map[string]map[*conn]struct{} // channel_id → 订阅连接
	// shares 是每个频道里正在进行的屏幕共享：channel_id → sharer_member_id → 状态。
	// 只在内存里（共享随连接生命周期走），对应 PROTOCOL.md §5 的 screen.state。
	shares map[string]map[string]protocol.ShareState

	upgrader websocket.Upgrader

	// 语音（PROTOCOL.md §4）。这三张表都受 h.mu 保护，与 subs 共用一把锁，
	// 避免出现「持 A 锁等 B 锁」的嵌套死锁；房间自身另有锁，调用房间方法时不持 h.mu。
	voice map[string]map[string]protocol.VoiceMemberState // channel_id → member_id → 状态
	// voiceSeq 是每频道的加入序号计数器：角色分配只依赖「加入顺序」，不看客户端自报。
	voiceSeq map[string]int
	// voiceMode 是**服务端级**策略（启动参数 --voice-mode），不是每频道可变量：
	// 客户端无权选择传输方式，语音/共享走中继还是直连由运维配置决定。
	voiceMode string
	// screenMode 是**屏幕共享**的独立传输策略（--screen-mode），与 voiceMode 无关：
	// 语音走中继、共享走直连（或反过来）都是合法配置（ADR-0002 三类媒体各自决定路径）。
	screenMode string
	// sfuVideo 表示 SFU 中继是否已具备视频通道（屏幕共享）。
	// v1 先只做音频；视频通道落地前 relay 下的 screen.start 必须干净失败，
	// 绝不能落到「客户端当 offerer 发 offer」的旧路径——那会把语音 PC 一起搞坏。
	sfuVideo bool
	rooms    map[string]*sfu.Room // channel_id → 中继房间（仅 relay 策略下存在）
}

// NewHub 构造会话中心。
func NewHub(st *store.Store, log *slog.Logger, name, version, voiceMode, screenMode string) *Hub {
	return &Hub{
		store:      st,
		log:        log,
		name:       name,
		version:    version,
		conns:      make(map[*conn]struct{}, 8),
		subs:       make(map[string]map[*conn]struct{}, 8),
		shares:     make(map[string]map[string]protocol.ShareState, 4),
		voice:      make(map[string]map[string]protocol.VoiceMemberState, 4),
		voiceSeq:   make(map[string]int, 4),
		voiceMode:  NormalizeVoiceMode(voiceMode),
		screenMode: NormalizeScreenMode(screenMode),
		sfuVideo:   false, // 视频通道待实现；实现后置 true
		rooms:      make(map[string]*sfu.Room, 4),
		upgrader: websocket.Upgrader{
			HandshakeTimeout: 10 * time.Second,
			// 桌面客户端不是浏览器，没有同源概念；放开 Origin 检查。
			CheckOrigin: func(*http.Request) bool { return true },
		},
	}
}

// ServeWS 处理 GET /ws：升级后阻塞在读取循环里，直到连接断开。
// 鉴权不在这里做——PROTOCOL.md §1.1 规定先建连，再在 30 秒内发 auth。
func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	ws, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		h.log.Warn("ws upgrade failed", "err", err, "remote", r.RemoteAddr)
		return
	}
	c := &conn{
		hub:  h,
		ws:   ws,
		log:  h.log.With("remote", r.RemoteAddr),
		send: make(chan []byte, sendBuffer),
		done: make(chan struct{}),
		subs: make(map[string]struct{}, 4),
	}
	h.add(c)
	go c.writePump()
	c.readPump()
	h.log.Info("ws closed", "authed", c.authed, "member", c.member.Name)
}

// Close 断开所有连接（优雅停机时调用）。
func (h *Hub) Close() {
	h.mu.RLock()
	conns := make([]*conn, 0, len(h.conns))
	for c := range h.conns {
		conns = append(conns, c)
	}
	h.mu.RUnlock()
	for _, c := range conns {
		c.close()
	}
}

func (h *Hub) add(c *conn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.conns[c] = struct{}{}
}

func (h *Hub) remove(c *conn) {
	h.mu.Lock()
	delete(h.conns, c)
	channels := make([]string, 0, len(c.subs))
	for ch := range c.subs {
		channels = append(channels, ch)
		delete(h.subs[ch], c)
		if len(h.subs[ch]) == 0 {
			delete(h.subs, ch)
		}
	}
	c.subs = map[string]struct{}{}
	// 断开时清掉自己发起的共享，并记下受影响的频道（锁外广播 screen.state）。
	sharesChanged := make([]string, 0, 2)
	for ch, bySharer := range h.shares {
		if _, ok := bySharer[c.member.ID]; !ok {
			continue
		}
		delete(bySharer, c.member.ID)
		if len(bySharer) == 0 {
			delete(h.shares, ch)
		}
		sharesChanged = append(sharesChanged, ch)
	}
	h.mu.Unlock()

	// 锁外广播 presence / screen.state：避免在持锁时做 I/O。
	for _, ch := range channels {
		h.broadcastPresence(ch)
	}
	for _, ch := range sharesChanged {
		h.broadcastScreenState(ch)
	}
	// 语音同理：断线即视为离开语音（连接是成员存在的唯一凭据）。
	h.leaveVoiceEverywhere(c.member.ID)
}

// subscribe 把连接加入频道订阅，并广播一次 presence。
func (h *Hub) subscribe(c *conn, channelID string) {
	h.mu.Lock()
	if h.subs[channelID] == nil {
		h.subs[channelID] = make(map[*conn]struct{}, 4)
	}
	h.subs[channelID][c] = struct{}{}
	c.subs[channelID] = struct{}{}
	h.mu.Unlock()
	h.broadcastPresence(channelID)
}

// unsubscribe 取消订阅并广播 presence。未订阅时是无操作。
func (h *Hub) unsubscribe(c *conn, channelID string) {
	h.mu.Lock()
	_, existed := c.subs[channelID]
	delete(c.subs, channelID)
	delete(h.subs[channelID], c)
	if len(h.subs[channelID]) == 0 {
		delete(h.subs, channelID)
	}
	h.mu.Unlock()
	if existed {
		h.broadcastPresence(channelID)
		// 退订频道等同离开该频道的语音：否则频道里会挂着「幽灵语音成员」。
		h.leaveVoiceChannel(c.member.ID, channelID)
	}
}

// isSubscribed 报告连接是否订阅了该频道。
func (h *Hub) isSubscribed(c *conn, channelID string) bool {
	h.mu.RLock()
	defer h.mu.RUnlock()
	_, ok := c.subs[channelID]
	return ok
}

// broadcastText 把一条新消息推给该频道的所有订阅者（包括发送者本人：
// 发送者也按服务端分配的 seq 落进自己的消息流，这样两端排序一致）。
func (h *Hub) broadcastText(channelID string, msg protocol.TextMessage) {
	data, err := protocol.MustNew(protocol.TypeTextNew, msg).Marshal()
	if err != nil {
		h.log.Error("marshal text.new", "err", err)
		return
	}
	h.mu.RLock()
	targets := make([]*conn, 0, len(h.subs[channelID]))
	for c := range h.subs[channelID] {
		targets = append(targets, c)
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.enqueueBytes(data)
	}
}

// broadcastPresence 向频道订阅者推送当前在线成员列表。
func (h *Hub) broadcastPresence(channelID string) {
	h.mu.RLock()
	seen := make(map[string]struct{}, len(h.subs[channelID]))
	members := make([]protocol.PresenceMember, 0, len(h.subs[channelID]))
	var targets []*conn
	for c := range h.subs[channelID] {
		targets = append(targets, c)
		if !c.authed {
			continue
		}
		if _, dup := seen[c.member.ID]; dup {
			continue
		}
		seen[c.member.ID] = struct{}{}
		members = append(members, protocol.PresenceMember{
			MemberID: c.member.ID,
			Name:     c.member.Name,
			State:    "online",
		})
	}
	h.mu.RUnlock()

	data, err := protocol.MustNew(protocol.TypePresence, protocol.PresencePayload{
		ChannelID: channelID,
		Members:   members,
	}).Marshal()
	if err != nil {
		h.log.Error("marshal presence", "err", err)
		return
	}
	for _, c := range targets {
		if c.authed {
			c.enqueueBytes(data)
		}
	}
}

// ------------------------------------------------------------------
// 单条连接
// ------------------------------------------------------------------

type conn struct {
	hub *Hub
	ws  *websocket.Conn
	log *slog.Logger

	member protocol.Member
	authed bool

	send      chan []byte
	done      chan struct{}
	closeOnce sync.Once

	// subs 由 hub.mu 保护（与 hub.subs 一起改动）。
	subs map[string]struct{}
}

// enqueueBytes 把已序列化的消息放进发送队列。队列满表示客户端消费不过来，
// 关掉连接让客户端重连，而不是无限缓冲吃内存。
func (c *conn) enqueueBytes(data []byte) {
	select {
	case c.send <- data:
	case <-c.done:
	default:
		c.log.Warn("send buffer full, dropping connection")
		c.close()
	}
}

// enqueue 序列化并排队一个信封。
func (c *conn) enqueue(env protocol.Envelope) {
	data, err := env.Marshal()
	if err != nil {
		c.log.Error("marshal envelope", "type", env.Type, "err", err)
		return
	}
	c.enqueueBytes(data)
}

// writeEnvelope 同步写一个信封（用于 auth 失败后立即断开：必须保证消息发出去）。
func (c *conn) writeEnvelope(env protocol.Envelope) {
	data, err := env.Marshal()
	if err != nil {
		return
	}
	_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
	_ = c.ws.WriteMessage(websocket.TextMessage, data)
}

func (c *conn) close() {
	c.closeOnce.Do(func() {
		close(c.done)
		_ = c.ws.Close()
		c.hub.remove(c)
	})
}

// writePump 独占写端：gorilla 不允许并发写，所有发送都归它。
func (c *conn) writePump() {
	ticker := time.NewTicker(pingInterval)
	defer ticker.Stop()
	for {
		select {
		case data := <-c.send:
			_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.ws.WriteMessage(websocket.TextMessage, data); err != nil {
				c.close()
				return
			}
		case <-ticker.C:
			env := protocol.MustNew(protocol.TypePing, nil)
			data, err := env.Marshal()
			if err != nil {
				continue
			}
			_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.ws.WriteMessage(websocket.TextMessage, data); err != nil {
				c.close()
				return
			}
		case <-c.done:
			return
		}
	}
}

// readPump 独占读端：解码信封并分发。返回即连接结束。
func (c *conn) readPump() {
	defer c.close()
	// 单帧上限 = 媒体信令载荷上限 + 信封与转义余量。SDP 有几 KB，60 KB 太小会把
	// 合法 offer 直接按 1009 断开；具体字段长度仍由各 handler 校验。
	c.ws.SetReadLimit(int64(protocol.MaxSignalPayload) + (8 << 10))
	// 未鉴权时只给 authWait；鉴权后放宽到 pongWait，任何消息都会续期。
	_ = c.ws.SetReadDeadline(time.Now().Add(authWait))
	c.ws.SetPongHandler(func(string) error {
		return c.ws.SetReadDeadline(time.Now().Add(pongWait))
	})

	for {
		_, data, err := c.ws.ReadMessage()
		if err != nil {
			return
		}
		deadline := pongWait
		if !c.authed {
			deadline = authWait
		}
		_ = c.ws.SetReadDeadline(time.Now().Add(deadline))

		env, err := protocol.Decode(data)
		if err != nil {
			c.replyError("", protocol.CodeInvalidArgument, err.Error(), "")
			continue
		}
		c.handle(env)
	}
}

// handle 按类型分发。auth 之前的任何业务消息都拒绝（PROTOCOL.md §1.1）。
func (c *conn) handle(env protocol.Envelope) {
	switch env.Type {
	case protocol.TypePing:
		c.enqueue(protocol.MustNew(protocol.TypePong, nil))
		return
	case protocol.TypePong:
		return // 心跳应答，读超时已被上面的 deadline 续期覆盖
	case protocol.TypeAuth:
		c.handleAuth(env)
		return
	}

	if !c.authed {
		c.replyError(env.Type, protocol.CodeUnauthenticated, "auth required before "+env.Type, env.ID)
		return
	}

	switch env.Type {
	case protocol.TypeChannelList:
		c.handleChannelList(env)
	case protocol.TypeChannelCreate:
		c.handleChannelCreate(env)
	case protocol.TypeChannelSub:
		c.handleChannelSubscribe(env)
	case protocol.TypeChannelUnsub:
		c.handleChannelUnsubscribe(env)
	case protocol.TypeTextSend:
		c.handleTextSend(env)
	case protocol.TypeTextHistory:
		c.handleTextHistory(env)
	case protocol.TypeScreenStart:
		c.handleScreenStart(env)
	case protocol.TypeScreenStop:
		c.handleScreenStop(env)
	case protocol.TypeMediaSignal:
		c.handleMediaSignal(env)
	case protocol.TypeVoiceJoin:
		c.handleVoiceJoin(env)
	case protocol.TypeVoiceLeave:
		c.handleVoiceLeave(env)
	case protocol.TypeVoiceSetMute:
		c.handleVoiceSetMute(env)
	case protocol.TypeVoiceSetDeafen:
		c.handleVoiceSetDeafen(env)
	default:
		c.replyError(env.Type, protocol.CodeInvalidArgument, "unknown message type "+env.Type, env.ID)
	}
}

// replyOK 回 <req.type>.ok，id 复用请求 id（PROTOCOL.md §0：id 用于请求响应配对）。
func (c *conn) replyOK(req protocol.Envelope, payload any) {
	env, err := protocol.New(protocol.TypeOK(req.Type), payload)
	if err != nil {
		c.log.Error("build ok envelope", "type", req.Type, "err", err)
		return
	}
	env.ID = req.ID
	c.enqueue(env)
}

// replyError 回 <reqType>.err。reqID 为空时用新 id（解码失败时拿不到请求 id）。
func (c *conn) replyError(reqType, code, message, reqID string) {
	typ := protocol.TypeError(reqType)
	if reqType == "" {
		typ = "error"
	}
	env, err := protocol.New(typ, protocol.ErrorPayload{Code: code, Message: message})
	if err != nil {
		c.log.Error("build error envelope", "type", typ, "err", err)
		return
	}
	if reqID != "" {
		env.ID = reqID
	}
	c.enqueue(env)
}

// ctx 给存储调用一个带超时的独立 context——WS 读取循环没有请求 context。
func (c *conn) ctx() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), 5*time.Second)
}

// ------------------------------------------------------------------
// 各消息处理器
// ------------------------------------------------------------------

func (c *conn) handleAuth(env protocol.Envelope) {
	if c.authed {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "already authenticated", env.ID)
		return
	}
	var req protocol.AuthRequest
	if err := env.DecodePayload(&req); err != nil {
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: protocol.CodeInvalidArgument, Message: err.Error()}))
		c.close()
		return
	}
	if strings.TrimSpace(req.Token) == "" {
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: protocol.CodeUnauthenticated, Message: "missing token"}))
		c.close()
		return
	}

	ctx, cancel := c.ctx()
	defer cancel()
	member, err := c.hub.store.MemberBySession(ctx, auth.HashToken(strings.TrimSpace(req.Token)))
	if err != nil {
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: protocol.CodeUnauthenticated, Message: "invalid or revoked token"}))
		c.close()
		return
	}

	c.member = protocol.Member{ID: member.ID, Name: member.Name, Admin: member.Admin}
	c.authed = true
	_ = c.ws.SetReadDeadline(time.Now().Add(pongWait))

	c.replyOK(env, protocol.AuthOK{
		Member: c.member,
		Server: protocol.ServerInfo{
			Name:            c.hub.name,
			Version:         c.hub.version,
			ProtocolVersion: protocol.Version,
			Capabilities:    []string{"text", "channel", "presence"},
		},
	})
}

func (c *conn) handleChannelList(env protocol.Envelope) {
	ctx, cancel := c.ctx()
	defer cancel()
	channels, err := c.hub.store.ListChannels(ctx)
	if err != nil {
		c.replyError(env.Type, protocol.CodeInternal, "could not list channels", env.ID)
		return
	}
	out := make([]protocol.Channel, 0, len(channels))
	for _, ch := range channels {
		out = append(out, toProtocolChannel(ch))
	}
	c.replyOK(env, protocol.ChannelListOK{Channels: out})
}

func (c *conn) handleChannelCreate(env protocol.Envelope) {
	var req protocol.ChannelCreateRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	name := strings.TrimSpace(req.Name)
	if !validChannelName(name) {
		c.replyError(env.Type, protocol.CodeInvalidArgument,
			"channel name must be 1-32 visible characters", env.ID)
		return
	}

	ctx, cancel := c.ctx()
	defer cancel()
	ch, err := c.hub.store.CreateChannel(ctx, name)
	if errors.Is(err, store.ErrConflict) {
		c.replyError(env.Type, protocol.CodeConflict, "channel name already taken", env.ID)
		return
	}
	if err != nil {
		c.hub.log.Error("create channel", "err", err)
		c.replyError(env.Type, protocol.CodeInternal, "could not create channel", env.ID)
		return
	}
	c.hub.log.Info("channel created", "channel", ch.Name, "by", c.member.Name)
	c.replyOK(env, protocol.ChannelCreateOK{Channel: toProtocolChannel(ch)})
}

func (c *conn) handleChannelSubscribe(env protocol.Envelope) {
	var req protocol.ChannelSubscribeRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	ctx, cancel := c.ctx()
	defer cancel()
	if _, err := c.hub.store.ChannelByID(ctx, channelID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			c.replyError(env.Type, protocol.CodeNotFound, "channel not found", env.ID)
			return
		}
		c.replyError(env.Type, protocol.CodeInternal, "could not load channel", env.ID)
		return
	}
	c.hub.subscribe(c, channelID)
	c.replyOK(env, protocol.ChannelSubscribeOK{ChannelID: channelID})
	// 订阅后立刻推一次语音快照：客户端由此得知「本频道传输模式」与已在语音的成员，
	// 不必等有人加入语音才知道（PROTOCOL.md §4：模式由服务端策略决定）。
	c.hub.broadcastVoiceState(channelID)
}

func (c *conn) handleChannelUnsubscribe(env protocol.Envelope) {
	var req protocol.ChannelSubscribeRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	c.hub.unsubscribe(c, strings.TrimSpace(req.ChannelID))
	c.replyOK(env, protocol.ChannelSubscribeOK{ChannelID: req.ChannelID})
}

func (c *conn) handleTextSend(env protocol.Envelope) {
	var req protocol.TextSendRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if strings.TrimSpace(req.Body) == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "body must not be empty", env.ID)
		return
	}
	if len(req.Body) > protocol.MaxBodyBytes {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "body too long", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}

	ctx, cancel := c.ctx()
	defer cancel()
	if _, err := c.hub.store.ChannelByID(ctx, channelID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			c.replyError(env.Type, protocol.CodeNotFound, "channel not found", env.ID)
			return
		}
		c.replyError(env.Type, protocol.CodeInternal, "could not load channel", env.ID)
		return
	}

	msg, err := c.hub.store.AppendMessage(ctx, channelID, c.member.ID, req.Body)
	if err != nil {
		c.hub.log.Error("append message", "err", err)
		c.replyError(env.Type, protocol.CodeInternal, "could not store message", env.ID)
		return
	}
	// MemberName 不落库（members 表里有），实时这条路直接取当前连接身份；
	// 历史由 store.History 联表带出，两条路都能给客户端完整作者信息。
	msg.MemberName = c.member.Name

	// 先回执（带 seq，发送者据此把乐观消息转正），再广播 text.new。
	c.replyOK(env, protocol.TextSendOK{
		ID:          msg.ID,
		Seq:         msg.Seq,
		TS:          msg.TS,
		ClientMsgID: req.ClientMsgID,
	})
	c.hub.broadcastText(channelID, toProtocolMessage(msg))
}

func (c *conn) handleTextHistory(env protocol.Envelope) {
	var req protocol.TextHistoryRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	limit := req.Limit
	if limit <= 0 {
		limit = 50
	}
	if limit > protocol.MaxHistoryLimit {
		limit = protocol.MaxHistoryLimit
	}

	ctx, cancel := c.ctx()
	defer cancel()
	if _, err := c.hub.store.ChannelByID(ctx, channelID); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			c.replyError(env.Type, protocol.CodeNotFound, "channel not found", env.ID)
			return
		}
		c.replyError(env.Type, protocol.CodeInternal, "could not load channel", env.ID)
		return
	}
	msgs, err := c.hub.store.History(ctx, channelID, req.BeforeSeq, limit)
	if err != nil {
		c.hub.log.Error("read history", "err", err)
		c.replyError(env.Type, protocol.CodeInternal, "could not read history", env.ID)
		return
	}
	out := make([]protocol.TextMessage, 0, len(msgs))
	for _, m := range msgs {
		out = append(out, toProtocolMessage(m))
	}
	c.replyOK(env, protocol.TextHistoryOK{Messages: out})
}

// validChannelName 校验频道名：1–32 个 rune，不含控制字符。
func validChannelName(name string) bool {
	n := len([]rune(name))
	if n == 0 || n > protocol.MaxChannelName {
		return false
	}
	for _, r := range name {
		if r < 0x20 || r == 0x7F {
			return false
		}
	}
	return true
}

// ------------------------------------------------------------------
// 屏幕共享与媒体信令（PROTOCOL.md §4.2 / §5）
// ------------------------------------------------------------------

// broadcastScreenState 把频道当前共享列表推给该频道的所有订阅者。
func (h *Hub) broadcastScreenState(channelID string) {
	h.mu.RLock()
	bySharer := h.shares[channelID]
	shares := make([]protocol.ShareState, 0, len(bySharer))
	for _, share := range bySharer {
		shares = append(shares, share)
	}
	targets := make([]*conn, 0, len(h.subs[channelID]))
	for peer := range h.subs[channelID] {
		targets = append(targets, peer)
	}
	h.mu.RUnlock()

	h.mu.RLock()
	screenMode := h.screenMode
	h.mu.RUnlock()
	data, err := protocol.MustNew(protocol.TypeScreenState, protocol.ScreenStatePayload{
		ChannelID: channelID,
		Mode:      screenMode,
		Shares:    shares,
	}).Marshal()
	if err != nil {
		h.log.Error("marshal screen.state", "err", err)
		return
	}
	for _, peer := range targets {
		if peer.authed {
			peer.enqueueBytes(data)
		}
	}
}

// validScreenSource 校验共享源描述。真实约束在客户端（来自 pu_screen_sources），
// 服务端只做形状与长度校验，避免把脏数据广播给整个频道。
func validScreenSource(src *protocol.ScreenSource) bool {
	src.ID = strings.TrimSpace(src.ID)
	src.Title = strings.TrimSpace(src.Title)
	if src.ID == "" || src.Title == "" || len(src.ID) > 128 || len(src.Title) > 256 {
		return false
	}
	if src.Width < 0 || src.Height < 0 || src.Width > 32768 || src.Height > 32768 {
		return false
	}
	if src.Kind == "" {
		src.Kind = "screen"
	}
	switch src.Kind {
	case "screen", "window", "synthetic":
		return true
	default:
		return false
	}
}

func (c *conn) handleScreenStart(env protocol.Envelope) {
	var req protocol.ScreenStartRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}
	source := req.Source
	if !validScreenSource(&source) {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "invalid source", env.ID)
		return
	}
	// 传输方式完全由服务端 --screen-mode 决定（与语音 --voice-mode 相互独立）。
	// 客户端传的 mode 仅作告知，不参与决策，因此也不校验它的取值。
	c.hub.mu.RLock()
	mode := c.hub.screenMode
	sfuVideo := c.hub.sfuVideo
	c.hub.mu.RUnlock()
	if mode == "relay" && !sfuVideo {
		// 视频通道未就绪时必须在广播 screen.state 之前拒绝：否则客户端会开始采集
		// 并向已有语音 PC 上挂视频轨，把语音一起拖坏（历史实测 role mismatch → PCM 冻结）。
		c.replyError(env.Type, protocol.CodeNotImplemented,
			"当前服务器为纯 SFU 中继模式，暂不支持屏幕共享（视频通道开发中）", env.ID)
		return
	}

	share := protocol.ShareState{
		SharerID:   c.member.ID,
		MemberName: c.member.Name,
		Source:     source,
		Mode:       mode,
		Viewers:    []string{},
	}
	c.hub.mu.Lock()
	if c.hub.shares[channelID] == nil {
		c.hub.shares[channelID] = make(map[string]protocol.ShareState, 2)
	}
	c.hub.shares[channelID][c.member.ID] = share
	c.hub.mu.Unlock()

	c.hub.log.Info("screen share started", "channel", channelID, "member", c.member.Name,
		"source", source.Title, "width", source.Width, "height", source.Height, "mode", mode)
	c.replyOK(env, protocol.ScreenStartOK{ChannelID: channelID})
	c.hub.broadcastScreenState(channelID)
}

func (c *conn) handleScreenStop(env protocol.Envelope) {
	var req protocol.ScreenStopRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}

	c.hub.mu.Lock()
	removed := false
	if bySharer := c.hub.shares[channelID]; bySharer != nil {
		if _, ok := bySharer[c.member.ID]; ok {
			delete(bySharer, c.member.ID)
			removed = true
		}
		if len(bySharer) == 0 {
			delete(c.hub.shares, channelID)
		}
	}
	c.hub.mu.Unlock()

	if removed {
		c.hub.log.Info("screen share stopped", "channel", channelID, "member", c.member.Name)
	}
	c.replyOK(env, protocol.ScreenStopOK{ChannelID: channelID})
	c.hub.broadcastScreenState(channelID)
}

// handleMediaSignal 转发一条媒体信令（offer / answer / candidate）。
//
// 服务端不解析、不改写 payload：它是媒体核心产出的原始 JSON 信封字符串。
//   - to 为空 → 广播给频道内其它订阅者（offer 用这条）；
//   - to 非空 → 只投递给该成员（answer / candidate 用这条，避免打扰其他观看者）。
func (c *conn) handleMediaSignal(env protocol.Envelope) {
	var req protocol.MediaSignalRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}
	media := strings.TrimSpace(req.Media)
	if media == "" {
		media = "screen"
	}
	if media != "screen" && media != "voice" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "media must be screen or voice", env.ID)
		return
	}
	kind := strings.TrimSpace(req.Kind)
	switch kind {
	case "offer", "answer", "candidate":
	default:
		c.replyError(env.Type, protocol.CodeInvalidArgument, "kind must be offer, answer or candidate", env.ID)
		return
	}
	if req.Payload == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "payload is required", env.ID)
		return
	}
	if len(req.Payload) > protocol.MaxSignalPayload {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "payload too large", env.ID)
		return
	}

	// relay 模式：这条信令的对象是服务端 SFU（offer 由服务端发，客户端只回 answer/candidate）。
	// 用显式 target 区分 relay 与 direct，绝不靠 media/kind 猜——两条路径的信封长得一样。
	// 路由只看 media + 服务端策略，**不再靠 target 猜**：
	//   media=voice + voice_mode=relay  → 交给服务端 SFU（offer/answer/candidate 的对象都是服务端）
	//   media=voice + voice_mode=direct → 成员间转发（按 to 定向 / 不带 to 广播 offer）
	//   media=screen + screen_mode=relay → SFU 视频通道（尚未实现，明确 NOT_IMPLEMENTED）
	//   media=screen + screen_mode=direct → 成员间转发
	// target 仍接受但只做一致性校验：与策略矛盾就报错，避免客户端以为是 relay 却被当 direct 转发。
	//
	// 为什么必须显式 media：语音与屏幕各用独立 PeerConnection 后，candidate 的 SDP 行里
	// 不带媒体类型，靠嗅探只能「两边都试一次让 libwebrtc 拒掉一个」——那是静默失败。
	c.hub.mu.RLock()
	voiceMode, screenMode := c.hub.voiceMode, c.hub.screenMode
	sfuVideo := c.hub.sfuVideo
	c.hub.mu.RUnlock()
	target := strings.TrimSpace(req.Target)

	if media == "voice" {
		if voiceMode == "relay" {
			if target != "" && target != "sfu" {
				c.replyError(env.Type, protocol.CodeInvalidArgument,
					"media=voice 且 voice_mode=relay 时信令对象只能是服务端 SFU", env.ID)
				return
			}
			if err := c.hub.handleVoiceSignal(c.member.ID, channelID, kind, req.Payload); err != nil {
				code := protocol.CodeInvalidArgument
				if errors.Is(err, sfu.ErrNotInRoom) {
					code = protocol.CodeConflict
				}
				c.replyError(env.Type, code, err.Error(), env.ID)
				return
			}
			c.replyOK(env, protocol.MediaSignalOK{ChannelID: channelID})
			return
		}
		if target == "sfu" {
			c.replyError(env.Type, protocol.CodeInvalidArgument,
				"voice_mode=direct 时不应带 target=sfu", env.ID)
			return
		}
	} else { // media == "screen"
		if screenMode == "relay" {
			if !sfuVideo {
				c.replyError(env.Type, protocol.CodeNotImplemented,
					"屏幕共享 SFU 视频通道尚未实现（screen_mode=relay）", env.ID)
				return
			}
			c.replyError(env.Type, protocol.CodeNotImplemented,
				"屏幕共享 SFU 视频通道尚未实现（screen_mode=relay）", env.ID)
			return
		}
		if target == "sfu" {
			c.replyError(env.Type, protocol.CodeInvalidArgument,
				"screen_mode=direct 时不应带 target=sfu", env.ID)
			return
		}
	}

	push := protocol.MediaSignalPush{
		ChannelID: channelID,
		From:      c.member.ID,
		FromName:  c.member.Name,
		Media:     media,
		Kind:      kind,
		Payload:   req.Payload,
	}
	data, err := protocol.MustNew(protocol.TypeMediaSignal, push).Marshal()
	if err != nil {
		c.hub.log.Error("marshal media.signal", "err", err)
		c.replyError(env.Type, protocol.CodeInternal, "could not encode signal", env.ID)
		return
	}

	to := strings.TrimSpace(req.To)
	c.hub.mu.RLock()
	targets := make([]*conn, 0, len(c.hub.subs[channelID]))
	for peer := range c.hub.subs[channelID] {
		if !peer.authed || peer == c {
			continue
		}
		if to != "" && peer.member.ID != to {
			continue
		}
		targets = append(targets, peer)
	}
	c.hub.mu.RUnlock()

	if to != "" && len(targets) == 0 {
		c.replyError(env.Type, protocol.CodeNotFound, "target member is not in this channel", env.ID)
		return
	}
	for _, peer := range targets {
		peer.enqueueBytes(data)
	}
	c.replyOK(env, protocol.MediaSignalOK{ChannelID: channelID})
}
