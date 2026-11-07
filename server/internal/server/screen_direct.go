package server

// B2b direct：严格接收会话的**经纪**（screen 共享走 P2P 时）。
//
// direct 模式下没有服务端 SFU 房间：屏幕 RTP 直接在观看者与共享者之间流动。
// 但「严格接收」仍需要服务端裁决与绑定，否则客户端自报 session/nonce 就等于
// 没有身份。服务端的职责被刻意压到最小：
//
//   - watch 成功时把 (viewer 连接, sharer 发布连接, session, nonce, generation)
//     绑定成一条会话，只把 arm 控制发给**共享者的发布连接**；
//   - offer/answer/candidate 由两端自己的 PeerConnection 产出，服务端不解析、
//     不改写 SDP，只按会话把信令投递给**对端连接**（不是广播频道）；
//   - unwatch/停发/断线时按会话撤销，并通知另一侧关闭该会话的传输——不全局
//     screen.stop，也不要求共享者重启采集。
//
// 为什么信令不能广播：direct 的 legacy 路径（没有 receive_id）靠广播 offer + 成员
// 定向 answer 工作；同频道里同时存在多条会话时，广播会把 A 的 offer 交给无关成员，
// 旧会话的 answer 也可能被新会话接收。会话 id + nonce + 连接归属是唯一稳定的身份。
//
// 锁纪律：本文件所有 *Locked 函数都要求 h.mu 已被持有；其余函数自己取锁，且
// 绝不在持锁时做 I/O（enqueue 只是非阻塞写 channel，但仍统一放到锁外）。

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"partyup/internal/protocol"
)

// errDirectSession 表示 direct 严格会话不存在、已关闭，或信令方向/身份不匹配。
// 调用方必须把它映射成 CONFLICT：这是可预期的过期/错身份，不是内部错误。
var errDirectSession = errors.New("direct screen session is missing, closed or not owned by this sender")

// directScreenSession 是一条 direct 严格接收会话的完整身份。
//
// 两端各有一条自己的传输：观看者的 receive PC（session 由客户端在 watch 前生成）
// 与共享者对该 viewer 的 send PC（同一 session id，由 viewer_arm 触发创建）。
// generation 来自 hub 的单调代次；nonce 由服务端生成，只经服务端下发。
type directScreenSession struct {
	id         string
	channelID  string
	nonce      string
	generation uint64
	viewerID   string
	viewerName string
	viewerConn *conn
	sharerID   string
	sharerConn *conn
	closed     bool
}

// makeDirectSessionNonce 生成不可猜测的会话 nonce（与 SFU 严格接收同一强度）。
func makeDirectSessionNonce() (string, error) {
	var raw [18]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate direct session nonce: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(raw[:]), nil
}

// directPublishConnLocked 返回共享者的**发布连接**：它必须就是真正发起
// screen.start 的那条连接，并且声明了 screen.publish.sessions.v1、仍存活。
//
// 只认 shareOwners，绝不按 member 去其它 socket 里找「有能力的连接」：同一成员
// 可能同时有多个客户端/窗口，各自是独立的 native engine；另一个 socket 没有这路
// 共享的采集源与视频轨，viewer_arm 发给它只会建立一个没有 source 的发送会话，
// 或把 arm 发给错误的窗口。发布归属与能力都是**连接级**事实，两者必须同时成立；
// 不成立就明确 NOT_IMPLEMENTED，由观看者重试/共享者升级，绝不按同成员补位。
//
// 调用方必须持有 h.mu。
func (h *Hub) directPublishConnLocked(channelID, sharerID string) *conn {
	c := h.shareOwners[channelID][sharerID]
	if c == nil || !c.screenPublishSessions {
		return nil
	}
	select {
	case <-c.done:
		return nil
	default:
	}
	return c
}

