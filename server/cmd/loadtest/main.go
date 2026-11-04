// Command loadtest 用 pion 假客户端压测 PartyUp 的 SFU 中继容量。
//
// 它和 internal/server 里的 relayPeer 测试夹具是同一套思路（服务端是 offerer，
// 客户端回 answer 并往 mic 轨道里灌 20ms 一帧的假 Opus），但这里**不启服务端**，
// 而是连一个已经跑起来的 partyup 进程，这样测出来的 CPU/内存才是纯服务端的。
//
// 用法（服务端需以 --voice-mode=relay 启动）：
//
//	loadtest -server http://127.0.0.1:19001 \
//	         -admin-token "$ADMIN_TOKEN" -channel-name scale \
//	         -tokens-file /tmp/tokens.txt -members 12 -duration 20s
//
// tokens 文件每行一个成员 token（可写成 "name=token" 形式，便于看日志）。
// 没有 -admin-token 时必须用 -channel-id 指定一个已存在的频道。
//
// 输出：每个成员收到的远端音轨数 / RTP 包数，以及一行可由脚本解析的 JSON 汇总。
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"

	"partyup/internal/protocol"
)

func main() {
	var (
		server      = flag.String("server", "http://127.0.0.1:19001", "服务端 HTTP 地址")
		adminToken  = flag.String("admin-token", "", "管理员 token（用于自动建频道）")
		channelID   = flag.String("channel-id", "", "已有频道 id（与 -admin-token 二选一）")
		channelName = flag.String("channel-name", "loadtest", "自动创建的频道名前缀（多频道为 <name>-0..N-1）")
		channels    = flag.Int("channels", 1, "频道数：成员按 i%channels 分摊到各频道（50 人用 5 频道 × 10 人）")
		tokensFile  = flag.String("tokens-file", "", "成员 token 文件（每行一个，可 name=token）")
		members     = flag.Int("members", 12, "参与压测的成员数")
		duration    = flag.Duration("duration", 20*time.Second, "所有成员进语音后持续压测的时长")
		joinTimeout = flag.Duration("join-timeout", 90*time.Second, "等待全部成员进语音的超时")
		jsonOut     = flag.String("json", "", "把汇总 JSON 写到这个文件（空则只打印表格）")
		shares      = flag.Int("share", 0, "有屏幕共享的频道数（每个频道 1 个假共享者，其余成员观看）")
		videoFPS    = flag.Int("video-fps", 30, "假共享者的帧率")
		videoBytes  = flag.Int("video-frame-bytes", 6000, "假共享者每帧字节数（6000B×30fps≈1.4 Mbps，接近真实 1440p 共享）")
	)
	flag.Parse()

	if *tokensFile == "" {
		fatalf("-tokens-file is required")
	}
	tokens, err := readTokens(*tokensFile)
	if err != nil {
		fatalf("read tokens: %v", err)
	}
	if len(tokens) < *members {
		fatalf("tokens file has %d entries, need %d", len(tokens), *members)
	}

	// 1) 管理员：建频道（如果没指定已有频道）。
	chanIDs := make([]string, 0, *channels)
	if *channelID != "" {
		chanIDs = append(chanIDs, *channelID)
	} else {
		if *adminToken == "" {
			fatalf("either -channel-id or -admin-token is required")
		}
		admin, err := newClient(*server, "admin", *adminToken)
		if err != nil {
			fatalf("admin client: %v", err)
		}
		if err := admin.connect(); err != nil {
			fatalf("admin connect: %v", err)
		}
		for i := 0; i < *channels; i++ {
			name := *channelName
			if *channels > 1 {
				name = fmt.Sprintf("%s-%d", *channelName, i)
			}
			ch, err := admin.createChannel(name)
			if err != nil {
				fatalf("create channel %q: %v", name, err)
			}
			chanIDs = append(chanIDs, ch)
			fmt.Printf("CHANNEL_ID=%s name=%s\n", ch, name)
		}
		admin.close()
	}

	// 2) 建 N 个成员并让它们全部进语音。
	clients := make([]*client, 0, *members)
	var joined atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < *members; i++ {
		name, token := tokens[i].name, tokens[i].token
		c, err := newClient(*server, name, token)
		if err != nil {
			fatalf("%s: connect: %v", name, err)
		}
		clients = append(clients, c)
		wg.Add(1)
		chID := chanIDs[i%len(chanIDs)]
		go func() {
			defer wg.Done()
			if err := c.joinVoice(chID); err != nil {
				fmt.Printf("JOIN_FAILED name=%s err=%v\n", c.name, err)
				return
			}
			joined.Add(1)
		}()
		// 错开 150ms：避免 N 个客户端在同一毫秒同时打 WS/SDP。
		time.Sleep(150 * time.Millisecond)
	}
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(*joinTimeout):
		fatalf("timed out waiting for members to join voice (%d/%d joined)", joined.Load(), *members)
	}
	if int(joined.Load()) != *members {
		fatalf("only %d/%d members joined voice", joined.Load(), *members)
	}
	// 2.5) 屏幕共享：每个指定频道挑第 1 个成员当假共享者（其余成员是观看者）。
	//      screen.start 会触发服务端**重建**共享者的视频 PC 并重发 offer，所以给 2s 让
	//      重协商完成，再开始计时——否则前几秒的视频流量会漏统计。
	sharers := 0
	for chIdx := 0; chIdx < *shares && chIdx < len(chanIDs); chIdx++ {
		for _, c := range clients {
			if c.channelID != chanIDs[chIdx] || c.isSharer {
				continue
			}
			c.isSharer = true
			c.videoFPS = *videoFPS
			c.videoBytes = *videoBytes
			sharers++
			go func(cc *client) {
				if err := cc.startShare(); err != nil {
					fmt.Printf("SHARE_FAILED name=%s err=%v\n", cc.name, err)
				}
			}(c)
			break
		}
	}
	if sharers > 0 {
		time.Sleep(2 * time.Second)
	}
	fmt.Printf("LOAD=ready members=%d sharers=%d\n", *members, sharers)

	// 3) 压测：每个成员都在说话（20ms 一帧），期间统计每端收到的包。
	startEdges := takeSnapshot(clients)
	startVideo := make([]int64, len(clients))
	for i, c := range clients {
		_, startVideo[i] = c.videoSnapshot()
	}
	time.Sleep(*duration)

	// 4) 汇总。
	elapsed := duration.Seconds()
	type peerStat struct {
		Name        string `json:"name"`
		Tracks      int    `json:"tracks"`
		Publishers  int    `json:"publishers"`
		Packets     int64  `json:"packets"`
		PPS         int64  `json:"pps"`
		VideoTracks int    `json:"video_tracks"`
		VideoPPS    int64  `json:"video_pps"`
		Sharer      bool   `json:"sharer"`
	}
	stats := make([]peerStat, 0, len(clients))
	// 每个频道的人数（i%channels 分摊），据此算每人应收到几路。
	chanCounts := make(map[string]int, len(chanIDs))
	expectByPeer := make([]int, len(clients))
	for i := range clients {
		chanCounts[chanIDs[i%len(chanIDs)]]++
	}
	for i, c := range clients {
		expectByPeer[i] = chanCounts[c.channelID] - 1
	}
	minTracks, minPublishers, minDeficit := 1<<30, 1<<30, 0
	var totalPPS, totalVideoPPS int64
	videoViewers := 0
	var sharerFrames int64
	for i, c := range clients {
		tracks, publishers, packets := c.snapshot()
		delta := packets - startEdges[i]
		pps := int64(float64(delta) / elapsed)
		vtracks, vpackets := c.videoSnapshot()
		vpps := int64(float64(vpackets-startVideo[i]) / elapsed)
		if vtracks > 0 {
			videoViewers++
		}
		if c.isSharer {
			sharerFrames += c.videoSendSnapshot()
		}
		stats = append(stats, peerStat{
			Name: c.name, Tracks: tracks, Publishers: publishers, Packets: packets, PPS: pps,
			VideoTracks: vtracks, VideoPPS: vpps, Sharer: c.isSharer,
		})
		totalVideoPPS += vpps
		if tracks < minTracks {
			minTracks = tracks
		}
		if publishers < minPublishers {
			minPublishers = publishers
		}
		if d := expectByPeer[i] - tracks; d > minDeficit {
			minDeficit = d
		}
		totalPPS += pps
	}
	sort.Slice(stats, func(i, j int) bool { return stats[i].Name < stats[j].Name })
	// 单频道时 expect=members-1；多频道时每频道人数可能不同，取各频道期望的最小值。
	minExpect := 1 << 30
	for _, n := range chanCounts {
		if n-1 < minExpect {
			minExpect = n - 1
		}
	}
	chanSummary := make(map[string]int, len(chanCounts))
	for _, ch := range chanIDs {
		chanSummary[ch] = chanCounts[ch]
	}

	summary := map[string]any{
		"members":         *members,
		"channels":        len(chanIDs),
		"channel_counts":  chanSummary,
		"duration_s":      elapsed,
		"min_tracks":      minTracks,
		"min_publishers":  minPublishers,
		"expect_tracks":   minExpect, // 各频道「其余成员数」的最小值
		"max_deficit":     minDeficit,
		"total_pps":       totalPPS,
		"avg_pps":         totalPPS / int64(*members),
		"sharers":         sharers,
		"video_viewers":   videoViewers,
		"total_video_pps": totalVideoPPS,
		"avg_video_pps":   totalVideoPPS / int64(max(1, videoViewers)),
		"peers":           stats,
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	fmt.Println("SUMMARY_JSON_BEGIN")
	_ = enc.Encode(summary)
	fmt.Println("SUMMARY_JSON_END")
	if *jsonOut != "" {
		if f, err := os.Create(*jsonOut); err == nil {
			_ = json.NewEncoder(f).Encode(summary)
			_ = f.Close()
		}
	}

	for _, c := range clients {
		c.close()
	}

	if minDeficit > 0 {
		fmt.Printf("RESULT=degraded min_tracks=%d min_expect=%d max_deficit=%d\n",
			minTracks, minExpect, minDeficit)
		os.Exit(1)
	}
	fmt.Printf("RESULT=ok min_tracks=%d min_expect=%d avg_pps=%d\n", minTracks, minExpect, summary["avg_pps"])
}

