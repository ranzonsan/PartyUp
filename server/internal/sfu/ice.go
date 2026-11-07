package sfu

// 媒体 ICE 的端口范围与对外 IP 映射（2026-11-08，task-6）。
//
// 为什么需要（用户原话）：「这个不能给容器内进程传递一个宿主机 IP（对 VPS 那种
// 就是公网 IP）来纠正吗？」
//
// 能。Pion 的 SettingEngine 有两个开关：
//   - SetEphemeralUDPPortRange(min, max)：ICE 的 UDP socket 只从固定区间挑端口，
//     部署时可用 `docker run -p 50000-50010:50000-50010/udp` 精确映射；
//   - SetNAT1To1IPs(ips, ICECandidateTypeHost)：把 host 候选里的内网/容器地址
//     改写成对外 IP（VPS 填公网 IP；家用 NAT 填公网 IP 并做端口转发）。
//
// 两者配合后，Docker bridge 网络不再需要 `--network host` 才能跑媒体；
// 代价是部署者必须显式告诉进程「对外地址是什么」——进程无法可靠地自己猜出来
// （多网卡、NAT、容器网络各层都不同），所以本文件只做严格校验，不玩自动推断。

import (
	"fmt"
	"net"
	"strconv"
	"strings"

	"github.com/pion/webrtc/v4"
)

// maxICEPortSpan 是允许的最大端口区间跨度（含两端）。手滑填 1-65535 会让
// 「固定端口」失去意义，也等于把整个 UDP 空间暴露成候选池，因此直接拒绝。
const maxICEPortSpan = 4096

// ICEConfig 是 SFU 建 PeerConnection 时注入给 Pion 的媒体配置。
//
// 零值 = 不限制端口、不改写 IP（与 2026-11-07 之前的行为完全一致，升级无感）。
type ICEConfig struct {
	// PortRangeMin / PortRangeMax 是 ICE UDP 端口区间（含两端）。两者都为 0 表示不限制。
	PortRangeMin uint16
	PortRangeMax uint16
	// PublicIPs 是 host 候选要改写成对外 IP 列表（按顺序去重）。
	// 空表示不改写（容器/多网卡部署必须显式给）。
	PublicIPs []string
}

// PortRangeSet 报告是否设置了固定端口区间。
func (c ICEConfig) PortRangeSet() bool { return c.PortRangeMin != 0 && c.PortRangeMax != 0 }

// NAT1To1Set 报告是否需要改写 host 候选 IP。
func (c ICEConfig) NAT1To1Set() bool { return len(c.PublicIPs) > 0 }

// Enabled 报告是否有任何需要注入 SettingEngine 的配置。
func (c ICEConfig) Enabled() bool { return c.PortRangeSet() || c.NAT1To1Set() }

// PortRangeString 返回可打印的端口区间；未设置时为 "unset"。
func (c ICEConfig) PortRangeString() string {
	if !c.PortRangeSet() {
		return "unset"
	}
	return fmt.Sprintf("%d-%d", c.PortRangeMin, c.PortRangeMax)
}

// PublicIPsString 返回逗号分隔的对外 IP；未设置时为 "unset"。
func (c ICEConfig) PublicIPsString() string {
	if !c.NAT1To1Set() {
		return "unset"
	}
	return strings.Join(c.PublicIPs, ",")
}

// settingEngine 按配置构造 Pion SettingEngine。第二个返回值表示是否真的需要它。
func (c ICEConfig) settingEngine() (webrtc.SettingEngine, bool) {
	if !c.Enabled() {
		return webrtc.SettingEngine{}, false
	}
	var se webrtc.SettingEngine
	if c.PortRangeSet() {
		se.SetEphemeralUDPPortRange(c.PortRangeMin, c.PortRangeMax)
	}
	if c.NAT1To1Set() {
		// ICECandidateTypeHost：只改写 host 候选。srflx 候选是服务端自己的 STUN
		// 反射结果，由 STUN 决定，不应该被 NAT1To1 二次改写。
		se.SetNAT1To1IPs(c.PublicIPs, webrtc.ICECandidateTypeHost)
	}
	return se, true
}

// newAPIWithICE 构造注册了默认编解码的 webrtc.API，并按需注入 SettingEngine。
// 语音房间与视频房间都必须走这个函数，否则两条媒体路径里会有一条不生效。
func newAPIWithICE(media *webrtc.MediaEngine, ice ICEConfig) (*webrtc.API, error) {
	se, enabled := ice.settingEngine()
	if !enabled {
		return webrtc.NewAPI(webrtc.WithMediaEngine(media)), nil
	}
	return webrtc.NewAPI(webrtc.WithMediaEngine(media), webrtc.WithSettingEngine(se)), nil
}

