package stunserver

import (
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/pion/stun/v4"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// TestBindingRequestReturnsMappedAddress 是本包的核心验收：客户端发一条 Binding Request，
// 必须收到 Binding Success，且 XOR-MAPPED-ADDRESS 等于客户端自己的 UDP 源地址
// （这正是 libwebrtc 用来生成 srflx 候选的信息）。
func TestBindingRequestReturnsMappedAddress(t *testing.T) {
	srv, err := Start("127.0.0.1:0", "127.0.0.1:0", quietLogger())
	if err != nil {
		t.Fatalf("start stun: %v", err)
	}
	defer func() { _ = srv.Close() }()

	if got, want := srv.URL(), "stun:127.0.0.1:"+portOf(t, srv.Addr()); got != want {
		t.Fatalf("URL() = %q, want %q", got, want)
	}

	conn, err := net.Dial("udp", srv.Addr().String())
	if err != nil {
		t.Fatalf("dial stun: %v", err)
	}
	defer func() { _ = conn.Close() }()

	req, err := stun.Build(stun.TransactionID, stun.BindingRequest)
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	if _, err := conn.Write(req.Raw); err != nil {
		t.Fatalf("write request: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	buf := make([]byte, 1500)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	resp := &stun.Message{Raw: append([]byte(nil), buf[:n]...)}
	if err := resp.Decode(); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if resp.Type != stun.BindingSuccess {
		t.Fatalf("response type = %v, want Binding Success", resp.Type)
	}
	if resp.TransactionID != req.TransactionID {
		t.Fatalf("transaction id mismatch: got %v want %v", resp.TransactionID, req.TransactionID)
	}
	var mapped stun.XORMappedAddress
	if err := mapped.GetFrom(resp); err != nil {
		t.Fatalf("read XOR-MAPPED-ADDRESS: %v", err)
	}
	local := conn.LocalAddr().(*net.UDPAddr)
	if mapped.Port != local.Port {
		t.Fatalf("mapped port = %d, want %d (client's own source port)", mapped.Port, local.Port)
	}
	if !mapped.IP.Equal(local.IP) {
		t.Fatalf("mapped ip = %v, want %v", mapped.IP, local.IP)
	}
}

// TestStartDisabled 覆盖「可关」：空串 / off / none 都不监听，也不产出 URL。
func TestStartDisabled(t *testing.T) {
	for _, listen := range []string{"", "off", "none", "disabled"} {
		srv, err := Start(listen, "", quietLogger())
		if err != nil {
			t.Fatalf("Start(%q): %v", listen, err)
		}
		if srv != nil {
			t.Fatalf("Start(%q) returned a server, want nil", listen)
		}
		var nilSrv *Server
		if url := nilSrv.URL(); url != "" {
			t.Fatalf("nil server URL = %q, want empty", url)
		}
	}
}

// TestResolveAdvertise 覆盖通告地址的推导规则（多网卡 VPS 必须显式传 --stun-advertise）。
func TestResolveAdvertise(t *testing.T) {
	cases := []struct {
		name      string
		listen    string
		advertise string
		want      string
	}{
		{"explicit ip:port", ":3478", "203.0.113.7:3478", "203.0.113.7:3478"},
		{"explicit ip only reuses listen port", "0.0.0.0:3478", "203.0.113.7", "203.0.113.7:3478"},
		{"explicit ipv6", ":3478", "[2001:db8::1]:3478", "[2001:db8::1]:3478"},
		{"concrete listen host", "192.168.1.10:3478", "", "192.168.1.10:3478"},
		{"wildcard listen derives host", ":3478", "", ""}, // 只断言非空，见下
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, _ := resolveAdvertise(tc.listen, tc.advertise, nil)
			if tc.want != "" && got != tc.want {
				t.Fatalf("resolveAdvertise(%q,%q) = %q, want %q", tc.listen, tc.advertise, got, tc.want)
			}
			if tc.want == "" {
				if got == "" {
					t.Fatalf("resolveAdvertise(%q,%q) = empty, want derived host:port", tc.listen, tc.advertise)
				}
				if _, _, err := net.SplitHostPort(got); err != nil {
					t.Fatalf("derived advertise %q is not host:port: %v", got, err)
				}
			}
		})
	}
}

func portOf(t *testing.T, addr net.Addr) string {
	t.Helper()
	if addr == nil {
		t.Fatal("nil addr")
	}
	_, port, err := net.SplitHostPort(addr.String())
	if err != nil {
		t.Fatalf("split %q: %v", addr.String(), err)
	}
	return port
}