type tokenEntry struct{ name, token string }

func readTokens(path string) ([]tokenEntry, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var out []tokenEntry
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		name, token := "", line
		if i := strings.IndexByte(line, '='); i > 0 {
			name, token = line[:i], line[i+1:]
		}
		if name == "" {
			name = fmt.Sprintf("m%d", len(out))
		}
		out = append(out, tokenEntry{name: name, token: token})
	}
	return out, nil
}

func takeSnapshot(clients []*client) []int64 {
	out := make([]int64, len(clients))
	for i, c := range clients {
		_, _, out[i] = c.snapshot()
	}
	return out
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "loadtest: "+format+"\n", args...)
	os.Exit(2)
}

// ---------------------------------------------------------------------------
// 一个假客户端
// ---------------------------------------------------------------------------

type client struct {
	name      string
	serverURL string
	token     string

	ws   *websocket.Conn
	pc   *webrtc.PeerConnection
	mic  *webrtc.TrackLocalStaticSample
	done chan struct{}

	memberID  string
	channelID string

	// 屏幕共享（视频中继）：服务端在 subscribe 时就会下发 screen offer（观看者身份），
	// 共享者 screen.start 后服务端还会重建视频 PC 再发一次 offer（要带轨才发得出去）。
	screenPC   *webrtc.PeerConnection
	videoTrack *webrtc.TrackLocalStaticSample
	isSharer   bool
	videoFPS   int
	videoBytes int

	mu               sync.Mutex
	recv             map[string]int64 // 发布者 member id（= 远端 track id）→ 收到的 RTP 包数
	videoPackets     int64
	videoFramesSent  int64
	videoSenderReady bool
	videoTracks      map[string]struct{}
	tracks           map[string]struct{}
	packets          int64
	sfuOffers        int
	requests         int
	closeOnce        sync.Once
	writeMu          sync.Mutex
	nextID           int
	pendingMu        sync.Mutex
	pending          map[string]chan protocol.Envelope
	connErrors       []string
}

