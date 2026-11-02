// WebSocket 会话中心。
//
// 一条 WS 连接对应一个成员（PROTOCOL.md §0）。消息分两类：
//   * 请求/响应：客户端发 <type>，服务端回 <type>.ok / <type>.err，响应 id 与请求 id 相同（配对用）；
//   * 推送：text.new / presence / ping，带服务端自己生成的 id。
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

	upgrader websocket.Upgrader
}

// NewHub 构造会话中心。
func NewHub(st *store.Store, log *slog.Logger, name, version string) *Hub {
	return &Hub{
		store:   st,
		log:     log,
		name:    name,
		version: version,
		conns:   make(map[*conn]struct{}, 8),
		subs:    make(map[string]map[*conn]struct{}, 8),
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
	h.mu.Unlock()

	// 锁外广播 presence：避免在持锁时做 I/O。
	for _, ch := range channels {
		h.broadcastPresence(ch)
	}
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
	c.ws.SetReadLimit(64 << 10)
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
