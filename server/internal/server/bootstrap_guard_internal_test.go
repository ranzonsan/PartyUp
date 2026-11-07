package server

// bootstrap 安全边界（规格 §7.4）的内部回归：回环/ TLS / --trust-proxy 的信任判定，
// 以及 server-info 的 bootstrap_allowed 必须反映「这条连接」而不是全局状态。
// 用直接构造的 http.Request 覆盖 RemoteAddr/TLS，因为 httptest 客户端只能从回环来。

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"partyup/internal/protocol"
	"partyup/internal/store"
)

func newGuardServer(t *testing.T, trustProxy bool) (*Server, string) {
	t.Helper()
	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { _ = st.Close() })
	srv := New(st, Options{Name: "guard", Version: "test", TrustProxy: trustProxy})
	t.Cleanup(func() { srv.Hub().Close() })
	info, err := srv.EnsureFirstRun(context.Background())
	if err != nil {
		t.Fatalf("ensure first run: %v", err)
	}
	return srv, info.Token
}

func bootstrapReq(token string) *http.Request {
	body, _ := json.Marshal(map[string]string{
		"token": token, "username": "alice", "password": "secret123",
	})
	req := httptest.NewRequest(http.MethodPost, "/api/bootstrap", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	return req
}

// TestBootstrapRejectsNonLoopbackPlaintext：默认配置下，非回环明文连接必须 403。
func TestBootstrapRejectsNonLoopbackPlaintext(t *testing.T) {
	srv, token := newGuardServer(t, false)

	req := bootstrapReq(token)
	req.RemoteAddr = "203.0.113.9:4444" // 文档保留地址，非回环
	rec := httptest.NewRecorder()
	srv.handleBootstrap(rec, req)

	if rec.Code != http.StatusForbidden {
		t.Fatalf("non-loopback plaintext bootstrap status = %d, want 403 (body=%s)", rec.Code, rec.Body.String())
	}
	var e protocol.ErrorPayload
	if err := json.Unmarshal(rec.Body.Bytes(), &e); err != nil || e.Code != protocol.CodeForbidden {
		t.Fatalf("bootstrap code = %q, want %q", e.Code, protocol.CodeForbidden)
	}
}

// TestBootstrapAllowsLoopback：回环连接 + 正确 token → 201。
func TestBootstrapAllowsLoopback(t *testing.T) {
	srv, token := newGuardServer(t, false)
	req := bootstrapReq(token)
	req.RemoteAddr = "127.0.0.1:5555"
	rec := httptest.NewRecorder()
	srv.handleBootstrap(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("loopback bootstrap status = %d, want 201 (body=%s)", rec.Code, rec.Body.String())
	}
}

// TestBootstrapAllowsTLS：TLS 连接即使非回环也可信；错误 token 得到 401（说明通过了连接检查）。
func TestBootstrapAllowsTLS(t *testing.T) {
	srv, _ := newGuardServer(t, false)
	req := bootstrapReq("wrong-token")
	req.RemoteAddr = "203.0.113.9:4444"
	req.TLS = &tls.ConnectionState{}
	rec := httptest.NewRecorder()
	srv.handleBootstrap(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("TLS bootstrap with bad token status = %d, want 401 (body=%s)", rec.Code, rec.Body.String())
	}
}

// TestBootstrapAllowsTrustProxy：显式 --trust-proxy 后，代理连接（明文但带 XFF）可信。
func TestBootstrapAllowsTrustProxy(t *testing.T) {
	srv, _ := newGuardServer(t, true)
	req := bootstrapReq("wrong-token")
	req.RemoteAddr = "127.0.0.1:5555"
	req.Header.Set("X-Forwarded-For", "203.0.113.9")
	rec := httptest.NewRecorder()
	srv.handleBootstrap(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("trust-proxy bootstrap with bad token status = %d, want 401 (body=%s)", rec.Code, rec.Body.String())
	}
}

// TestServerInfoBootstrapAllowedReflectsConnection：server-info 的 bootstrap_allowed
// 必须由「本连接是否可信 + 是否已初始化」共同决定。
func TestServerInfoBootstrapAllowedReflectsConnection(t *testing.T) {
	srv, _ := newGuardServer(t, false)

	// 未初始化 + 回环 → true。
	loop := httptest.NewRequest(http.MethodGet, "/api/server-info", nil)
	loop.RemoteAddr = "127.0.0.1:1234"
	rec := httptest.NewRecorder()
	srv.handleServerInfo(rec, loop)
	var info protocol.ServerInfoPublic
	if err := json.Unmarshal(rec.Body.Bytes(), &info); err != nil {
		t.Fatalf("decode server-info: %v", err)
	}
	// newGuardServer 的 EnsureFirstRun 不建成员，所以 initialized=false。
	if info.Initialized {
		t.Fatalf("initialized = true on fresh store")
	}
	if !info.BootstrapAllowed {
		t.Fatal("loopback uninitialized server: bootstrap_allowed = false, want true")
	}

	// 未初始化 + 非回环明文 → false。
	remote := httptest.NewRequest(http.MethodGet, "/api/server-info", nil)
	remote.RemoteAddr = "203.0.113.9:4444"
	rec = httptest.NewRecorder()
	srv.handleServerInfo(rec, remote)
	if err := json.Unmarshal(rec.Body.Bytes(), &info); err != nil {
		t.Fatalf("decode server-info: %v", err)
	}
	if info.BootstrapAllowed {
		t.Fatal("non-loopback plaintext: bootstrap_allowed = true, want false")
	}
}

// TestBootstrapRejectsWhenInitialized 覆盖「已初始化」的 409 语义（ALREADY_BOOTSTRAPPED）。
func TestBootstrapRejectsWhenInitialized(t *testing.T) {
	srv, token := newGuardServer(t, false)
	req := bootstrapReq(token)
	req.RemoteAddr = "127.0.0.1:5555"
	rec := httptest.NewRecorder()
	srv.handleBootstrap(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("first bootstrap status = %d, want 201 (body=%s)", rec.Code, rec.Body.String())
	}

	req = bootstrapReq(token)
	req.RemoteAddr = "127.0.0.1:5555"
	rec = httptest.NewRecorder()
	srv.handleBootstrap(rec, req)
	if rec.Code != http.StatusConflict {
		t.Fatalf("second bootstrap status = %d, want 409 (body=%s)", rec.Code, rec.Body.String())
	}
	var e protocol.ErrorPayload
	if err := json.Unmarshal(rec.Body.Bytes(), &e); err != nil || e.Code != protocol.CodeAlreadyBootstrapped {
		t.Fatalf("second bootstrap code = %q, want %q", e.Code, protocol.CodeAlreadyBootstrapped)
	}
}