func newClient(serverURL, name, token string) (*client, error) {
	c := &client{
		name:        name,
		serverURL:   serverURL,
		token:       token,
		recv:        make(map[string]int64, 16),
		tracks:      make(map[string]struct{}, 16),
		videoTracks: make(map[string]struct{}, 4),
		done:        make(chan struct{}),
		pending:     make(map[string]chan protocol.Envelope, 8),
	}
	return c, nil
}

// joinVoice 建 WS、鉴权、订阅频道、进语音、建 PC 并回 answer。
func (c *client) joinVoice(channelID string) error {
	c.channelID = channelID
	if err := c.connect(); err != nil {
		return err
	}
	if err := c.subscribe(); err != nil {
		c.close()
		return err
	}
	if err := c.ensurePC(); err != nil {
		c.close()
		return err
	}
	resp, err := c.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: channelID})
	if err != nil {
		c.close()
		return err
	}
	if resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
		c.close()
		return fmt.Errorf("voice.join: %s %s", resp.Type, resp.Payload)
	}
	// 服务端进语音后会下发 offer；等它连上。
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if c.pc.ConnectionState() == webrtc.PeerConnectionStateConnected {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if c.pc.ConnectionState() != webrtc.PeerConnectionStateConnected {
		c.close()
		return fmt.Errorf("pc never connected (state=%s)", c.pc.ConnectionState())
	}
	return nil
}

