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
	"crypto/subtle"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"sort"
	"strconv"
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

// 全服务器名册里的成员状态（members.state / GET /api/members）。
// 「在线」按 member_id 聚合所有已鉴权 WebSocket；「空闲」= 在线但不在任何语音频道，
// 与键盘活动无关；「voice」= 在线且至少在一个语音频道（可能有多个）。
const (
	memberStateOffline = "offline"
	memberStateIdle    = "idle"
	memberStateVoice   = "voice"
)

// screenWatch 是一个观看者的显式选台状态（受 h.mu 保护）。
// 一次一路：Target 为空表示没有在看；Slot 只有 legacy relay 有意义。
// B2 严格接收路径用 SessionID/Nonce/Owner/Isolated：接收 lease 绑定到**发起
// watch 的那个连接**（同一成员的另一条连接不能停它），过期/错身份的
// answer/ICE 不会进入新会话。
type screenWatch struct {
	Target     string
	Slot       int
	Generation uint64
	SessionID  string
	Nonce      string
	Owner      *conn
	Isolated   bool
	Transport  string
}

// screenWatchClaim 是「正在建立中」的选台占位（受 h.mu 保护）。
//
// 为什么需要它：每条连接各有一把 screenWatchMu，但同一成员的**两条连接**是并发
// 处理的。它们会各自读到「没有旧选台」，然后同时建接收传输、各自写 owner——最后
// SFU 里活着一个会话、hub 记录另一个，谁也不知道该关哪条。占位在 h.mu 内一次
// 判定/写入，把「同一频道同一成员同时只能有一个 watch 在建」变成原子事实；
// 建传输（ICE/Close）仍在锁外，h.mu 内不等任何回调。
type screenWatchClaim struct {
	conn   *conn
	gen    uint64
	target string
}

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
	// shareOwners 记录每路共享由**哪条连接**发起：channel_id → member_id → conn。
	// 同一成员可能有多条连接，断开其中一条时不能清掉另一条发起的共享。
	shareOwners map[string]map[string]*conn
	// screenWatches 是观看者的显式选台：channel_id → viewer_member_id → 目标状态。
	// 只有 opt-in（auth.capabilities 含 screen.watch.v1）的成员会写入这里；旧客户端
	// 不写、继续走 v1 「订阅即自动看全部共享」的兼容路径。
	screenWatches map[string]map[string]screenWatch
	// screenWatchGen 是每频道每个观看者的 watch 代次计数器（单调，只在 h.mu 内递增）。
	screenWatchGen map[string]map[string]uint64
	// screenWatchClaims 是每频道每个观看者「正在建立中」的选台占位（见 screenWatchClaim）。
	// 只由 h.mu 保护；OpenReceive/Close PC 都在锁外完成。
	screenWatchClaims map[string]map[string]screenWatchClaim
	// directSessions / directByViewer 是 direct 严格接收会话（B2b）。direct 没有
	// 服务端 SFU 房间，接收 PC 在观看者本地、发送 PC 在共享者本地；服务端只保存
	// 会话身份（连接/目标/session/nonce/generation）并据此路由信令与撤销。
	// 两张表都与 screenWatches 在同一段 h.mu 内更新（见 commitDirectScreenWatch）。
	directSessions map[string]map[string]*directScreenSession // channel → session id → session
	directByViewer map[string]map[string]string               // channel → viewer → 当前会话 id

	// memberStatePublishMu 串行化全服名册的「取快照 → 编码 → 发送」序列。
	//
	// MemberRoster 在 h.mu 内取在线/语音快照后锁外查数据库；两个并发上下线各自取到
	// 快照 S1/S2 时，完全可能 S2 先发、S1 后发，客户端最终停在旧状态上。发布序列
	// 必须与快照顺序一致，所以整段「快照 + 发送」都在本锁内完成。
	//
	// 它**不是** h.mu 的替代：数据库查询仍不持 h.mu（不阻塞在线路径），本锁只保护
	// 发布顺序。锁内只做一次短查询、JSON 编码与非阻塞 enqueue，不做网络 I/O。
	memberStatePublishMu sync.Mutex
	// memberStatePublishSeq 是发布序号（只在 memberStatePublishMu 内递增），
	// 给确定性回归测试一个可断言的单调序列；不进入协议。
	memberStatePublishSeq uint64
	// memberStatePublishHook 仅用于测试：在快照构造完成后、发送之前调用。
	// 生产路径恒为 nil；测试用它把两次发布确定性地交错，证明旧快照不会后发。
	memberStatePublishHook func(seq uint64)

	upgrader websocket.Upgrader

	// 语音（PROTOCOL.md §4）。这三张表都受 h.mu 保护，与 subs 共用一把锁，
	// 避免出现「持 A 锁等 B 锁」的嵌套死锁；房间自身另有锁，调用房间方法时不持 h.mu。
	voice map[string]map[string]protocol.VoiceMemberState // channel_id → member_id → 状态
	// voiceOwners 记录每个成员的语音状态由哪些连接持有：channel → member → conns。
	// voice.join 是成员级状态，但同一成员可能通过多条连接进入（同频道或不同频道）；
	// 只有最后一个持有者离开/断开时才真正把成员移出语音。不能被单连接断开误清。
	voiceOwners map[string]map[string]map[*conn]struct{}
	// voiceSeq 是每频道的加入序号计数器：角色分配只依赖「加入顺序」，不看客户端自报。
	voiceSeq map[string]int
	// voiceMode 是**服务端级**策略（启动参数 --voice-mode），不是每频道可变量：
	// 客户端无权选择传输方式，语音/共享走中继还是直连由运维配置决定。
	voiceMode string
	// screenMode 是**屏幕共享**的独立传输策略（--screen-mode），与 voiceMode 无关：
	// 语音走中继、共享走直连（或反过来）都是合法配置（ADR-0002 三类媒体各自决定路径）。
	screenMode string
	// modesMu 保护 voiceMode/screenMode 的运行时热更（管理页「危险设置」，仅 owner）。
	// 读路径用 modeSnapshot()、写路径用 SetModes()；两字段只在这把锁下读写，
	// 且**不与 h.mu 嵌套持有**（h.mu 内原先的直接读已改成锁外快照），避免锁序死锁。
	modesMu sync.RWMutex
	// voiceTransport 是每频道**实际**的传输路径（relay | direct）。v1 与 voiceMode 同值，
	// 但它是「事实」而非「策略」：直连失败回落时改它（整频道所有成员一起），不改 voiceMode。
	// 按成员下发（VoiceMemberState.Transport）是为了将来支持逐成员混合路径而不破坏协议。
	voiceTransport map[string]string
	// voiceTransportReason 记录该频道为什么是当前 transport（server_policy / direct_failed_fallback）。
	voiceTransportReason map[string]string
	// voiceFailedAt 是「直连失败上报」的限流表：channel_id + member_id → 上次处理时刻。
	// 网络抖动会连续上报，同一成员在冷却窗口内只处理一次（防刷）。
	voiceFailedAt map[string]time.Time
	// voiceDowngradeCount 是每频道成功降级次数（正常恒为 1；用于日志 trigger_count 与测试断言）。
	voiceDowngradeCount map[string]int
	// stunURLs 是 server.info 下发给客户端的 STUN 地址（`stun:host:port`），
	// 来自启动参数 --stun-listen/--stun-advertise。空表示没开 STUN。
	// 它是**配置**不是**策略**：只影响 ICE 候选收集，不影响 transport 决策。
	stunURLs []string
	// iceConfig 是 SFU 建 PeerConnection 时注入 Pion 的媒体 ICE 配置
	// （固定 UDP 端口区间 + host 候选对外 IP 改写，--ice-port-range/--ice-public-ip）。
	// 零值 = 不限制/不改写。语音房间与视频房间都必须用它，否则容器部署会一条路径失效。
	iceConfig sfu.ICEConfig
	// sessionTTL 是会话有效期（--session-ttl）。WS 鉴权成功时按它做滑动续期
	// （节流粒度见 server.sessionRenewGranularity）。
	sessionTTL time.Duration
	// sfuVideo 表示 SFU 中继是否已具备视频通道（屏幕共享，sfu.VideoRoom）。
	sfuVideo   bool
	rooms      map[string]*sfu.Room      // channel_id → 语音中继房间（仅 voice_mode=relay）
	videoRooms map[string]*sfu.VideoRoom // channel_id → 屏幕共享视频房间（仅 screen_mode=relay）
}

