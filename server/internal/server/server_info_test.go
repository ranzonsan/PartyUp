package server_test

import (
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/protocol"
	"partyup/internal/server"
)

// TestServerInfoPublishesStunURLs 验收 server.info（PROTOCOL.md §1.4）：
// 服务端把内建 STUN responder 的地址下发给客户端，客户端据此配置 ICE servers。
func TestServerInfoPublishesStunURLs(t *testing.T) {
	env := newTestEnvWithOptions(t, server.Options{StunURLs: []string{"stun:192.168.1.10:3478"}})
	_, adminToken := env.bootstrapAdmin("admin", "hunter2hunter2")

	c := dialWS(t, env.http.URL, adminToken)
	defer func() { _ = c.conn.Close() }()

	env2 := c.request(protocol.TypeServerInfo, nil)
	if env2.Type != protocol.TypeOK(protocol.TypeServerInfo) {
		t.Fatalf("server.info response type = %q, want server.info.ok", env2.Type)
	}
	var out protocol.ServerInfoOK
	if err := env2.DecodePayload(&out); err != nil {
		t.Fatalf("decode server.info.ok: %v", err)
	}
	if len(out.StunURLs) != 1 || out.StunURLs[0] != "stun:192.168.1.10:3478" {
		t.Fatalf("stun_urls = %v, want [stun:192.168.1.10:3478]", out.StunURLs)
	}
}

// TestServerInfoEmptyWhenStunDisabled：没开 STUN 时下发空数组（不是 null），
// 客户端仍可直连，只是没有 srflx 候选。
func TestServerInfoEmptyWhenStunDisabled(t *testing.T) {
	env := newTestEnv(t)
	_, adminToken := env.bootstrapAdmin("admin", "hunter2hunter2")

	c := dialWS(t, env.http.URL, adminToken)
	defer func() { _ = c.conn.Close() }()

	resp := c.request(protocol.TypeServerInfo, nil)
	var out protocol.ServerInfoOK
	if err := resp.DecodePayload(&out); err != nil {
		t.Fatalf("decode server.info.ok: %v", err)
	}
	if out.StunURLs == nil {
		t.Fatal("stun_urls is null, want an empty array")
	}
	if len(out.StunURLs) != 0 {
		t.Fatalf("stun_urls = %v, want empty", out.StunURLs)
	}
	// 原始 JSON 也必须是 []，不是 null——第三方客户端不该需要区分两者。
	var raw struct {
		StunURLs []string `json:"stun_urls"`
	}
	if err := resp.DecodePayload(&raw); err != nil {
		t.Fatalf("decode again: %v", err)
	}
	if raw.StunURLs == nil {
		t.Fatal("decoded stun_urls is nil")
	}
}

// TestServerInfoRequiresAuth：server.info 是鉴权后的消息，未鉴权必须被拒。
func TestServerInfoRequiresAuth(t *testing.T) {
	env := newTestEnv(t)
	wsURL := "ws" + strings.TrimPrefix(env.http.URL, "http") + "/ws"
	raw, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial ws: %v", err)
	}
	defer func() { _ = raw.Close() }()
	conn := &wsClient{t: t, conn: raw}

	req, err := protocol.New(protocol.TypeServerInfo, nil)
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	conn.sendRaw(req)
	resp, err := conn.recv(time.Now().Add(5 * time.Second))
	if err != nil {
		t.Fatalf("recv: %v", err)
	}
	if resp.Type != protocol.TypeError(protocol.TypeServerInfo) {
		t.Fatalf("response type = %q, want server.info.err", resp.Type)
	}
	var e protocol.ErrorPayload
	if err := resp.DecodePayload(&e); err != nil {
		t.Fatalf("decode error payload: %v", err)
	}
	if e.Code != protocol.CodeUnauthenticated {
		t.Fatalf("error code = %q, want %q", e.Code, protocol.CodeUnauthenticated)
	}
}