func (c *client) connect() error {
	base := strings.TrimSuffix(strings.TrimSpace(c.serverURL), "/")
	base = strings.Replace(base, "http://", "ws://", 1)
	base = strings.Replace(base, "https://", "wss://", 1)
	wsURL := base + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		return err
	}
	c.ws = conn
	go c.readLoop()

	resp, err := c.request(protocol.TypeAuth, protocol.AuthRequest{Token: c.token})
	if err != nil {
		return err
	}
	if resp.Type != protocol.TypeOK(protocol.TypeAuth) {
		return fmt.Errorf("auth: %s %s", resp.Type, resp.Payload)
	}
	var authOK protocol.AuthOK
	if err := resp.DecodePayload(&authOK); err != nil {
		return err
	}
	c.memberID = authOK.Member.ID
	return nil
}

func (c *client) ensurePC() error {
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return err
	}
	c.pc = pc
	mic, err := webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2},
		"mic", c.name,
	)
	if err != nil {
		return err
	}
	c.mic = mic
	if _, err := pc.AddTransceiverFromTrack(mic, webrtc.RTPTransceiverInit{
		Direction: webrtc.RTPTransceiverDirectionSendonly,
	}); err != nil {
		return err
	}
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		c.mu.Lock()
		c.tracks[track.ID()] = struct{}{}
		c.mu.Unlock()
		for {
			if _, _, err := track.ReadRTP(); err != nil {
				return
			}
			c.mu.Lock()
			c.recv[track.ID()]++
			c.packets++
			c.mu.Unlock()
		}
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		if state == webrtc.PeerConnectionStateFailed || state == webrtc.PeerConnectionStateClosed {
			c.mu.Lock()
			c.connErrors = append(c.connErrors, state.String())
			c.mu.Unlock()
		}
	})
	go c.speakerLoop()
	return nil
}