// NewHub 构造会话中心。ice 是媒体 ICE 的端口范围/对外 IP 配置（零值 = 不限制）。
func NewHub(st *store.Store, log *slog.Logger, name, version, voiceMode, screenMode string, sessionTTL time.Duration, stunURLs []string, ice sfu.ICEConfig) *Hub {
	h := &Hub{
		store:                st,
		log:                  log,
		name:                 name,
		version:              version,
		sessionTTL:           sessionTTL,
		stunURLs:             append([]string(nil), stunURLs...),
		iceConfig:            ice,
		conns:                make(map[*conn]struct{}, 8),
		subs:                 make(map[string]map[*conn]struct{}, 8),
		shares:               make(map[string]map[string]protocol.ShareState, 4),
		shareOwners:          make(map[string]map[string]*conn, 4),
		screenWatches:        make(map[string]map[string]screenWatch, 4),
		screenWatchGen:       make(map[string]map[string]uint64, 4),
		screenWatchClaims:    make(map[string]map[string]screenWatchClaim, 4),
		directSessions:       make(map[string]map[string]*directScreenSession, 4),
		directByViewer:       make(map[string]map[string]string, 4),
		voice:                make(map[string]map[string]protocol.VoiceMemberState, 4),
		voiceOwners:          make(map[string]map[string]map[*conn]struct{}, 4),
		voiceSeq:             make(map[string]int, 4),
		voiceMode:            NormalizeVoiceMode(voiceMode),
		screenMode:           NormalizeScreenMode(screenMode),
		sfuVideo:             true, // SFU 视频转发（sfu.VideoRoom）已落地
		voiceTransport:       make(map[string]string, 4),
		voiceTransportReason: make(map[string]string, 4),
		voiceFailedAt:        make(map[string]time.Time, 8),
		voiceDowngradeCount:  make(map[string]int, 4),
		rooms:                make(map[string]*sfu.Room, 4),
		videoRooms:           make(map[string]*sfu.VideoRoom, 4),
		upgrader: websocket.Upgrader{
			HandshakeTimeout: 10 * time.Second,
			// 桌面客户端不是浏览器，没有同源概念；放开 Origin 检查。
			CheckOrigin: func(*http.Request) bool { return true },
		},
	}
	// 容量观测：每 60s 记一次 SFU 的 PC 数/频道数。默认 relay 后每个订阅者都会建
	// 一条视频 PC（即使没人共享），需要真实部署数据再决定要不要做懒加载。
	go h.startCapacityLogger()
	return h
}

// capacityInterval 是容量日志的采样间隔（60s：够看趋势，又不刷日志）。
const capacityInterval = 60 * time.Second

// startCapacityLogger 定期记录 SFU 规模，给「要不要优化空跑视频 PC」提供数据。
func (h *Hub) startCapacityLogger() {
	ticker := time.NewTicker(capacityInterval)
	defer ticker.Stop()
	for range ticker.C {
		h.LogCapacity()
	}
}

