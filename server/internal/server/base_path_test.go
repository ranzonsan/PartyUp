package server_test

// REG-03 前置验证：反代 base path 不改变服务端根路由。
//
// 生产路由仍是根上的 /api/... 与 /ws；这里用标准 http.StripPrefix 模拟
// 「外部 /partyup/ 前缀 → 服务端根」的反向代理，证明同一份 Handler 在前缀后
// 仍能真实处理 HTTP 预检/注册与 WebSocket 鉴权。root 部署同时回归。

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gorilla/websocket"

	"partyup/internal/protocol"
)

func postJSONAt(t *testing.T, rawURL string, body any) (int, []byte) {
	t.Helper()
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatalf("marshal request body: %v", err)
	}
	resp, err := http.Post(rawURL, "application/json", bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("POST %s: %v", rawURL, err)
	}
	defer func() { _ = resp.Body.Close() }()
	payload, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read response body: %v", err)
	}
	return resp.StatusCode, payload
}

// TestPrefixedReverseProxyHTTPKeepsBasePath 用真实 HTTP 请求走
// /partyup/api/invite/validate 与 /partyup/api/invite/{code}，并回归根路径。
func TestPrefixedReverseProxyHTTPKeepsBasePath(t *testing.T) {
	env := newTestEnv(t)
	_, adminToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()

	// 反代模拟：外部 /partyup 前缀在转发时被标准 StripPrefix 去掉。
	proxied := httptest.NewServer(http.StripPrefix("/partyup", env.srv.Handler()))
	t.Cleanup(proxied.Close)

	// 1) 带前缀的预检必须命中真实 handler，而不是 404。
	status, body := postJSONAt(t, proxied.URL+"/partyup/api/invite/validate", map[string]string{"code": code})
	if status != http.StatusOK {
		t.Fatalf("prefixed validate: status=%d body=%s, want 200", status, body)
	}
	var valid struct {
		Valid bool `json:"valid"`
	}
	if err := json.Unmarshal(body, &valid); err != nil || !valid.Valid {
		t.Fatalf("prefixed validate body = %s (err=%v), want {\"valid\":true}", body, err)
	}

	// 2) 带前缀的正式注册也能走通（预检与注册是两条真实路由）。
	status, body = postJSONAt(t, proxied.URL+"/partyup/api/invite/"+code,
		map[string]string{"username": "bob", "password": "secret456"})
	if status != http.StatusCreated {
		t.Fatalf("prefixed register: status=%d body=%s, want 201", status, body)
	}
	var session sessionResponse
	if err := json.Unmarshal(body, &session); err != nil || session.Token == "" {
		t.Fatalf("prefixed register body = %s (err=%v), want session token", body, err)
	}

	// 3) 带前缀的认证 HTTP 路由（GET /api/me）保持可用。
	req, err := http.NewRequest(http.MethodGet, proxied.URL+"/partyup/api/me", nil)
	if err != nil {
		t.Fatalf("build GET request: %v", err)
	}
	req.Header.Set("Authorization", "Bearer "+adminToken)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("prefixed GET /api/me: %v", err)
	}
	meBody, err := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if err != nil {
		t.Fatalf("read /api/me body: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("prefixed /api/me: status=%d body=%s, want 200", resp.StatusCode, meBody)
	}

	// 4) root 部署不回归：同一服务端根路径仍可直接访问。
	rootCode := env.newInvite()
	status, body = env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]string{"code": rootCode})
	if status != http.StatusOK {
		t.Fatalf("root validate: status=%d body=%s, want 200", status, body)
	}
}

// TestPrefixedReverseProxyWebSocketKeepsBasePath 用标准 StripPrefix 让
// ws://host/partyup/ws 落到服务端 /ws，完成真实 auth 握手与一次请求；
// 同时回归 root 的 /ws。
func TestPrefixedReverseProxyWebSocketKeepsBasePath(t *testing.T) {
	env := newTestEnv(t)
	_, token := env.bootstrapAdmin("alice", "secret123")

	proxied := httptest.NewServer(http.StripPrefix("/partyup", env.srv.Handler()))
	t.Cleanup(proxied.Close)

	prefixed := dialWS(t, proxied.URL+"/partyup", token)
	if got := prefixed.request(protocol.TypeChannelList, struct{}{}); got.Type != protocol.TypeOK(protocol.TypeChannelList) {
		t.Fatalf("prefixed ws channel.list: type=%s payload=%s", got.Type, got.Payload)
	}

	// root 部署保持原路径。
	root := dialWS(t, env.http.URL, token)
	if got := root.request(protocol.TypeChannelList, struct{}{}); got.Type != protocol.TypeOK(protocol.TypeChannelList) {
		t.Fatalf("root ws channel.list: type=%s payload=%s", got.Type, got.Payload)
	}

	// 防呆：前缀后的错误路径不应被误当成 /ws。
	badURL := "ws" + strings.TrimPrefix(proxied.URL, "http") + "/partyup/ws/extra"
	if conn, resp, err := websocket.DefaultDialer.Dial(badURL, nil); err == nil {
		_ = conn.Close()
		t.Fatalf("dial %s unexpectedly succeeded", badURL)
	} else if resp != nil && resp.StatusCode == http.StatusSwitchingProtocols {
		t.Fatalf("dial %s got 101, want rejection", badURL)
	}
}