func (c *client) speakerLoop() {
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-c.done:
			return
		case <-ticker.C:
			select {
			case <-c.done:
				return
			default:
			}
			_ = c.mic.WriteSample(media.Sample{
				Data:     []byte{0xF8, 0xFF, 0xFE}, // 3 字节假 Opus 帧：只要求是 RTP
				Duration: 20 * time.Millisecond,
			})
		}
	}
}

func (c *client) subscribe() error {
	resp, err := c.request(protocol.TypeChannelSub, protocol.ChannelSubscribeRequest{ChannelID: c.channelID})
	if err != nil {
		return err
	}
	if resp.Type != protocol.TypeOK(protocol.TypeChannelSub) {
		return fmt.Errorf("channel.subscribe: %s %s", resp.Type, resp.Payload)
	}
	return nil
}

func (c *client) createChannel(name string) (string, error) {
	resp, err := c.request(protocol.TypeChannelCreate, protocol.ChannelCreateRequest{Name: name})
	if err != nil {
		return "", err
	}
	if resp.Type != protocol.TypeOK(protocol.TypeChannelCreate) {
		return "", fmt.Errorf("channel.create: %s %s", resp.Type, resp.Payload)
	}
	var out protocol.ChannelCreateOK
	if err := resp.DecodePayload(&out); err != nil {
		return "", err
	}
	return out.Channel.ID, nil
}

func (c *client) readLoop() {
	for {
		_, data, err := c.ws.ReadMessage()
		if err != nil {
			return
		}
		var env protocol.Envelope
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		if env.ID != "" {
			c.pendingMu.Lock()
			ch := c.pending[env.ID]
			c.pendingMu.Unlock()
			if ch != nil {
				select {
				case ch <- env:
				default:
				}
				continue
			}
		}
		if env.Type == protocol.TypeMediaSignal {
			var push protocol.MediaSignalPush
			if err := env.DecodePayload(&push); err != nil {
				continue
			}
			// 语音与屏幕各一条 PC：信令必须按 media 分流，否则两条 SDP 会互相覆盖。
			switch push.Media {
			case "voice":
				switch push.Kind {
				case "offer":
					go c.applyOffer(push.Payload)
				case "candidate":
					c.applyCandidate(push.Payload)
				}
			case "screen":
				switch push.Kind {
				case "offer":
					go c.applyScreenOffer(push.Payload)
				case "candidate":
					c.applyScreenCandidate(push.Payload)
				}
			}
		}
	}
}

// applyScreenCandidate 把服务端给视频 PC 的 ICE 候选喂进当前视频 PC。
func (c *client) applyScreenCandidate(payload string) {
	c.mu.Lock()
	pc := c.screenPC
	c.mu.Unlock()
	if pc == nil {
		return
	}
	var msg struct {
		Candidate string `json:"candidate"`
	}
	if err := json.Unmarshal([]byte(payload), &msg); err != nil || msg.Candidate == "" {
		return
	}
	_ = pc.AddICECandidate(webrtc.ICECandidateInit{Candidate: msg.Candidate})
}

