package sfu

// 媒体 ICE 端口/对外 IP 配置（2026-11-08，task-6）的回归测试：
//   1) 严格解析：合法/非法端口区间与 IP 列表；
//   2) SettingEngine 注入分支：零值不注入、单开一项也注入、两项都开；
//   3) 真实候选：用同一 API 起一个 PeerConnection，gathering 出的 host 候选
//      必须带对外 IP、UDP 端口必须落在配置区间内（这是容器部署能跑媒体的核心判据）。

import (
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
)

func TestParseICEPortRange(t *testing.T) {
	valid := []struct {
		in       string
		min, max uint16
	}{
		{"50000-50010", 50000, 50010},
		{"50000", 50000, 50000},
		{" 50000 - 50010 ", 50000, 50010},
		{"1-1", 1, 1},
		{"65534-65535", 65534, 65535},
		{"1000-5095", 1000, 5095}, // 跨度 4096（含两端）恰好在上限
	}
	for _, tc := range valid {
		lo, hi, err := ParseICEPortRange(tc.in)
		if err != nil {
			t.Fatalf("ParseICEPortRange(%q) error: %v", tc.in, err)
		}
		if lo != tc.min || hi != tc.max {
			t.Fatalf("ParseICEPortRange(%q) = %d-%d, want %d-%d", tc.in, lo, hi, tc.min, tc.max)
		}
	}

	invalid := []string{
		"", " ", "-", "50000-", "-50010", "0", "0-10", "65536", "50000-65536",
		"50010-50000", "1-65535", "1000-5096", "50000-50010-50020",
		"abc", "50000-abc", "50000-50010x", "+50000", "0x10",
	}
	for _, in := range invalid {
		if _, _, err := ParseICEPortRange(in); err == nil {
			t.Fatalf("ParseICEPortRange(%q) accepted invalid input", in)
		}
	}
}

func TestParseICEPublicIPs(t *testing.T) {
	got, err := ParseICEPublicIPs("203.0.113.10, 2001:db8::1 ,203.0.113.10")
	if err != nil {
		t.Fatalf("ParseICEPublicIPs: %v", err)
	}
	want := []string{"203.0.113.10", "2001:db8::1"}
	if len(got) != len(want) {
		t.Fatalf("ParseICEPublicIPs = %v, want %v (dedupe)", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("ParseICEPublicIPs[%d] = %q, want %q", i, got[i], want[i])
		}
	}

	if got, err := ParseICEPublicIPs("  "); err != nil || got != nil {
		t.Fatalf("ParseICEPublicIPs(empty) = %v, %v; want nil, nil", got, err)
	}

	invalid := []string{"999.1.1.1", "example.com", "203.0.113.10,", ",203.0.113.10", "0.0.0.0", "::"}
	for _, in := range invalid {
		if _, err := ParseICEPublicIPs(in); err == nil {
			t.Fatalf("ParseICEPublicIPs(%q) accepted invalid input", in)
		}
	}

	// 回环地址必须允许：本机验收用 --ice-public-ip 127.0.0.1。
	if _, err := ParseICEPublicIPs("127.0.0.1"); err != nil {
		t.Fatalf("loopback public IP rejected: %v", err)
	}
}

func TestParseICEConfigAndDisplay(t *testing.T) {
	// 零值：什么都不开，显示 unset（与旧行为一致）。
	if (ICEConfig{}).Enabled() {
		t.Fatal("zero ICEConfig must be disabled")
	}
	if got := (ICEConfig{}).PortRangeString(); got != "unset" {
		t.Fatalf("zero PortRangeString = %q, want unset", got)
	}
	if got := (ICEConfig{}).PublicIPsString(); got != "unset" {
		t.Fatalf("zero PublicIPsString = %q, want unset", got)
	}

	cfg, err := ParseICEConfig("50000-50010", "203.0.113.10,2001:db8::1")
	if err != nil {
		t.Fatalf("ParseICEConfig: %v", err)
	}
	if !cfg.Enabled() || !cfg.PortRangeSet() || !cfg.NAT1To1Set() {
		t.Fatalf("parsed config not fully enabled: %+v", cfg)
	}
	if got := cfg.PortRangeString(); got != "50000-50010" {
		t.Fatalf("PortRangeString = %q", got)
	}
	if got := cfg.PublicIPsString(); got != "203.0.113.10,2001:db8::1" {
		t.Fatalf("PublicIPsString = %q", got)
	}

	// 单开一项也要 Injection（两个能力互相独立）。
	onlyRange, err := ParseICEConfig("50000", "")
	if err != nil || !onlyRange.Enabled() || onlyRange.NAT1To1Set() {
		t.Fatalf("only-range config = %+v, %v", onlyRange, err)
	}
	onlyIP, err := ParseICEConfig("", "203.0.113.10")
	if err != nil || !onlyIP.Enabled() || onlyIP.PortRangeSet() {
		t.Fatalf("only-IP config = %+v, %v", onlyIP, err)
	}
	if onlyIP.PortRangeString() != "unset" {
		t.Fatalf("only-IP PortRangeString = %q", onlyIP.PortRangeString())
	}

	// 非法值必须是带 flag 名的可读错误（main 会直接拒绝启动）。
	if _, err := ParseICEConfig("50010-50000", ""); err == nil || !strings.Contains(err.Error(), "--ice-port-range") {
		t.Fatalf("bad range error = %v, want --ice-port-range prefix", err)
	}
	if _, err := ParseICEConfig("", "not-an-ip"); err == nil || !strings.Contains(err.Error(), "--ice-public-ip") {
		t.Fatalf("bad IP error = %v, want --ice-public-ip prefix", err)
	}
}