// LogCapacity 记一条容量线：视频 PC 总数/频道数 + 语音成员数。
// 日志形状：msg="sfu: video peers=6 channels=3" voice_peers=… voice_rooms=…
func (h *Hub) LogCapacity() {
	h.mu.RLock()
	rooms := make([]*sfu.VideoRoom, 0, len(h.videoRooms))
	for _, r := range h.videoRooms {
		rooms = append(rooms, r)
	}
	voicePeers := 0
	for _, members := range h.voice {
		voicePeers += len(members)
	}
	voiceRooms := len(h.voice)
	h.mu.RUnlock()

	// 先快照再查每个房间：VideoRoom 的回调会反过来取 h.mu，持锁调用有锁序风险。
	videoPeers := 0
	for _, r := range rooms {
		videoPeers += len(r.Members())
	}
	h.log.Info("sfu: video peers="+strconv.Itoa(videoPeers)+" channels="+strconv.Itoa(len(rooms)),
		"voice_peers", voicePeers, "voice_rooms", voiceRooms)
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
		hub:           h,
		ws:            ws,
		log:           h.log.With("remote", r.RemoteAddr),
		send:          make(chan outbound, sendBuffer),
		done:          make(chan struct{}),
		subs:          make(map[string]struct{}, 4),
		voiceChannels: make(map[string]struct{}, 2),
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

// closeMemberConns 关闭某成员的所有已鉴权连接，关闭前逐条发 session.replaced。
//
// 用于单端登录：新会话签发并**提交成功后**踢掉该成员的所有旧连接（无论旧连接
// 用的是哪条旧 token）。返回被通知的连接数，0 表示当时该成员不在线。
//
// 锁序：先在 h.mu 内快照目标，释放锁后再逐条 endSession。原因是 endSession 在
// 发送队列满时会走 c.close() → h.remove() → 再次加 h.mu；若持着 RLock 调用会
// 自死锁（RWMutex 不可重入）。
func (h *Hub) closeMemberConns(memberID, reason string) int {
	h.mu.RLock()
	targets := make([]*conn, 0, 2)
	for c := range h.conns {
		if c.authed && c.member.ID == memberID {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.endSession(reason)
	}
	return len(targets)
}

// closeConnsByTokenHash 关闭使用该 token 的所有已鉴权连接（POST /api/logout 用）。
// 连接上只记 token 的 SHA-256，不记明文。
func (h *Hub) closeConnsByTokenHash(tokenHash, reason string) int {
	h.mu.RLock()
	targets := make([]*conn, 0, 2)
	for c := range h.conns {
		if c.authed && c.tokenHash == tokenHash {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.endSession(reason)
	}
	return len(targets)
}

// touchSession 在 WS 鉴权成功时做滑动续期；节流逻辑在 store.TouchSession。
// 失败只记日志：续期是附带动作，不能因为它让鉴权失败。
func (h *Hub) touchSession(tokenHash string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := h.store.TouchSession(ctx, tokenHash,
		time.Now().UnixMilli(), h.sessionTTL, sessionRenewGranularity); err != nil {
		h.log.Warn("renew ws session", "err", err)
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
	authed := c.authed
	channels := make([]string, 0, len(c.subs))
	for ch := range c.subs {
		channels = append(channels, ch)
		delete(h.subs[ch], c)
		if len(h.subs[ch]) == 0 {
			delete(h.subs, ch)
		}
	}
	c.subs = map[string]struct{}{}
	// 断开时只清这条连接**自己发起**的共享，并记下受影响的频道（锁外广播 screen.state）。
	// 同一成员的另一条连接发起的共享必须保留。
	sharesChanged := make([]string, 0, 2)
	stoppedShares := make([]string, 0, 2) // 被断线清掉的共享者 member_id
	for ch, bySharer := range h.shares {
		if _, ok := bySharer[c.member.ID]; !ok {
			continue
		}
		if h.shareOwners[ch][c.member.ID] != c {
			continue
		}
		delete(bySharer, c.member.ID)
		delete(h.shareOwners[ch], c.member.ID)
		if len(h.shareOwners[ch]) == 0 {
			delete(h.shareOwners, ch)
		}
		if len(bySharer) == 0 {
			delete(h.shares, ch)
		}
		sharesChanged = append(sharesChanged, ch)
		stoppedShares = append(stoppedShares, ch+"\x00"+c.member.ID)
	}
	// 观看者断线：只清**这条连接持有**的选台（同一成员另一条连接的观看不能被误清）。
	// 同时撤掉它还没建完的 watch 占位：在飞的 OpenReceive 会在提交时发现占位已消失，
	// 关闭刚建的接收传输，不会留下没人认领的 lease。
	for ch, claims := range h.screenWatchClaims {
		if claim, ok := claims[c.member.ID]; ok && claim.conn == c {
			delete(claims, c.member.ID)
			if len(claims) == 0 {
				delete(h.screenWatchClaims, ch)
			}
		}
	}
	watchChannels := make([]string, 0, 2)
	watchReceives := make([]struct {
		channel string
		watch   screenWatch
	}, 0, 2)
	for ch, watches := range h.screenWatches {
		watch, ok := watches[c.member.ID]
		if !ok || (watch.Owner != nil && watch.Owner != c) {
			continue
		}
		delete(watches, c.member.ID)
		if len(watches) == 0 {
			delete(h.screenWatches, ch)
		}
		watchChannels = append(watchChannels, ch)
		if watch.Isolated {
			watchReceives = append(watchReceives, struct {
				channel string
				watch   screenWatch
			}{ch, watch})
		}
	}
	// 被清共享的目标也要撤掉所有观看者的选台（停发后不能自动续看）。
	for _, entry := range stoppedShares {
		ch, sharerID, _ := strings.Cut(entry, "\x00")
		for viewerID, watch := range h.screenWatches[ch] {
			if watch.Target == sharerID {
				delete(h.screenWatches[ch], viewerID)
			}
		}
		if len(h.screenWatches[ch]) == 0 {
			delete(h.screenWatches, ch)
		}
		// 还没建完的 watch 同样作废：否则提交时可能把已停发的目标重新 arm。
		for viewerID, claim := range h.screenWatchClaims[ch] {
			if claim.target == sharerID {
				delete(h.screenWatchClaims[ch], viewerID)
			}
		}
		if len(h.screenWatchClaims[ch]) == 0 {
			delete(h.screenWatchClaims, ch)
		}
	}
	// B2b direct：断线时按会话清理。两者都必须通知**对端**：观看者掉线 → 共享者
	// 关闭该 viewer 的发送传输；共享者掉线/停发 → 观看者关闭自己的接收传输。
	// 不连带清同成员另一条连接参与的其它会话（按 *_Conn 精确匹配）。
	var directClosing []*directScreenSession
	for ch, sessions := range h.directSessions {
		for id, session := range sessions {
			if session.closed || (session.viewerConn != c && session.sharerConn != c) {
				continue
			}
			if taken := h.takeDirectSessionLocked(ch, id); taken != nil {
				directClosing = append(directClosing, taken)
			}
		}
	}
	h.mu.Unlock()

	// 锁外广播 presence / screen.state：避免在持锁时做 I/O。
	for _, entry := range watchReceives {
		if room := h.videoRoom(entry.channel); room != nil {
			room.CloseReceive(c.member.ID, entry.watch.SessionID)
		}
	}
	for _, session := range directClosing {
		if session.viewerConn == c {
			h.notifyDirectSession(session, session.sharerConn, protocol.MediaSignalKindViewerClose)
			continue
		}
		h.notifyDirectSession(session, session.viewerConn, protocol.MediaSignalKindReceiveClose)
	}
	for _, ch := range channels {
		h.broadcastPresence(ch)
	}
	for _, entry := range stoppedShares {
		ch, sharerID, _ := strings.Cut(entry, "\x00")
		// relay：摘槽并清空观看者绑定，避免断线后画面冻结在最后一帧。
		h.StopVideoShare(ch, sharerID)
	}
	for _, ch := range sharesChanged {
		h.broadcastScreenState(ch)
	}
	for _, ch := range watchChannels {
		h.broadcastScreenState(ch)
	}
	// 语音/视频同理，但只清这条连接的持有权：同一成员的其他连接仍在频道里时，
	// 其语音与视频 PC 不能被误清（多 WebSocket 语义）。
	h.leaveVoiceConnEverywhere(c)
	h.leaveVideoRoomsForConn(c, channels)
	if authed {
		// 未鉴权连接从未进入名册；跳过数据库查询与无意义全服推送。
		h.broadcastMemberState()
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
		// 退订频道等同离开该频道的语音：只释放**这条连接**的持有权。同一成员的
		// 另一条连接仍订阅/仍在语音里时，不能把对方的语音状态清掉。
		h.leaveVoiceForConn(c, channelID)
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
			MemberID:    c.member.ID,
			Name:        c.member.Name,
			DisplayName: c.member.DisplayName,
			State:       "online",
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

// MemberRoster 返回全服务器名册（含离线成员），按创建顺序排列。
//
// 在线状态按 member_id 聚合所有已鉴权连接：同一成员开多条 WebSocket 时仍是同一项，
// 不会重复、也不会因为其中一条断开就显示离线。语音频道用数组下发，支持成员实际
// 同时在多个频道（例如两条连接分别进入不同频道）的模型；不做「随机取一个」。
func (h *Hub) MemberRoster(ctx context.Context) ([]protocol.MemberRosterEntry, error) {
	h.mu.RLock()
	online := make(map[string]bool, len(h.conns))
	for c := range h.conns {
		if c.authed {
			online[c.member.ID] = true
		}
	}
	voiceChannels := make(map[string][]string, len(h.voice))
	for channelID, members := range h.voice {
		for memberID := range members {
			voiceChannels[memberID] = append(voiceChannels[memberID], channelID)
		}
	}
	h.mu.RUnlock()

	records, err := h.store.ListMembers(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]protocol.MemberRosterEntry, 0, len(records))
	for _, m := range records {
		isOnline := online[m.ID]
		channels := voiceChannels[m.ID]
		if !isOnline {
			channels = nil // 离线成员不可能还在语音里；清掉任何残留。
		}
		sort.Strings(channels)
		if channels == nil {
			channels = []string{}
		}
		state := memberStateOffline
		if isOnline {
			state = memberStateIdle
			if len(channels) > 0 {
				state = memberStateVoice
			}
		}
		out = append(out, protocol.MemberRosterEntry{
			ID:              m.ID,
			Name:            m.Name,
			DisplayName:     m.DisplayName,
			Role:            string(m.Role),
			Admin:           m.Role.IsAdmin(),
			Online:          isOnline,
			State:           state,
			VoiceChannelIDs: channels,
		})
	}
	return out, nil
}

// broadcastMemberState 把全服务器名册推给所有已鉴权连接。
//
// 这是全服成员状态的增量路径；频道内 presence 保持不变（旧客户端只消费它）。
// 先取快照再发送，查询数据库时不持 h.mu。
//
// 整段「取快照 → 编码 → 枚举目标 → 入队」由 memberStatePublishMu 串行化：
// 并发的上线/下线/语音变化各自触发一次发布，发布顺序必须与快照顺序一致，
// 否则后取的快照可能先发，旧快照最后覆盖客户端状态。锁不覆盖 h.mu，数据库
// 查询也不在 h.mu 内；锁内只有一次短查询、编码与非阻塞 enqueue。
func (h *Hub) broadcastMemberState() {
	h.memberStatePublishMu.Lock()
	defer h.memberStatePublishMu.Unlock()
	h.memberStatePublishSeq++
	seq := h.memberStatePublishSeq

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	roster, err := h.MemberRoster(ctx)
	if err != nil {
		h.log.Error("build member roster", "err", err)
		return
	}
	// 测试注入点：此刻快照已经固定，但还没发送。生产路径恒为 nil。
	if h.memberStatePublishHook != nil {
		h.memberStatePublishHook(seq)
	}
	data, err := protocol.MustNew(protocol.TypeMembersState, protocol.MemberListOK{Members: roster}).Marshal()
	if err != nil {
		h.log.Error("marshal members.state", "err", err)
		return
	}
	h.mu.RLock()
	targets := make([]*conn, 0, len(h.conns))
	for c := range h.conns {
		if c.authed {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.enqueueBytes(data)
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
	// tokenHash 是鉴权所用 token 的 SHA-256（由 hub.mu 保护，与 member/authed
	// 同一把锁）。退出登录/单端登录踢人时按它反查连接；不存 token 原文。
	tokenHash string

	send      chan outbound
	done      chan struct{}
	closeOnce sync.Once

	// subs 由 hub.mu 保护（与 hub.subs 一起改动）。
	subs map[string]struct{}
	// voiceChannels 是这条连接加入过语音的频道（由 hub.mu 保护）。断线/退订时只
	// 释放这些频道的持有权；同一成员的其他连接加入的频道不受影响。
	voiceChannels map[string]struct{}
	// screenWatchOptIn 表示这条连接在 auth 里声明了 screen.watch.v1：它订阅频道时
	// 服务端**不会**自动把已有共享绑定给它，只有收到 screen.watch 才绑定。
	// 旧客户端为 false，保持 v1 自动观看行为。由 hub.mu 保护（auth 时写入）。
	screenWatchOptIn bool
	// screenWatchIsolated 表示这条连接在 auth 里声明了 screen.watch.isolated.v1，
	// 且服务端此刻确实支持严格接收传输（relay 的 SFU 接收 PC，或 direct 的会话
	// 经纪 + 对端发布能力）。B2：只有它为 true 才允许 screen.watch 带 receive_id；
	// 缺任一层时明确报不支持，不降级。
	screenWatchIsolated bool
	// screenPublishSessions 表示这条连接（作为共享者）声明了
	// screen.publish.sessions.v1：能为单个 viewer 建立独立发送传输/会话。
	// direct 严格 watch 只在共享者的**发布连接**具备它时才成立。
	screenPublishSessions bool
	// screenWatchMu 串行化同一连接上的 watch/unwatch：客户端按 ack 顺序发请求，
	// 但两个请求的处理协程可能交错；不串行会出现「新 watch 刚绑好又被旧 unwatch 解掉」。
	screenWatchMu sync.Mutex
}

// outbound 是发送队列里的一帧。
//
// 绝大多数是普通业务帧（data 非空、closeCode=0）。会话被替换/注销时队列里会
// 追加一个「最后一帧」：先写 data（session.replaced），再用 closeCode 发 Close
// 控制帧，然后结束 writePump。
//
// 为什么把「关闭」也放进同一个队列：gorilla 不允许并发写，所有写必须由
// writePump 单 goroutine 串行完成。若在别的 goroutine 直接写应用层消息再 Close，
// 客户端可能先收到 Close 帧、后收到（或丢掉）应用层消息，就看不到「账号已在
// 另一处登录」的提示。
type outbound struct {
	data      []byte
	closeCode int    // 0 表示不关闭；否则写完 data 后以该码关闭连接
	closeText string // Close 帧里的 reason（会出现在线上，不放敏感信息）
}

// enqueueBytes 把已序列化的消息放进发送队列。队列满表示客户端消费不过来，
// 关掉连接让客户端重连，而不是无限缓冲吃内存。
func (c *conn) enqueueBytes(data []byte) {
	c.enqueueOutbound(outbound{data: data})
}

// enqueueOutbound 投递一帧（普通帧或「最后一帧」）。非阻塞：队列满时直接断开。
func (c *conn) enqueueOutbound(out outbound) {
	select {
	case c.send <- out:
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

// endSession 通知客户端「这条连接所属的会话结束了」并优雅关闭。
//
// 先入队 session.replaced（reason 区分被顶号/被踢/被封禁/主动退出），再以
// 与 reason 对应的关闭码（4001/4002/4003）关闭连接。发送队列恰好满（客户端
// 已读不动）时 enqueueOutbound 会直接断开——宁可少一条提示，也不能让连接僵住。
//
// 关连接前打一行 info 日志（含 reason 与 close_code）：真机验收与排障需要能从
// 服务端侧确认「哪条连接因为什么被结束」，而不是只看客户端说什么。
func (c *conn) endSession(reason string) {
	closeCode := protocol.CloseCodeForSessionReason(reason)
	if c.authed {
		c.log.Info("session ended by server",
			"member_id", c.member.ID, "member_name", c.member.Name,
			"reason", reason, "close_code", closeCode)
	}
	env := protocol.MustNew(protocol.TypeSessionReplaced, protocol.SessionReplacedPayload{Reason: reason})
	data, err := env.Marshal()
	if err != nil {
		c.log.Error("marshal session.replaced", "err", err)
		c.close()
		return
	}
	c.enqueueOutbound(outbound{data: data, closeCode: closeCode, closeText: reason})
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
		case out := <-c.send:
			_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
			if len(out.data) > 0 {
				if err := c.ws.WriteMessage(websocket.TextMessage, out.data); err != nil {
					c.close()
					return
				}
			}
			if out.closeCode != 0 {
				// 应用层消息（若有）已经先写完，再发 Close 控制帧：顺序在单
				// goroutine 内天然保证，客户端一定能先看到 session.replaced。
				msg := websocket.FormatCloseMessage(out.closeCode, out.closeText)
				_ = c.ws.WriteControl(websocket.CloseMessage, msg, time.Now().Add(writeWait))
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
	case protocol.TypeMemberList:
		c.handleMemberList(env)
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
	case protocol.TypeScreenWatch:
		c.handleScreenWatch(env)
	case protocol.TypeScreenUnwatch:
		c.handleScreenUnwatch(env)
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
	case protocol.TypeServerInfo:
		c.handleServerInfo(env)
	case protocol.TypeVoiceTransportFailed:
		c.handleVoiceTransportFailed(env)
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
	tokenHash := auth.HashToken(strings.TrimSpace(req.Token))
	member, err := c.hub.store.MemberBySession(ctx, tokenHash)
	// 失败时再查一次会话状态，把「被新登录替换」与「无效/过期」区分成不同错误码。
	// 只在失败路径多一次查询，成功路径零额外开销。
	var state store.SessionState
	if err != nil {
		state, _ = c.hub.store.SessionState(ctx, tokenHash)
	}
	cancel()
	if err != nil {
		code := protocol.CodeUnauthenticated
		message := "invalid or expired token"
		if state.Exists && state.Revoked {
			// 单端登录：同一账号在别处登录成功，这条会话已被撤销。
			code = protocol.CodeSessionReplaced
			message = "session replaced by a newer login"
		}
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: code, Message: message}))
		c.close()
		return
	}
	// 鉴权成功 → 滑动续期（PROTOCOL.md §1.1）。节流在 store 层：活跃连接每
	// 5 分钟最多一次单行 UPDATE。失败不影响本次鉴权。
	c.hub.touchSession(tokenHash)

	// 封禁检查（规格 §5）：被封禁的账号即使持有未过期的 token 也不能建立连接。
	// 放在续期之前——被封禁的会话没必要再续命。
	banCtx, banCancel := context.WithTimeout(context.Background(), 5*time.Second)
	banned, banErr := c.hub.store.IsBanned(banCtx, member.ID)
	banCancel()
	if banErr != nil {
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: protocol.CodeInternal, Message: "could not verify account status"}))
		c.close()
		return
	}
	if banned {
		c.writeEnvelope(protocol.MustNew(protocol.TypeError(env.Type),
			protocol.ErrorPayload{Code: protocol.CodeMemberBanned, Message: "this account is banned"}))
		c.close()
		return
	}

	// member/authed 会被其他 goroutine（名册/广播/断线清理）读取；统一用 h.mu
	// 与连接集合一起保护，避免 -race 下的数据竞争。
	optIn := hasCapability(req.Capabilities, protocol.CapabilityScreenWatch)
	wantsIsolated := hasCapability(req.Capabilities, protocol.CapabilityScreenWatchIsolated)
	wantsPublishSessions := hasCapability(req.Capabilities, protocol.CapabilityScreenPublishSessions)
	// 模式快照在 h.mu 之外取：modeSnapshot 用独立的 modesMu，避免与 h.mu 形成
	// 锁序问题（管理页改模式时只写 modesMu）。
	_, screenModeNow := c.hub.modeSnapshot()
	c.hub.mu.Lock()
	// 严格接收传输在两种模式下都成立：
	//   - relay：接收 PC 在服务端 SFU（需要视频通道就绪）；
	//   - direct：接收 PC 在观看者本地、发送 PC 在共享者本地，服务端只做会话经纪。
	// direct 是否真的能 arm 还要看共享者的发布连接是否声明了 publish-sessions；
	// 那一条在 watch 时判定，以便明确报 NOT_IMPLEMENTED（而不是提前不广告能力）。
	strictSupported := screenModeNow == "direct" || (screenModeNow == "relay" && c.hub.sfuVideo)
	c.member = protocol.Member{
		ID: member.ID, Name: member.Name, DisplayName: member.DisplayName,
		Role: string(member.Role), Admin: member.Admin,
	}
	c.tokenHash = tokenHash
	c.authed = true
	c.screenWatchOptIn = optIn
	c.screenWatchIsolated = wantsIsolated && strictSupported
	c.screenPublishSessions = wantsPublishSessions
	c.hub.mu.Unlock()
	_ = c.ws.SetReadDeadline(time.Now().Add(pongWait))
	memberOK := c.member

	caps := []string{
		"text", "channel", "presence", "member_roster",
		protocol.CapabilityScreenWatch,
		protocol.CapabilityScreenWatchSlots,
	}
	if strictSupported {
		caps = append(caps, protocol.CapabilityScreenWatchIsolated)
	}
	c.replyOK(env, protocol.AuthOK{
		Member: memberOK,
		Server: protocol.ServerInfo{
			Name:            c.hub.name,
			Version:         c.hub.version,
			ProtocolVersion: protocol.Version,
			Capabilities:    caps,
		},
	})
	// 上线是服务器级名册变化：推给所有已鉴权连接，不只本频道订阅者。
	c.hub.broadcastMemberState()
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

// handleMemberList 返回全服务器名册（含离线），是 GET /api/members 的 WS 等价物。
// 之后的增量由 members.state 推送；本请求本身只读。
func (c *conn) handleMemberList(env protocol.Envelope) {
	ctx, cancel := c.ctx()
	defer cancel()
	roster, err := c.hub.MemberRoster(ctx)
	if err != nil {
		c.hub.log.Error("list member roster", "err", err)
		c.replyError(env.Type, protocol.CodeInternal, "could not list members", env.ID)
		return
	}
	c.replyOK(env, protocol.MemberListOK{Members: roster})
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
	// 屏幕共享中继：订阅该频道即建视频 PC，观看者要在共享开始之前就绪。
	// direct 模式下绝不建（用户以为屏幕是 P2P，实际却连服务器 = 资源浪费 + 隐私问题）。
	// 新客户端已声明 screen.watch.v1：建 PC 但不自动绑定任何共享（manual=true），
	// 未点 watch 前不接收视频；旧客户端保持 v1 自动绑定（manual=false）。
	c.hub.mu.RLock()
	watchOptIn := c.screenWatchOptIn
	c.hub.mu.RUnlock()
	c.hub.joinVideoRoom(channelID, c.member.ID, watchOptIn)
	c.replyOK(env, protocol.ChannelSubscribeOK{ChannelID: channelID})
	// 订阅后立刻推一次语音快照：客户端由此得知「本频道传输模式」与已在语音的成员，
	// 不必等有人加入语音才知道（PROTOCOL.md §4：模式由服务端策略决定）。
	c.hub.broadcastVoiceState(channelID)
	// 同样补一次 screen.state 初始快照：晚加入者要立刻看到已有共享，而不是等共享者
	// 下一次操作（PROTOCOL.md §5）。只发给本连接，不惊动频道内其他人。
	c.hub.sendScreenStateTo(c, channelID)
}

func (c *conn) handleChannelUnsubscribe(env protocol.Envelope) {
	var req protocol.ChannelSubscribeRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	// 离开频道 = 停止本连接在该频道的观看，释放严格接收传输（不碰发布/语音）。
	c.clearScreenWatchForConn(channelID)
	c.hub.unsubscribe(c, channelID)
	c.hub.leaveVideoRoomForConn(c, channelID)
	c.replyOK(env, protocol.ChannelSubscribeOK{ChannelID: req.ChannelID})
}

// clearScreenWatchForConn 只清理这条连接持有（Owner）的选台；同成员另一条
// 连接的观看保持不变。严格接收会话在锁外关闭。
func (c *conn) clearScreenWatchForConn(channelID string) {
	c.hub.mu.Lock()
	// 本条连接还没建完的 watch 占位一并撤销：在飞的 OpenReceive 提交时会失败，
	// 关闭刚建的接收传输，不会给已退订的连接留下 lease。
	if claim, ok := c.hub.screenWatchClaims[channelID][c.member.ID]; ok && claim.conn == c {
		delete(c.hub.screenWatchClaims[channelID], c.member.ID)
		if len(c.hub.screenWatchClaims[channelID]) == 0 {
			delete(c.hub.screenWatchClaims, channelID)
		}
	}
	watch, ok := c.hub.screenWatches[channelID][c.member.ID]
	if !ok || (watch.Owner != nil && watch.Owner != c) {
		c.hub.mu.Unlock()
		return
	}
	delete(c.hub.screenWatches[channelID], c.member.ID)
	if len(c.hub.screenWatches[channelID]) == 0 {
		delete(c.hub.screenWatches, channelID)
	}
	c.hub.mu.Unlock()
	if watch.Isolated {
		if watch.Transport == "direct" {
			// direct：撤销会话并让共享者关闭该 viewer 的发送传输（退订 = 停看）。
			c.hub.closeDirectSession(channelID, watch.SessionID)
			return
		}
		if room := c.hub.videoRoom(channelID); room != nil {
			room.CloseReceive(c.member.ID, watch.SessionID)
		}
	}
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
	msg.MemberDisplayName = c.member.DisplayName

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

// hasCapability 报告客户端声明里是否包含某个能力字符串（大小写敏感，去空白）。
func hasCapability(caps []string, want string) bool {
	for _, c := range caps {
		if strings.EqualFold(strings.TrimSpace(c), want) {
			return true
		}
	}
	return false
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

// screenStateSnapshot 构造频道当前共享快照（含服务端屏幕策略）。按 sharer_id 排序，
// 保证多次推送与测试断言稳定。
func (h *Hub) screenStateSnapshot(channelID string) protocol.ScreenStatePayload {
	h.mu.RLock()
	bySharer := h.shares[channelID]
	shares := make([]protocol.ShareState, 0, len(bySharer))
	for _, share := range bySharer {
		shares = append(shares, share)
	}
	// Viewers 从显式选台表推导（不信任存储里的旧值）：一路共享的观看者 =
	// 该频道里 target 指向它的成员。旧客户端不写 screenWatches，因此列表为空，
	// 与 v1 字段语义一致（旧字段过去也恒为空）。
	watches := h.screenWatches[channelID]
	for i := range shares {
		viewers := make([]string, 0, 2)
		for viewerID, watch := range watches {
			if watch.Target == shares[i].SharerID {
				viewers = append(viewers, viewerID)
			}
		}
		sort.Strings(viewers)
		shares[i].Viewers = viewers
	}
	_, screenMode := h.modeSnapshot()
	h.mu.RUnlock()

	sort.Slice(shares, func(i, j int) bool { return shares[i].SharerID < shares[j].SharerID })
	return protocol.ScreenStatePayload{
		ChannelID: channelID,
		Mode:      screenMode,
		Shares:    shares,
	}
}

// broadcastScreenState 把频道当前共享列表推给该频道的所有订阅者。
func (h *Hub) broadcastScreenState(channelID string) {
	payload := h.screenStateSnapshot(channelID)
	data, err := protocol.MustNew(protocol.TypeScreenState, payload).Marshal()
	if err != nil {
		h.log.Error("marshal screen.state", "err", err)
		return
	}
	h.mu.RLock()
	targets := make([]*conn, 0, len(h.subs[channelID]))
	for peer := range h.subs[channelID] {
		if peer.authed {
			targets = append(targets, peer)
		}
	}
	h.mu.RUnlock()
	for _, peer := range targets {
		peer.enqueueBytes(data)
	}
}

// sendScreenStateTo 只给一条连接发当前共享快照：用于 channel.subscribe 后的
// 「晚加入立即看到已有共享」。没有共享时不发空快照——客户端从 voice.state 的
// screen_mode 已能知道策略，空列表是默认值；这样也不会给旧客户端增加无意义事件。
func (h *Hub) sendScreenStateTo(c *conn, channelID string) {
	payload := h.screenStateSnapshot(channelID)
	if len(payload.Shares) == 0 {
		return
	}
	data, err := protocol.MustNew(protocol.TypeScreenState, payload).Marshal()
	if err != nil {
		h.log.Error("marshal screen.state snapshot", "err", err)
		return
	}
	c.enqueueBytes(data)
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
	_, mode := c.hub.modeSnapshot()
	c.hub.mu.RLock()
	sfuVideo := c.hub.sfuVideo
	c.hub.mu.RUnlock()
	if mode == "relay" && !sfuVideo {
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
	if c.hub.shareOwners[channelID] == nil {
		c.hub.shareOwners[channelID] = make(map[string]*conn, 2)
	}
	// 记录发起共享的连接：同一成员断开另一条连接时不能误清这路共享。
	c.hub.shareOwners[channelID][c.member.ID] = c
	c.hub.mu.Unlock()

	c.hub.log.Info("screen share started", "channel", channelID, "member", c.member.Name,
		"source", source.Title, "width", source.Width, "height", source.Height, "mode", mode)
	// relay：共享者的视频 PC 里那条 recvonly m-line 在它「当观看者」时就已经答成「不发」，
	// 而客户端只有本端 answer 之前才能挂上视频轨（ABI v2.11 无重协商）。
	// 所以这里重建它的视频 PC 并重发 offer——客户端此时已用被动模式建好带轨的新会话。
	if mode == "relay" && sfuVideo {
		c.hub.RefreshVideoPeer(channelID, c.member.ID)
	}
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
	clearedWatches := 0
	if bySharer := c.hub.shares[channelID]; bySharer != nil {
		if _, ok := bySharer[c.member.ID]; ok {
			delete(bySharer, c.member.ID)
			removed = true
		}
		if len(bySharer) == 0 {
			delete(c.hub.shares, channelID)
		}
	}
	if owners := c.hub.shareOwners[channelID]; owners != nil {
		delete(owners, c.member.ID)
		if len(owners) == 0 {
			delete(c.hub.shareOwners, channelID)
		}
	}
	// 停发即撤掉所有指向这路共享的显式选台：观看者要重新点 watch 才会被绑定。
	// 不清理的话，同一成员重新开共享时会被旧 watch 自动续看（等于「未点也接收」）。
	for viewerID, watch := range c.hub.screenWatches[channelID] {
		if watch.Target == c.member.ID {
			delete(c.hub.screenWatches[channelID], viewerID)
			clearedWatches++
		}
	}
	if len(c.hub.screenWatches[channelID]) == 0 {
		delete(c.hub.screenWatches, channelID)
	}
	// 在飞的 watch 占位同样作废：提交时会重新验证目标仍在共享，绝不 arm 已停发的目标。
	for viewerID, claim := range c.hub.screenWatchClaims[channelID] {
		if claim.target == c.member.ID {
			delete(c.hub.screenWatchClaims[channelID], viewerID)
		}
	}
	if len(c.hub.screenWatchClaims[channelID]) == 0 {
		delete(c.hub.screenWatchClaims, channelID)
	}
	c.hub.mu.Unlock()

	if removed {
		c.hub.log.Info("screen share stopped", "channel", channelID, "member", c.member.Name)
		// relay：立刻摘掉该共享占用的槽位并清空所有观看者的绑定，否则观看者会一直
		// 收到最后一帧（画面卡死）。direct 没有服务端房间，这一步是空操作。
		c.hub.StopVideoShare(channelID, c.member.ID)
		// B2b direct：撤销所有指向该共享者的严格会话，并通知每个观看者关闭自己的
		// 接收传输。共享者本端的发送 PC 随 native screen.stop 一起关闭。
		c.hub.closeDirectSessionsForTarget(channelID, c.member.ID)
	}
	if clearedWatches > 0 {
		c.hub.log.Info("screen watches cleared by stop", "channel", channelID,
			"sharer", c.member.Name, "count", clearedWatches)
	}
	c.replyOK(env, protocol.ScreenStopOK{ChannelID: channelID})
	c.hub.broadcastScreenState(channelID)
}

// handleScreenWatch 让观看者显式选择一路共享（一次一路）。
//
// 服务端职责：
//   - 校验频道订阅与目标存在（目标必须正在共享、不能是自己）；
//   - 传输路径由服务端 screenMode 裁决（relay/direct），客户端不能选；
//   - relay：在观看者的视频 PC 上把目标绑到一个 sendonly 槽（与上一目标的槽
//     不同），并解绑旧目标；direct：服务端只做选台/1v1 容量校验，P2P 由客户端
//     自己协商（不建 SFU 槽，slot=-1）；
//   - 递增 generation 并回给客户端，原生据此做「目标 + 代次」的帧归属。
func (c *conn) handleScreenWatch(env protocol.Envelope) {
	c.screenWatchMu.Lock()
	defer c.screenWatchMu.Unlock()
	var req protocol.ScreenWatchRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	sharerID := strings.TrimSpace(req.SharerID)
	receiveID := strings.TrimSpace(req.ReceiveID)
	if channelID == "" || sharerID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id and sharer_id are required", env.ID)
		return
	}
	c.hub.mu.RLock()
	optIn := c.screenWatchOptIn
	isolated := c.screenWatchIsolated
	c.hub.mu.RUnlock()
	_, mode := c.hub.modeSnapshot()
	if !optIn {
		c.replyError(env.Type, protocol.CodeForbidden,
			"screen.watch requires the screen.watch.v1 capability in auth; legacy clients keep automatic viewing", env.ID)
		return
	}
	strict := receiveID != ""
	if isolated && !strict {
		// 声明了严格能力的连接必须带会话身份；否则只能得到旧混合 PC 的
		// sink-only 语义，等于静默降级——直接拒绝，要求客户端补齐 receive_id。
		c.replyError(env.Type, protocol.CodeInvalidArgument,
			"receive_id is required for connections that opted into screen.watch.isolated.v1", env.ID)
		return
	}
	if strict {
		if !isolated {
			// 严格接收传输要求 client opt-in + server 支持（relay + 视频通道）。
			// 缺任何一层都明确报不支持，绝不退回旧混合槽位冒充成功。
			code := protocol.CodeNotImplemented
			msg := "server does not support screen.watch.isolated.v1 in the current screen mode; strict receive transport is unavailable"
			if mode != "relay" {
				msg = "strict receive transport is not implemented for direct screen mode yet (B2b open); refusing to degrade to legacy viewing"
			}
			c.replyError(env.Type, code, msg, env.ID)
			return
		}
		if !validReceiveID(receiveID) {
			c.replyError(env.Type, protocol.CodeInvalidArgument,
				"receive_id must be 8..128 chars of [A-Za-z0-9_-]", env.ID)
			return
		}
	}
	if sharerID == c.member.ID {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "cannot watch your own share", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}

	// 目标存在性、跨连接 owner 检查、代次推进与「正在建立」占位必须在**同一段
	// h.mu** 内完成：两个同成员连接各自先读快照、再各自写 owner 的窗口，正是
	// 「SFU 里活着一个会话、hub 记录另一个」的来源。锁内只改内存状态，不等
	// ICE/Close/DB；真正的接收传输在锁外建。
	c.hub.mu.Lock()
	prev, hadPrev := c.hub.screenWatches[channelID][c.member.ID]
	// 接收 lease 属于「发起 watch 的连接」：同一成员的另一条连接不能顶掉/停止
	// 它正在用的接收会话（跨连接不误清）。要切换必须先在该连接上 unwatch。
	if hadPrev && prev.Owner != nil && prev.Owner != c {
		c.hub.mu.Unlock()
		c.replyError(env.Type, protocol.CodeConflict,
			"another connection of this member owns the current screen watch", env.ID)
		return
	}
	if claim, ok := c.hub.screenWatchClaims[channelID][c.member.ID]; ok && claim.conn != c {
		c.hub.mu.Unlock()
		c.replyError(env.Type, protocol.CodeConflict,
			"another connection of this member is already opening a screen watch", env.ID)
		return
	}
	if _, sharing := c.hub.shares[channelID][sharerID]; !sharing {
		c.hub.mu.Unlock()
		c.replyError(env.Type, protocol.CodeNotFound, "no active share from this member", env.ID)
		return
	}
	viewers := 0
	for viewerID, watch := range c.hub.screenWatches[channelID] {
		if watch.Target == sharerID && viewerID != c.member.ID {
			viewers++
		}
	}
	// direct 屏幕共享是 P2P 1v1（见 voice.go 注释）：服务端只能保护现有上限，
	// 不为第三个观看者偷偷建 mesh。明确报容量错误，客户端应展示可操作的提示。
	if mode == "direct" && viewers >= 1 {
		c.hub.mu.Unlock()
		c.replyError(env.Type, protocol.CodeConflict,
			"direct screen share supports one viewer at a time (P2P 1v1); use --screen-mode=relay for multi-viewer", env.ID)
		return
	}
	// 先推进代次并占位：即使下面建接收传输失败，也不会复用旧 generation；
	// 同一时刻该成员只允许一个 watch 在建（另一条连接会得到 CONFLICT）。
	if c.hub.screenWatchGen[channelID] == nil {
		c.hub.screenWatchGen[channelID] = make(map[string]uint64, 2)
	}
	c.hub.screenWatchGen[channelID][c.member.ID]++
	gen := c.hub.screenWatchGen[channelID][c.member.ID]
	if c.hub.screenWatchClaims[channelID] == nil {
		c.hub.screenWatchClaims[channelID] = make(map[string]screenWatchClaim, 2)
	}
	c.hub.screenWatchClaims[channelID][c.member.ID] = screenWatchClaim{conn: c, gen: gen, target: sharerID}
	c.hub.mu.Unlock()
	// 兜底：任何提前返回都撤销占位；提交成功后占位已被删除，这里是 no-op。
	defer c.hub.releaseScreenWatchClaim(c, channelID, gen)

	room := c.hub.videoRoom(channelID)
	slot := -1
	var sessionID, nonce string
	if strict && mode == "direct" {
		// ---- B2b：direct 严格接收 = 会话经纪（两端各自建 PC，服务端只路由）----
		// 共享者的发布连接必须声明 screen.publish.sessions.v1；否则明确不支持，
		// 不退回「旧共享 PC 上暂停/恢复」冒充隔离。
		c.hub.mu.RLock()
		sharerConn := c.hub.directPublishConnLocked(channelID, sharerID)
		c.hub.mu.RUnlock()
		if sharerConn == nil {
			c.replyError(env.Type, protocol.CodeNotImplemented,
				"peer does not support per-viewer direct publish sessions (screen.publish.sessions.v1); strict direct receive is unavailable", env.ID)
			return
		}
		sessionNonce, nonceErr := makeDirectSessionNonce()
		if nonceErr != nil {
			c.replyError(env.Type, protocol.CodeInternal, nonceErr.Error(), env.ID)
			return
		}
		session := &directScreenSession{
			id:         receiveID,
			channelID:  channelID,
			nonce:      sessionNonce,
			generation: gen,
			viewerID:   c.member.ID,
			viewerName: c.member.Name,
			viewerConn: c,
			sharerID:   sharerID,
			sharerConn: sharerConn,
		}
		replaced, committed, commitErr := c.hub.commitDirectScreenWatch(c, channelID, gen, screenWatch{
			Target:     sharerID,
			Slot:       -1,
			Generation: gen,
			SessionID:  receiveID,
			Nonce:      sessionNonce,
			Owner:      c,
			Isolated:   true,
			Transport:  "direct",
		}, session)
		if commitErr != nil {
			// session id 被另一个仍活着的会话占用：明确冲突，不覆盖、不改旧 watch。
			c.replyError(env.Type, protocol.CodeConflict, commitErr.Error(), env.ID)
			return
		}
		if !committed {
			c.replyError(env.Type, protocol.CodeConflict,
				"screen watch was cancelled or the share stopped while the direct session was being armed", env.ID)
			return
		}
		// 旧会话（同 viewer 的上一目标/上一代次）与 legacy 暂停先撤销，再 arm 新会话。
		if replaced != nil {
			c.hub.notifyDirectSession(replaced, replaced.sharerConn, protocol.MediaSignalKindViewerClose)
		}
		if hadPrev && !prev.Isolated {
			// 从 legacy direct 观看切到严格接收：旧路径是「一条 PC 发给所有人」，
			// 只能整体暂停该共享者的 legacy 发送（1v1 下只有这一个 viewer）。
			c.hub.notifyDirectViewerClosed(channelID, sharerID, c.member.ID)
		}
		c.hub.notifyDirectSession(session, sharerConn, protocol.MediaSignalKindViewerArm)
		sessionID, nonce = receiveID, sessionNonce
		c.replyOK(env, protocol.ScreenWatchOK{
			ChannelID:  channelID,
			SharerID:   sharerID,
			Slot:       -1,
			Generation: gen,
			Transport:  "direct",
			SessionID:  sessionID,
			Nonce:      nonce,
			Isolated:   true,
		})
		c.hub.log.Info("screen watch armed (direct isolated)", "channel", channelID,
			"viewer", c.member.Name, "target", sharerID, "session", receiveID, "generation", gen)
		c.hub.broadcastScreenState(channelID)
		return
	}
	if strict {
		if room == nil {
			c.replyError(env.Type, protocol.CodeConflict, "screen relay room is not ready for this channel", env.ID)
			return
		}
		// 从旧混合槽位路径切到严格接收：先解绑旧槽，避免两路同时交付。
		if hadPrev && !prev.Isolated {
			room.Unwatch(c.member.ID)
		}
		offer, err := room.OpenReceive(c.member.ID, sharerID, gen, receiveID)
		if err != nil {
			code := protocol.CodeInternal
			if errors.Is(err, sfu.ErrNotInRoom) || errors.Is(err, sfu.ErrReceiveSession) {
				code = protocol.CodeConflict
			}
			c.replyError(env.Type, code, err.Error(), env.ID)
			return
		}
		sessionID, nonce = offer.SessionID, offer.Nonce

		// 接收 PC 与 lease 已建立，才把 offer 只发给这条连接；原生在此之前已用
		// receive_id 建好接收会话（identity 先于信令与媒体）。
		envelope, err := json.Marshal(map[string]string{"type": "offer", "sdp": offer.SDP})
		if err != nil {
			room.CloseReceive(c.member.ID, sessionID)
			c.replyError(env.Type, protocol.CodeInternal, "could not encode receive offer", env.ID)
			return
		}
		// 提交前重新验证：占位仍属于本连接/本代次（未被断线/停发撤销），且目标
		// 此刻仍在共享。任何一项不成立就关掉刚建的接收传输——绝不留下没人认领的
		// lease，也不用旧 lease 冒充成功。
		if !c.hub.commitScreenWatch(c, channelID, gen, screenWatch{
			Target:     sharerID,
			Slot:       -1,
			Generation: gen,
			SessionID:  sessionID,
			Nonce:      nonce,
			Owner:      c,
			Isolated:   true,
			Transport:  mode,
		}) {
			room.CloseReceive(c.member.ID, sessionID)
			c.replyError(env.Type, protocol.CodeConflict,
				"screen watch was cancelled or the share stopped while the receive transport was being created", env.ID)
			return
		}

		c.enqueue(protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
			ChannelID:    channelID,
			Target:       "sfu",
			From:         "sfu",
			FromName:     "sfu",
			Media:        "screen",
			Kind:         "offer",
			SessionID:    sessionID,
			SessionNonce: nonce,
			Generation:   gen,
			Payload:      string(envelope),
		}))
		c.hub.log.Info("screen watch armed (isolated)", "channel", channelID,
			"viewer", c.member.Name, "target", sharerID, "session", sessionID, "generation", gen)
		c.replyOK(env, protocol.ScreenWatchOK{
			ChannelID:  channelID,
			SharerID:   sharerID,
			Slot:       -1,
			Generation: gen,
			Transport:  mode,
			SessionID:  sessionID,
			Nonce:      nonce,
			Isolated:   true,
		})
		c.hub.broadcastScreenState(channelID)
		return
	}

	// ---- legacy / v1.1 混合 PC 槽位路径（未声明 isolated 的客户端） ----
	if mode == "direct" && hadPrev && prev.Isolated {
		// 从严格接收切回 legacy direct：撤销该会话（共享者收到 viewer_close，
		// 关闭它为此 viewer 建的独立发送传输），再按旧路径 arm。
		c.hub.closeDirectSession(channelID, prev.SessionID)
	}
	if mode == "relay" {
		if room == nil {
			c.replyError(env.Type, protocol.CodeConflict, "screen relay room is not ready for this channel", env.ID)
			return
		}
		// 从严格接收切回旧混合槽位路径：先关旧接收 PC，避免两路并存。
		if hadPrev && prev.Isolated {
			room.CloseReceive(c.member.ID, prev.SessionID)
		}
		var err error
		slot, err = room.Watch(c.member.ID, sharerID)
		if err != nil {
			code := protocol.CodeInternal
			if errors.Is(err, sfu.ErrNotInRoom) {
				code = protocol.CodeConflict
			}
			c.replyError(env.Type, code, err.Error(), env.ID)
			return
		}
	}

	// 选台成功后才更新服务端事实。generation 单调递增，进程内唯一。
	// 提交同样要重新验证占位：watch 在建期间用户可能已经 stop/断线。
	if !c.hub.commitScreenWatch(c, channelID, gen, screenWatch{
		Target:     sharerID,
		Slot:       slot,
		Generation: gen,
		Owner:      c,
		Transport:  mode,
	}) {
		if mode == "relay" && room != nil {
			room.Unwatch(c.member.ID)
		}
		c.replyError(env.Type, protocol.CodeConflict,
			"screen watch was cancelled while it was being armed", env.ID)
		return
	}

	if hadPrev && prev.Target != "" && prev.Target != sharerID {
		c.hub.log.Info("screen watch switched", "channel", channelID, "viewer", c.member.Name,
			"from", prev.Target, "to", sharerID, "slot", slot)
	} else {
		c.hub.log.Info("screen watch armed", "channel", channelID, "viewer", c.member.Name,
			"target", sharerID, "slot", slot, "generation", gen)
	}
	c.replyOK(env, protocol.ScreenWatchOK{
		ChannelID:  channelID,
		SharerID:   sharerID,
		Slot:       slot,
		Generation: gen,
		Transport:  mode,
	})
	// direct 严格接收未实现（B2b open），但 legacy 观看者重新点选时通知共享者
	// 恢复向该 viewer 的发送（对应 viewer_close 的逆操作）。
	if mode == "direct" {
		c.hub.notifyDirectViewerArm(channelID, sharerID, c.member.ID)
	}
	c.hub.broadcastScreenState(channelID)
}

// releaseScreenWatchClaim 撤销本连接为 (channel, gen) 建立的「正在 watch」占位。
// 只撤销完全匹配的占位：另一条连接或新一代次的 claim 不会被误删。
func (h *Hub) releaseScreenWatchClaim(c *conn, channelID string, gen uint64) {
	h.mu.Lock()
	defer h.mu.Unlock()
	claim, ok := h.screenWatchClaims[channelID][c.member.ID]
	if !ok || claim.conn != c || claim.gen != gen {
		return
	}
	delete(h.screenWatchClaims[channelID], c.member.ID)
	if len(h.screenWatchClaims[channelID]) == 0 {
		delete(h.screenWatchClaims, channelID)
	}
}

// commitScreenWatch 把「正在 watch」占位换成正式选台。
//
// 提交是锁内的一次 CAS：占位必须仍是本连接/本代次（断线、停发、unwatch 会撤销它），
// 且目标此刻仍在共享。任一项不成立就返回 false，由调用方关闭刚建的接收传输——
// 这样不会出现「hub 记录了一个目标已停发/已被换掉的 lease」或悬空 owner。
func (h *Hub) commitScreenWatch(c *conn, channelID string, gen uint64, watch screenWatch) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	claim, ok := h.screenWatchClaims[channelID][c.member.ID]
	if !ok || claim.conn != c || claim.gen != gen {
		return false
	}
	if _, sharing := h.shares[channelID][watch.Target]; !sharing {
		delete(h.screenWatchClaims[channelID], c.member.ID)
		if len(h.screenWatchClaims[channelID]) == 0 {
			delete(h.screenWatchClaims, channelID)
		}
		return false
	}
	if h.screenWatches[channelID] == nil {
		h.screenWatches[channelID] = make(map[string]screenWatch, 2)
	}
	h.screenWatches[channelID][c.member.ID] = watch
	delete(h.screenWatchClaims[channelID], c.member.ID)
	if len(h.screenWatchClaims[channelID]) == 0 {
		delete(h.screenWatchClaims, channelID)
	}
	return true
}

// validReceiveID 只允许 URL-safe 的短标识；长度上限防止日志/协议字段被滥用。
func validReceiveID(id string) bool {
	if len(id) < 8 || len(id) > 128 {
		return false
	}
	for _, r := range id {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_':
		default:
			return false
		}
	}
	return true
}

// handleScreenUnwatch 停止当前观看目标（幂等）。
//
// 两种语义（见 protocol.ScreenUnwatchRequest）：
//   - receive_id 非空：严格接收路径按**会话**精确撤租。只用「本连接所有权 +
//     session 完全匹配」判定；sharer_id 不参与身份判定。会话已关闭/属于同成员
//     另一条连接/已被新会话取代时幂等返回空 SharerID，绝不清当前新 lease。
//   - receive_id 为空：v1.1 legacy 目标语义不变。
//
// relay：先清服务端选台，再让视频房间关闭对应接收 PC/解绑槽（网络层停止转发）；
// direct：只清选台——P2P 的 RTP 停收由原生 pu_screen_unwatch 侧关掉 sink/解码，
// 服务端不介入两端协商（也不能假装能停对端）。
func (c *conn) handleScreenUnwatch(env protocol.Envelope) {
	c.screenWatchMu.Lock()
	defer c.screenWatchMu.Unlock()
	var req protocol.ScreenUnwatchRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	want := strings.TrimSpace(req.SharerID)
	receiveID := strings.TrimSpace(req.ReceiveID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}
	strict := receiveID != ""
	if strict {
		c.hub.mu.RLock()
		isolated := c.screenWatchIsolated
		c.hub.mu.RUnlock()
		if !isolated {
			c.replyError(env.Type, protocol.CodeInvalidArgument,
				"receive_id requires the screen.watch.isolated.v1 capability", env.ID)
			return
		}
		if !validReceiveID(receiveID) {
			c.replyError(env.Type, protocol.CodeInvalidArgument,
				"receive_id must be 8..128 chars of [A-Za-z0-9_-]", env.ID)
			return
		}
	}

	c.hub.mu.Lock()
	watch, ok := c.hub.screenWatches[channelID][c.member.ID]
	if ok && watch.Owner != nil && watch.Owner != c {
		// 接收 lease 属于另一条连接：这条连接不能替它停（跨连接不误清）。
		c.hub.mu.Unlock()
		c.replyOK(env, protocol.ScreenUnwatchOK{ChannelID: channelID, SharerID: ""})
		return
	}
	if strict {
		// 严格身份 = 连接所有权 + 会话完全匹配；目标字段不是身份。旧 target 相同
		// 也可能是新会话，旧 target 不同也可能就是当前会话（客户端可只带
		// receive_id）。不匹配就什么都不清，按幂等成功返回空 SharerID。
		if !ok || !watch.Isolated || watch.SessionID != receiveID {
			c.hub.mu.Unlock()
			c.replyOK(env, protocol.ScreenUnwatchOK{ChannelID: channelID, SharerID: ""})
			return
		}
	} else if ok && want != "" && watch.Target != want {
		// 目标已切换：不要误停当前的新目标。
		c.hub.mu.Unlock()
		c.replyOK(env, protocol.ScreenUnwatchOK{ChannelID: channelID, SharerID: ""})
		return
	}
	if ok {
		delete(c.hub.screenWatches[channelID], c.member.ID)
		if len(c.hub.screenWatches[channelID]) == 0 {
			delete(c.hub.screenWatches, channelID)
		}
	}
	c.hub.mu.Unlock()

	if ok {
		room := c.hub.videoRoom(channelID)
		switch {
		case watch.Isolated && watch.Transport == "direct":
			// direct 严格会话：撤销服务端会话身份并通知共享者关闭**该 viewer** 的
			// 独立发送传输。观看者的接收 PC 已由客户端在本端关闭；这里不碰任何
			// 发布 PC、采集源或语音。
			c.hub.closeDirectSession(channelID, watch.SessionID)
		case watch.Isolated:
			// relay：只关闭这条接收 PC；发布 PC/发布轨/采集/语音完全不动。
			if room != nil {
				room.CloseReceive(c.member.ID, watch.SessionID)
			}
		case watch.Slot >= 0 && room != nil:
			room.Unwatch(c.member.ID)
		}
		if watch.Transport == "direct" && !watch.Isolated {
			// legacy direct：服务端不能替共享者关发送，必须显式通知它停止对该
			// viewer 的发送（共享者保留采集/轨/发布状态，不是全局 screen.stop）。
			c.hub.notifyDirectViewerClosed(channelID, watch.Target, c.member.ID)
		}
		c.hub.log.Info("screen watch cleared", "channel", channelID,
			"viewer", c.member.Name, "target", watch.Target, "isolated", watch.Isolated,
			"session", watch.SessionID)
	}
	c.replyOK(env, protocol.ScreenUnwatchOK{ChannelID: channelID, SharerID: watch.Target})
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
	_, screenMode := c.hub.modeSnapshot()
	c.hub.mu.RLock()
	// 语音按**频道实际 transport** 路由（不是策略值）：direct 失败降级后，
	// 媒体信令要立刻改走 SFU，否则会继续按 direct 转发给成员、永远连不上。
	voiceMode, _ := c.hub.channelTransportLocked(channelID)
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
			sessionID := strings.TrimSpace(req.SessionID)
			sessionNonce := strings.TrimSpace(req.SessionNonce)
			if sessionID != "" {
				// B2 严格接收：信令必须落到**当前连接持有的接收 lease**，
				// session/nonce 任一不匹配都按会话拒绝；旧 answer/ICE 不会
				// 进入新接收器。
				c.hub.mu.RLock()
				watch, ok := c.hub.screenWatches[channelID][c.member.ID]
				c.hub.mu.RUnlock()
				if !ok || !watch.Isolated || watch.SessionID != sessionID || watch.Owner != c ||
					watch.Nonce == "" ||
					subtle.ConstantTimeCompare([]byte(sessionNonce), []byte(watch.Nonce)) != 1 {
					c.replyError(env.Type, protocol.CodeConflict,
						"screen signal does not match the current receive session/nonce", env.ID)
					return
				}
				room := c.hub.videoRoom(channelID)
				if room == nil {
					c.replyError(env.Type, protocol.CodeConflict, "screen relay room is not ready", env.ID)
					return
				}
				if err := room.HandleReceiveSignal(c.member.ID, sessionID, sessionNonce, kind,
					[]byte(req.Payload)); err != nil {
					code := protocol.CodeInvalidArgument
					if errors.Is(err, sfu.ErrReceiveSession) {
						code = protocol.CodeConflict
					}
					c.replyError(env.Type, code, err.Error(), env.ID)
					return
				}
				c.replyOK(env, protocol.MediaSignalOK{ChannelID: channelID})
				return
			}
			// 该成员当前持有严格接收 lease 时，不允许用不带 session 的信令冒充；
			// 否则旧客户端路径可能把 answer/ICE 灌进另一条会话。
			c.hub.mu.RLock()
			watch, hasWatch := c.hub.screenWatches[channelID][c.member.ID]
			c.hub.mu.RUnlock()
			if hasWatch && watch.Isolated {
				c.replyError(env.Type, protocol.CodeConflict,
					"this viewer holds an isolated receive session; signal must carry session_id/session_nonce", env.ID)
				return
			}
			// 视频 PC 的信令对象是服务端：answer/candidate 交给视频房间。
			if err := c.hub.handleScreenSignal(c.member.ID, channelID, kind, req.Payload); err != nil {
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
				"screen_mode=direct 时不应带 target=sfu", env.ID)
			return
		}
		sessionID := strings.TrimSpace(req.SessionID)
		if sessionID != "" {
			// B2b direct：信令只按会话投递给对端连接，不广播频道。
			if err := c.hub.routeDirectScreenSignal(c, channelID, &req); err != nil {
				c.replyError(env.Type, protocol.CodeConflict, err.Error(), env.ID)
				return
			}
			c.replyOK(env, protocol.MediaSignalOK{ChannelID: channelID})
			return
		}
		// 该连接持有严格 direct 会话时，不允许用不带 session 的信令冒充：
		// 广播会把 answer/ICE 交给无关成员，旧会话也可能被新会话接收。
		c.hub.mu.RLock()
		watch, hasWatch := c.hub.screenWatches[channelID][c.member.ID]
		c.hub.mu.RUnlock()
		if hasWatch && watch.Isolated && watch.Owner == c {
			c.replyError(env.Type, protocol.CodeConflict,
				"this viewer holds an isolated direct receive session; signal must carry session_id/session_nonce", env.ID)
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
		// 持有严格 direct 会话的观看者只接受按会话路由的信令：广播的 legacy
		// offer/ICE 不得进入它的接收器（否则两条传输会在同一端串话）。
		if media == "screen" && screenMode == "direct" {
			if w, ok := c.hub.screenWatches[channelID][peer.member.ID]; ok &&
				w.Isolated && w.Transport == "direct" {
				continue
			}
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