// applyScreenOffer 处理服务端的视频 offer。
//
// 关键点：服务端在 screen.start 之后会**重建**共享者的视频 PC 并重发 offer（旧 PC 的那条
// recvonly m-line 在「当观看者」时已经答成「不发」，ABI 不允许重协商）。所以这里每次都
// 建一条全新的 PC，并把本地视频轨挂到 video transceiver 的发送侧——观看者不发帧，
// 共享者随后由 frameLoop 写帧。收到的视频 RTP 计入 videoPackets（衡量转发成本）。
func (c *client) applyScreenOffer(payload string) {
	var msg struct {
		SDP string `json:"sdp"`
	}
	if err := json.Unmarshal([]byte(payload), &msg); err != nil || msg.SDP == "" {
		return
	}
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return
	}
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		c.mu.Lock()
		c.videoTracks[track.ID()] = struct{}{}
		c.mu.Unlock()
		for {
			if _, _, err := track.ReadRTP(); err != nil {
				return
			}
			c.mu.Lock()
			c.videoPackets++
			c.mu.Unlock()
		}
	})

	c.mu.Lock()
	old := c.screenPC
	c.screenPC = pc
	if c.videoTrack == nil {
		// 假 VP8 帧：pion 的 VP8 payloader 只做 RTP 分包，不校验码流，所以任意字节都能作为
		// RTP 负载转发——我们要测的是服务端转发成本，不是解码。
		tr, err := webrtc.NewTrackLocalStaticSample(
			webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000},
			"screen", c.name,
		)
		if err == nil {
			c.videoTrack = tr
		}
	}
	videoTrack := c.videoTrack
	c.mu.Unlock()
	if old != nil {
		_ = old.Close()
	}

	// Pion 的 answerer 侧：只有**先**占一条 sendonly 的视频 transceiver，服务端 offer 的
	// 第一条视频 m-line（recvonly：它收我们的屏幕）才会复用它、SDP 才会答成 sendonly 并
	// 分配 sender。否则 SetRemoteDescription 之后这条 m-line 没有 sender，ReplaceTrack 无处可挂
	// （实测表现为 SCREEN_ATTACH_FAILED、服务端收不到任何视频轨）。
	if videoTrack != nil {
		if _, err := pc.AddTransceiverFromTrack(videoTrack, webrtc.RTPTransceiverInit{
			Direction: webrtc.RTPTransceiverDirectionSendonly,
		}); err != nil {
			fmt.Printf("SCREEN_TRACK_FAILED name=%s err=%v\n", c.name, err)
		}
	}
	if err := pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: msg.SDP,
	}); err != nil {
		return
	}
	// 服务端 offer 里**第一条**视频 m-line 是 recvonly（收我们的屏幕），其余是 sendonly 槽
	// （看别人的屏幕）。所以只给第一条视频 transceiver 挂本地轨——挂到槽位上没意义，
	// 且 Pion 在 answerer 侧对 Direction()/CurrentDirection() 的状态机不适合做判据。
	if videoTrack != nil {
		attached := false
		for _, tr := range pc.GetTransceivers() {
			if tr.Kind() != webrtc.RTPCodecTypeVideo {
				continue
			}
			if tr.Sender() != nil {
				if err := tr.Sender().ReplaceTrack(videoTrack); err == nil {
					c.mu.Lock()
					c.videoSenderReady = true
					c.mu.Unlock()
					attached = true
				}
			}
			break // 只看第一条视频 transceiver
		}
		if !attached {
			fmt.Printf("SCREEN_ATTACH_FAILED name=%s\n", c.name)
		}
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		return
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		return
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		return
	}
	body, _ := json.Marshal(map[string]string{"type": "answer", "sdp": local.SDP})
	_, _ = c.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: c.channelID,
		Target:    "sfu",
		Media:     "screen",
		Kind:      "answer",
		Payload:   string(body),
	})
}

// startShare 让本客户端成为假共享者：screen.start 后服务端会重建视频 PC 再发 offer，
// 帧循环从此开始写假 VP8 帧（30fps × videoBytes）。
func (c *client) startShare() error {
	resp, err := c.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: c.channelID,
		Source: protocol.ScreenSource{
			ID: "synthetic:0", Title: "loadtest synthetic", Width: 2560, Height: 1440, Kind: "synthetic",
		},
	})
	if err != nil {
		return err
	}
	if resp.Type != protocol.TypeOK(protocol.TypeScreenStart) {
		return fmt.Errorf("screen.start: %s %s", resp.Type, resp.Payload)
	}
	go c.frameLoop()
	return nil
}

// frameLoop 每 1/fps 秒写一帧假视频。
func (c *client) frameLoop() {
	interval := time.Second / time.Duration(c.videoFPS)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	frame := make([]byte, c.videoBytes)
	for {
		select {
		case <-c.done:
			return
		case <-ticker.C:
			c.mu.Lock()
			tr := c.videoTrack
			c.mu.Unlock()
			if tr == nil {
				continue
			}
			if err := tr.WriteSample(media.Sample{Data: frame, Duration: interval}); err == nil {
				c.mu.Lock()
				c.videoFramesSent++
				c.mu.Unlock()
			}
		}
	}
}