// commitDirectScreenWatch 在**同一段 h.mu** 内完成「占位 → 正式选台 + 会话登记」，
// 并把被替换掉的旧会话一起摘除（返回给调用方在锁外通知）。
//
// 它取代 relay 的 commitScreenWatch：direct 没有 SFU 接收 PC 需要事后关闭，但多了
// directSessions/receiveByViewer 两张表，必须与 screenWatches 原子更新，否则会出现
// 「hub 记录了一个没有会话的 lease」或「会话活着但没人能撤」。session id 与仍存活
// 的会话碰撞时返回 errDirectSession，旧会话与旧 watch 保持原样。
func (h *Hub) commitDirectScreenWatch(c *conn, channelID string, gen uint64,
	watch screenWatch, session *directScreenSession) (*directScreenSession, bool, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	claim, ok := h.screenWatchClaims[channelID][c.member.ID]
	if !ok || claim.conn != c || claim.gen != gen {
		return nil, false, nil
	}
	if _, sharing := h.shares[channelID][watch.Target]; !sharing {
		delete(h.screenWatchClaims[channelID], c.member.ID)
		if len(h.screenWatchClaims[channelID]) == 0 {
			delete(h.screenWatchClaims, channelID)
		}
		return nil, false, nil
	}
	// session id 是客户端生成的 receive_id，不能假设它全局唯一：另一个观看者
	// （或同成员的另一条连接）可能复用同一个字符串。它是会话表的键，直接写入
	// 会静默覆盖仍活着的 lease，让旧观看者/共享者的信令路由到新会话。碰撞一律
	// 明确 CONFLICT：不覆盖旧会话、不改旧 watch，旧会话的 unwatch/信令仍有效。
	if existing := h.directSessions[channelID][session.id]; existing != nil && !existing.closed {
		return nil, false, fmt.Errorf("%w: session id %q is already active", errDirectSession, session.id)
	}
	// 同一观看者的旧会话（换目标/重看）先摘除，锁外通知旧共享者关闭。
	var replaced *directScreenSession
	if oldID := h.directByViewer[channelID][c.member.ID]; oldID != "" && oldID != session.id {
		if old := h.directSessions[channelID][oldID]; old != nil && !old.closed {
			old.closed = true
			delete(h.directSessions[channelID], oldID)
			replaced = old
		}
	}
	if h.directSessions[channelID] == nil {
		h.directSessions[channelID] = make(map[string]*directScreenSession, 2)
	}
	if h.directByViewer[channelID] == nil {
		h.directByViewer[channelID] = make(map[string]string, 2)
	}
	h.directSessions[channelID][session.id] = session
	h.directByViewer[channelID][c.member.ID] = session.id
	if h.screenWatches[channelID] == nil {
		h.screenWatches[channelID] = make(map[string]screenWatch, 2)
	}
	h.screenWatches[channelID][c.member.ID] = watch
	delete(h.screenWatchClaims[channelID], c.member.ID)
	if len(h.screenWatchClaims[channelID]) == 0 {
		delete(h.screenWatchClaims, channelID)
	}
	return replaced, true, nil
}

// notifyDirectSession sends a session control (viewer_arm / viewer_close to the
// sharer, receive_close to the viewer). It never blocks the caller's lock.
func (h *Hub) notifyDirectSession(session *directScreenSession, to *conn, kind string) {
	if session == nil || to == nil {
		return
	}
	var from, fromName string
	switch kind {
	case protocol.MediaSignalKindViewerArm, protocol.MediaSignalKindViewerClose:
		from = session.viewerID
		fromName = session.viewerName
	case protocol.MediaSignalKindReceiveClose:
		from = session.sharerID
	default:
		return
	}
	data, err := protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
		ChannelID:    session.channelID,
		From:         from,
		FromName:     fromName,
		Media:        "screen",
		Kind:         kind,
		SessionID:    session.id,
		SessionNonce: session.nonce,
		Generation:   session.generation,
	}).Marshal()
	if err != nil {
		h.log.Error("marshal direct session control", "kind", kind, "err", err)
		return
	}
	to.enqueueBytes(data)
	h.log.Info("direct session control", "channel", session.channelID, "kind", kind,
		"viewer", session.viewerID, "sharer", session.sharerID,
		"session", session.id, "generation", session.generation)
}

// routeDirectScreenSignal 校验一条 direct 会话信令并投递给**对端连接**。
//
// 校验项（任一项不满足都不投递）：
//   - 会话存在且未关闭；
//   - 发送者必须是会话双方中的一方，且**连接**就是该端对应的连接（同成员另一
//     socket 不能冒充）；
//   - nonce 常量时间比较；generation 必须与 arm 时精确相等（旧客户端省略时
//     为 0，但旧客户端不可能带 session_id，不会进入严格路径）；
//   - 方向：共享者只能发 offer/candidate，观看者只能发 answer/candidate。
func (h *Hub) routeDirectScreenSignal(c *conn, channelID string, req *protocol.MediaSignalRequest) error {
	h.mu.RLock()
	session := h.directSessions[channelID][req.SessionID]
	if session != nil && session.closed {
		session = nil
	}
	h.mu.RUnlock()
	if session == nil {
		return fmt.Errorf("%w: no active direct session %q", errDirectSession, req.SessionID)
	}
	if subtle.ConstantTimeCompare([]byte(req.SessionNonce), []byte(session.nonce)) != 1 {
		return fmt.Errorf("%w: session nonce mismatch", errDirectSession)
	}
	if req.Generation != session.generation {
		return fmt.Errorf("%w: session generation mismatch", errDirectSession)
	}
	kind := strings.TrimSpace(req.Kind)
	var dst *conn
	switch {
	case c == session.viewerConn && c.member.ID == session.viewerID:
		if kind != protocol.MediaSignalKindAnswer && kind != protocol.MediaSignalKindCandidate {
			return fmt.Errorf("%w: viewer may only send answer/candidate", errDirectSession)
		}
		dst = session.sharerConn
	case c == session.sharerConn && c.member.ID == session.sharerID:
		if kind != protocol.MediaSignalKindOffer && kind != protocol.MediaSignalKindCandidate {
			return fmt.Errorf("%w: sharer may only send offer/candidate", errDirectSession)
		}
		dst = session.viewerConn
	default:
		return fmt.Errorf("%w: sender does not own this direct session", errDirectSession)
	}
	if dst == nil {
		// 对端连接已经没了：会话随后会被断线清理；这里按会话错误拒绝，
		// 让客户端明确知道信令没有送达，而不是静默丢弃。
		return fmt.Errorf("%w: peer connection is gone", errDirectSession)
	}
	select {
	case <-dst.done:
		return fmt.Errorf("%w: peer connection is closed", errDirectSession)
	default:
	}
	data, err := protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
		ChannelID:    channelID,
		From:         c.member.ID,
		FromName:     c.member.Name,
		Media:        "screen",
		Kind:         kind,
		SessionID:    session.id,
		SessionNonce: session.nonce,
		Generation:   session.generation,
		Payload:      req.Payload,
	}).Marshal()
	if err != nil {
		return fmt.Errorf("marshal direct signal: %w", err)
	}
	dst.enqueueBytes(data)
	return nil
}