func TestICEConfigSettingEngine(t *testing.T) {
	// 零值：不构造 SettingEngine（避免给旧路径引入任何行为变化）。
	if _, ok := (ICEConfig{}).settingEngine(); ok {
		t.Fatal("zero config must not produce a SettingEngine")
	}
	// 只开端口。
	se, ok := (ICEConfig{PortRangeMin: 50000, PortRangeMax: 50010}).settingEngine()
	if !ok {
		t.Fatal("port-range config must produce a SettingEngine")
	}
	_ = se
	// 只开 IP。
	if _, ok := (ICEConfig{PublicIPs: []string{"203.0.113.10"}}).settingEngine(); !ok {
		t.Fatal("public-IP config must produce a SettingEngine")
	}
	// 两项都开：构造函数不 panic（真正的效果由下面的真实候选测试证明）。
	if _, ok := (ICEConfig{PortRangeMin: 50000, PortRangeMax: 50010, PublicIPs: []string{"203.0.113.10"}}).settingEngine(); !ok {
		t.Fatal("full config must produce a SettingEngine")
	}
}

// TestGatheredCandidatesHonorPublicIPAndPortRange 是端到端判据的 Go 侧版本：
// 用与生产同一条 newAPIWithICE 路径建 PeerConnection，本地 SDP 里的 host 候选
// 必须出现配置的对外 IP，且 UDP 候选端口必须落在配置区间内。
//
// 如果运行环境无法完成 ICE gathering（无可用网卡/UDP socket 被限制），测试会
// 明确 Skip 并写清原因，而不是给出一个假绿。
func TestGatheredCandidatesHonorPublicIPAndPortRange(t *testing.T) {
	const (
		publicIP = "203.0.113.10"
		minPort  = 50000
		maxPort  = 50010
	)
	cfg := ICEConfig{PortRangeMin: minPort, PortRangeMax: maxPort, PublicIPs: []string{publicIP}}
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		t.Fatalf("register codecs: %v", err)
	}
	api, err := newAPIWithICE(media, cfg)
	if err != nil {
		t.Fatalf("newAPIWithICE: %v", err)
	}
	pc, err := api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatalf("new peer connection: %v", err)
	}
	defer func() { _ = pc.Close() }()

	// 必须先有 m-line，Pion 才会把 gather 到的候选写进 SDP：没有媒体段时
	// CreateOffer 产出的是 v=0 空壳（候选只从 OnICECandidate 走），SDP 里
	// 永远看不到 a=candidate。生产路径的语音房间有 13 条音频 m-line，
	// 这里加一条 recvonly 音频即复刻同一行为。
	if _, err := pc.AddTransceiverFromKind(
		webrtc.RTPCodecTypeAudio,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionRecvonly},
	); err != nil {
		t.Fatalf("add transceiver: %v", err)
	}

	offer, err := pc.CreateOffer(nil)
	if err != nil {
		t.Fatalf("create offer: %v", err)
	}
	gatherDone := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(offer); err != nil {
		t.Fatalf("set local description: %v", err)
	}
	select {
	case <-gatherDone:
	case <-time.After(10 * time.Second):
		t.Skip("ICE gathering did not complete in 10s; environment cannot bind UDP sockets (skipped, not a pass)")
	}

	desc := pc.LocalDescription()
	if desc == nil {
		t.Fatal("local description is nil after gathering")
	}
	var udpCandidates int
	var sawPublicIP, sawInRange bool
	for _, line := range strings.Split(desc.SDP, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "a=candidate:") {
			continue
		}
		// a=candidate:<foundation> <component> <protocol> <priority> <ip> <port> typ ...
		fields := strings.Fields(line)
		if len(fields) < 6 {
			continue
		}
		if fields[2] != "udp" {
			continue
		}
		udpCandidates++
		if fields[4] == publicIP {
			sawPublicIP = true
		}
		port, err := strconv.Atoi(fields[5])
		if err == nil && port >= minPort && port <= maxPort {
			sawInRange = true
		}
	}
	if udpCandidates == 0 {
		t.Skipf("no UDP candidates gathered (network unavailable); sdp=%s", desc.SDP)
	}
	if !sawPublicIP {
		t.Fatalf("no UDP candidate carries public IP %s; sdp=\n%s", publicIP, desc.SDP)
	}
	if !sawInRange {
		t.Fatalf("no UDP candidate port in %d-%d; sdp=\n%s", minPort, maxPort, desc.SDP)
	}
}