func (c *client) applyCandidate(payload string) {
	var msg struct {
		Candidate string `json:"candidate"`
	}
	if err := json.Unmarshal([]byte(payload), &msg); err != nil || msg.Candidate == "" {
		return
	}
	_ = c.pc.AddICECandidate(webrtc.ICECandidateInit{Candidate: msg.Candidate})
}

func (c *client) applyOffer(payload string) {
	var msg struct {
		SDP string `json:"sdp"`
	}
	if err := json.Unmarshal([]byte(payload), &msg); err != nil || msg.SDP == "" {
		return
	}
	if err := c.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: msg.SDP,
	}); err != nil {
		return
	}
	// 服务端第一条 m-line 是 recvonly 的麦克风：把自己已有的 sendonly 轨挂上去。
	for _, tr := range c.pc.GetTransceivers() {
		if tr.Kind() == webrtc.RTPCodecTypeAudio && tr.Sender() != nil &&
			tr.Direction() != webrtc.RTPTransceiverDirectionRecvonly {
			if err := tr.Sender().ReplaceTrack(c.mic); err == nil {
				break
			}
		}
	}
	answer, err := c.pc.CreateAnswer(nil)
	if err != nil {
		return
	}
	gathered := webrtc.GatheringCompletePromise(c.pc)
	if err := c.pc.SetLocalDescription(answer); err != nil {
		return
	}
	<-gathered
	local := c.pc.LocalDescription()
	if local == nil {
		return
	}
	body, _ := json.Marshal(map[string]string{"type": "answer", "sdp": local.SDP})
	_, _ = c.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: c.channelID,
		Target:    "sfu",
		Media:     "voice",
		Kind:      "answer",
		Payload:   string(body),
	})
}

// request 发一条带 id 的请求并等对应的 .ok/.err。
func (c *client) request(msgType string, payload any) (protocol.Envelope, error) {
	c.writeMu.Lock()
	c.nextID++
	id := fmt.Sprintf("%s-%d", c.name, c.nextID)
	env, err := protocol.New(msgType, payload)
	if err != nil {
		c.writeMu.Unlock()
		return protocol.Envelope{}, err
	}
	env.ID = id
	data, err := env.Marshal()
	if err != nil {
		c.writeMu.Unlock()
		return protocol.Envelope{}, err
	}
	ch := make(chan protocol.Envelope, 1)
	c.pendingMu.Lock()
	c.pending[id] = ch
	c.pendingMu.Unlock()

	_ = c.ws.SetWriteDeadline(time.Now().Add(10 * time.Second))
	err = c.ws.WriteMessage(websocket.TextMessage, data)
	c.writeMu.Unlock()
	if err != nil {
		return protocol.Envelope{}, err
	}

	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(15 * time.Second):
		return protocol.Envelope{}, fmt.Errorf("timeout waiting for %s", protocol.TypeOK(msgType))
	case <-c.done:
		return protocol.Envelope{}, fmt.Errorf("connection closed")
	}
}

func (c *client) snapshot() (tracks, publishers int, packets int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.tracks), len(c.recv), c.packets
}

// videoSendSnapshot 返回共享者已写出的帧数（诊断用）。
func (c *client) videoSendSnapshot() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.videoFramesSent
}

// videoSnapshot 返回 (收到的远端视频轨数, 收到的视频 RTP 包数)。
func (c *client) videoSnapshot() (tracks int, packets int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.videoTracks), c.videoPackets
}

func (c *client) close() {
	c.closeOnce.Do(func() {
		close(c.done)
		if c.ws != nil {
			_ = c.ws.Close()
		}
		if c.pc != nil {
			_ = c.pc.Close()
		}
	})
}