// ParseICEPortRange 解析 `MIN-MAX` 或单端口 `PORT`（等价 MIN=MAX）。
//
// 严格校验（不 panic、不静默忽略）：非空、只允许一个 '-' 或没有、十进制、
// 1–65535、MIN ≤ MAX、跨度 ≤ maxICEPortSpan。
func ParseICEPortRange(spec string) (minPort, maxPort uint16, err error) {
	s := strings.TrimSpace(spec)
	if s == "" {
		return 0, 0, fmt.Errorf("empty ICE port range")
	}
	parts := strings.Split(s, "-")
	if len(parts) > 2 {
		return 0, 0, fmt.Errorf("invalid ICE port range %q: want MIN-MAX or PORT", spec)
	}
	parse := func(p string) (uint16, error) {
		p = strings.TrimSpace(p)
		if p == "" {
			return 0, fmt.Errorf("invalid ICE port range %q: empty endpoint", spec)
		}
		v, err := strconv.ParseUint(p, 10, 16)
		if err != nil {
			return 0, fmt.Errorf("invalid ICE port range %q: %q is not a port number 1-65535", spec, p)
		}
		if v == 0 {
			return 0, fmt.Errorf("invalid ICE port range %q: port must be 1-65535", spec)
		}
		return uint16(v), nil
	}
	lo, err := parse(parts[0])
	if err != nil {
		return 0, 0, err
	}
	hi := lo
	if len(parts) == 2 {
		hi, err = parse(parts[1])
		if err != nil {
			return 0, 0, err
		}
	}
	if lo > hi {
		return 0, 0, fmt.Errorf("invalid ICE port range %q: MIN (%d) must be <= MAX (%d)", spec, lo, hi)
	}
	if int(hi)-int(lo)+1 > maxICEPortSpan {
		return 0, 0, fmt.Errorf("invalid ICE port range %q: span %d exceeds limit %d", spec, int(hi)-int(lo)+1, maxICEPortSpan)
	}
	return lo, hi, nil
}

// ParseICEPublicIPs 解析逗号分隔的对外 IP 列表。
//
// 严格校验：每个元素必须能被 net.ParseIP 解析；拒绝通配地址（0.0.0.0 / ::），
// 因为把它作为 host 候选的对外地址一定是配置错误（客户端连不上），静默接受只会
// 变成线上「偶发连不上」的谜题。允许回环地址（本机验收会传 127.0.0.1）。
func ParseICEPublicIPs(spec string) ([]string, error) {
	s := strings.TrimSpace(spec)
	if s == "" {
		return nil, nil
	}
	raw := strings.Split(s, ",")
	out := make([]string, 0, len(raw))
	seen := make(map[string]struct{}, len(raw))
	for _, item := range raw {
		item = strings.TrimSpace(item)
		if item == "" {
			return nil, fmt.Errorf("invalid ICE public IP list %q: empty entry", spec)
		}
		ip := net.ParseIP(item)
		if ip == nil {
			return nil, fmt.Errorf("invalid ICE public IP %q: not an IP address", item)
		}
		if ip.IsUnspecified() {
			return nil, fmt.Errorf("invalid ICE public IP %q: wildcard address is not usable as a candidate", item)
		}
		canonical := ip.String()
		if _, dup := seen[canonical]; dup {
			continue
		}
		seen[canonical] = struct{}{}
		out = append(out, canonical)
	}
	return out, nil
}

// ParseICEConfig 是 CLI/环境变量到 ICEConfig 的唯一入口。
//
// 空字符串表示「该能力未启用」；非空则严格解析，任何非法值都返回可读错误，
// 由 main 在启动前拒绝（不带着半截配置跑起来）。
func ParseICEConfig(portRange, publicIPs string) (ICEConfig, error) {
	var cfg ICEConfig
	if strings.TrimSpace(portRange) != "" {
		lo, hi, err := ParseICEPortRange(portRange)
		if err != nil {
			return ICEConfig{}, fmt.Errorf("--ice-port-range: %w", err)
		}
		cfg.PortRangeMin, cfg.PortRangeMax = lo, hi
	}
	ips, err := ParseICEPublicIPs(publicIPs)
	if err != nil {
		return ICEConfig{}, fmt.Errorf("--ice-public-ip: %w", err)
	}
	cfg.PublicIPs = ips
	return cfg, nil
}
