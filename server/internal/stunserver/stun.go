// Package stunserver 是 PartyUp 自带的 STUN responder（RFC 5389 Binding）。
//
// 为什么自己带一个：直连（direct）模式下两端要穿过 NAT，需要先拿到自己的
// server-reflexive 候选（srflx）。自托管场景没有公共 STUN 可用——依赖公共 STUN
// 会把客户端 IP 泄露给第三方、国内网络还不一定通。所以 STUN 跟着 PartyUp 服务端
// 一起发，由运维配置 `--stun-listen` / `--stun-advertise`。
//
// 协议实现直接用 pion/stun（**不自己写 STUN 编解码**）；本包只负责 UDP 收发、
// 应答 Binding Request，以及把「对外通告地址」算出来供 server.info 下发。
//
// STUN 只用于候选收集，不承载媒体：它回一个 XOR-MAPPED-ADDRESS 之后就没有别的职责。
package stunserver

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"

	"github.com/pion/stun/v4"
)

// Server 是一个 UDP STUN responder。
type Server struct {
	conn      net.PacketConn
	log       *slog.Logger
	advertise string // host:port，用于 server.info 与启动日志

	closeOnce sync.Once
	done      chan struct{}
	wg        sync.WaitGroup
}

// Addr 返回实际监听的 UDP 地址（测试与运维排查用）。未启动时返回 nil。
func (s *Server) Addr() net.Addr {
	if s == nil || s.conn == nil {
		return nil
	}
	return s.conn.LocalAddr()
}

// Advertise 返回对外通告的 host:port（不含 stun: 前缀）。未启动时为空串。
func (s *Server) Advertise() string {
	if s == nil {
		return ""
	}
	return s.advertise
}

// URL 返回下发给客户端的 STUN URL（`stun:host:port`）。未启动时为空串。
func (s *Server) URL() string {
	if adv := s.Advertise(); adv != "" {
		return "stun:" + adv
	}
	return ""
}

// Start 在 listen 上监听 UDP，并把 advertise（host:port）作为对外通告地址。
//
//   - listen 为空 / "off" / "none" 表示关闭 STUN，返回 (nil, nil)；
//   - advertise 为空时从 listen 推导：监听地址是具体 IP 就用它，否则取本机第一个
//     非回环 IPv4（多网卡 VPS 必须显式传 --stun-advertise，否则会通告内网地址）。
func Start(listen, advertise string, log *slog.Logger) (*Server, error) {
	if log == nil {
		log = slog.Default()
	}
	if disabled(listen) {
		log.Info("stun disabled", "reason", "listen empty/off")
		return nil, nil
	}
	pc, err := net.ListenPacket("udp", listen)
	if err != nil {
		return nil, fmt.Errorf("stun listen %s: %w", listen, err)
	}
	adv, warn := resolveAdvertise(listen, advertise, pc.LocalAddr())
	// 显式 advertise 里写了端口 0（常见于测试/临时端口）时替换成真实绑定端口。
	if h, p, err := net.SplitHostPort(adv); err == nil && p == "0" {
		if la, ok := pc.LocalAddr().(*net.UDPAddr); ok {
			adv = net.JoinHostPort(h, strconv.Itoa(la.Port))
		}
	}
	if warn != "" {
		log.Warn("stun advertise derived automatically", "advertise", adv, "hint", warn)
	}
	s := &Server{conn: pc, log: log, advertise: adv, done: make(chan struct{})}
	s.wg.Add(1)
	go s.serve()
	log.Info("stun listening", "listen", pc.LocalAddr().String(), "advertise", adv)
	return s, nil
}

// Close 停止 responder 并释放 UDP 端口。
func (s *Server) Close() error {
	if s == nil {
		return nil
	}
	var err error
	s.closeOnce.Do(func() {
		close(s.done)
		err = s.conn.Close()
		s.wg.Wait()
	})
	return err
}

func (s *Server) serve() {
	defer s.wg.Done()
	buf := make([]byte, 1500)
	for {
		n, addr, err := s.conn.ReadFrom(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			s.log.Warn("stun read", "err", err)
			continue
		}
		s.handle(buf[:n], addr)
	}
}

// handle 处理一条 UDP 报文：只应答 Binding Request，其它类型（含 indication）忽略。
func (s *Server) handle(raw []byte, addr net.Addr) {
	msg := &stun.Message{Raw: append([]byte(nil), raw...)}
	if err := msg.Decode(); err != nil {
		return // 不是 STUN，静默丢弃（公网 UDP 端口会收到扫描流量）
	}
	if msg.Type != stun.BindingRequest {
		return
	}
	udpAddr, ok := addr.(*net.UDPAddr)
	if !ok {
		return
	}
	// 应答：XOR-MAPPED-ADDRESS = 这条请求的源地址（这就是客户端要的 srflx 候选）。
	resp, err := stun.Build(
		stun.TransactionID,
		stun.NewTransactionIDSetter(msg.TransactionID),
		stun.BindingSuccess,
		&stun.XORMappedAddress{IP: udpAddr.IP, Port: udpAddr.Port},
		stun.Fingerprint,
	)
	if err != nil {
		s.log.Warn("stun build response", "err", err)
		return
	}
	if _, err := s.conn.WriteTo(resp.Raw, addr); err != nil {
		s.log.Debug("stun write response", "err", err, "addr", addr.String())
	}
}

// disabled 判断监听配置是否表示「关闭 STUN」。
func disabled(listen string) bool {
	switch strings.ToLower(strings.TrimSpace(listen)) {
	case "", "off", "none", "disabled", "false", "0":
		return true
	}
	return false
}

// resolveAdvertise 决定对外通告的 host:port，并返回一条给运维的提示（非空表示
// 地址是自动推导出来的，多网卡/有 NAT 的机器应当显式指定）。
func resolveAdvertise(listen, advertise string, local net.Addr) (string, string) {
	port := listenPort(listen)
	if port == "" && local != nil {
		port = strconv.Itoa(local.(*net.UDPAddr).Port)
	}
	if advertise = strings.TrimSpace(advertise); advertise != "" {
		// 允许只写 IP（沿用 listen 的端口），也允许写完整 IP:port / [v6]:port。
		if _, _, err := net.SplitHostPort(advertise); err != nil {
			advertise = net.JoinHostPort(strings.Trim(advertise, "[]"), port)
		}
		return advertise, ""
	}
	// 未显式指定：优先用监听地址里的具体 IP。
	if h, p, err := net.SplitHostPort(listen); err == nil && h != "" && !unspecified(h) {
		return net.JoinHostPort(h, p), ""
	}
	// 监听在 0.0.0.0/[::]：退到本机第一个非回环 IPv4。
	if ip := firstNonLoopbackIPv4(); ip != "" {
		return net.JoinHostPort(ip, port), "multi-homed or behind NAT hosts must pass --stun-advertise"
	}
	return net.JoinHostPort("127.0.0.1", port), "no non-loopback IPv4 found; pass --stun-advertise"
}

func listenPort(listen string) string {
	if _, p, err := net.SplitHostPort(listen); err == nil {
		return p
	}
	return ""
}

func unspecified(host string) bool {
	ip := net.ParseIP(strings.Trim(host, "[]"))
	return ip != nil && (ip.IsUnspecified())
}

func firstNonLoopbackIPv4() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return ""
	}
	for _, a := range addrs {
		ipnet, ok := a.(*net.IPNet)
		if !ok || ipnet.IP.IsLoopback() {
			continue
		}
		if v4 := ipnet.IP.To4(); v4 != nil {
			return v4.String()
		}
	}
	return ""
}