// takeDirectSessionLocked 摘除一条会话（幂等）。调用方必须持有 h.mu。
func (h *Hub) takeDirectSessionLocked(channelID, sessionID string) *directScreenSession {
	session := h.directSessions[channelID][sessionID]
	if session == nil || session.closed {
		return nil
	}
	session.closed = true
	delete(h.directSessions[channelID], sessionID)
	if h.directByViewer[channelID][session.viewerID] == sessionID {
		delete(h.directByViewer[channelID], session.viewerID)
	}
	if len(h.directSessions[channelID]) == 0 {
		delete(h.directSessions, channelID)
	}
	if len(h.directByViewer[channelID]) == 0 {
		delete(h.directByViewer, channelID)
	}
	return session
}

// closeDirectSession 按会话撤销并通知双方（sharer 收到 viewer_close，viewer 收到
// receive_close）。通知是幂等的：会话只能被摘除一次。
func (h *Hub) closeDirectSession(channelID, sessionID string) *directScreenSession {
	h.mu.Lock()
	session := h.takeDirectSessionLocked(channelID, sessionID)
	h.mu.Unlock()
	if session == nil {
		return nil
	}
	h.notifyDirectSession(session, session.sharerConn, protocol.MediaSignalKindViewerClose)
	h.notifyDirectSession(session, session.viewerConn, protocol.MediaSignalKindReceiveClose)
	return session
}

// closeDirectSessionsForViewer 撤销该观看者（连接）在频道里的会话。
func (h *Hub) closeDirectSessionsForViewer(channelID, viewerID string, owner *conn) []*directScreenSession {
	h.mu.Lock()
	var closing []*directScreenSession
	for id, session := range h.directSessions[channelID] {
		if session.closed || session.viewerID != viewerID || session.viewerConn != owner {
			continue
		}
		if taken := h.takeDirectSessionLocked(channelID, id); taken != nil {
			closing = append(closing, taken)
		}
	}
	h.mu.Unlock()
	for _, session := range closing {
		h.notifyDirectSession(session, session.sharerConn, protocol.MediaSignalKindViewerClose)
	}
	return closing
}

// closeDirectSessionsForTarget 撤销所有指向该共享者的会话（停发/断线）。
// 返回被关闭的会话（调用方可能还要通知观看者）。
func (h *Hub) closeDirectSessionsForTarget(channelID, targetID string) []*directScreenSession {
	h.mu.Lock()
	var closing []*directScreenSession
	for id, session := range h.directSessions[channelID] {
		if session.closed || session.sharerID != targetID {
			continue
		}
		if taken := h.takeDirectSessionLocked(channelID, id); taken != nil {
			closing = append(closing, taken)
		}
	}
	h.mu.Unlock()
	for _, session := range closing {
		h.notifyDirectSession(session, session.viewerConn, protocol.MediaSignalKindReceiveClose)
	}
	return closing
}

// closeDirectSessionsForConn 在连接断开/退订时撤销它参与的会话（作为任一端）。
// 返回数量只用于日志。
func (h *Hub) closeDirectSessionsForConn(c *conn) int {
	h.mu.Lock()
	var closing []*directScreenSession
	for channelID, sessions := range h.directSessions {
		for id, session := range sessions {
			if session.closed || (session.viewerConn != c && session.sharerConn != c) {
				continue
			}
			if taken := h.takeDirectSessionLocked(channelID, id); taken != nil {
				closing = append(closing, taken)
			}
		}
	}
	h.mu.Unlock()
	for _, session := range closing {
		if session.viewerConn == c {
			h.notifyDirectSession(session, session.sharerConn, protocol.MediaSignalKindViewerClose)
			continue
		}
		h.notifyDirectSession(session, session.viewerConn, protocol.MediaSignalKindReceiveClose)
	}
	return len(closing)
}
