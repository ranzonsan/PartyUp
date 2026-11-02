// Package server 是 PartyUp 服务端的 HTTP + WebSocket 控制面。
//
// 分层：protocol（消息形状）→ store（持久化）→ server（HTTP 端点 + WS 会话）。
// 本文件负责 HTTP 侧（bootstrap / login / invite / me / health）与启动时的一次性
// 初始化；WebSocket 会话在 hub.go。
package server

import (
	"bufio"
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"time"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/store"
)

// Options 是构造 Server 的可选参数。
type Options struct {
	Name         string        // 服务端展示名，auth.ok.server.name
	Version      string        // 二进制版本，auth.ok.server.version
	BootstrapTTL time.Duration // bootstrap token 有效期，默认 24h
	Logger       *slog.Logger  // 默认 slog.Default()
}

// Server 持有依赖与路由。一个进程一个 Server。
type Server struct {
	store        *store.Store
	hub          *Hub
	log          *slog.Logger
	name         string
	version      string
	bootstrapTTL time.Duration
	mux          *http.ServeMux
}

// New 组装 Server。store 由调用方负责 Close。
func New(st *store.Store, opts Options) *Server {
	if opts.Name == "" {
		opts.Name = "PartyUp"
	}
	if opts.Version == "" {
		opts.Version = "0.0.0"
	}
	if opts.BootstrapTTL <= 0 {
		opts.BootstrapTTL = 24 * time.Hour
	}
	if opts.Logger == nil {
		opts.Logger = slog.Default()
	}
	s := &Server{
		store:        st,
		log:          opts.Logger,
		name:         opts.Name,
		version:      opts.Version,
		bootstrapTTL: opts.BootstrapTTL,
	}
	s.hub = NewHub(st, s.log, s.name, s.version)
	s.mux = http.NewServeMux()
	s.routes()
	return s
}

// Handler 返回带访问日志的 HTTP 处理器。
func (s *Server) Handler() http.Handler { return s.withRequestLog(s.mux) }

// Hub 暴露会话中心，供优雅停机时统一关闭连接。
func (s *Server) Hub() *Hub { return s.hub }

func (s *Server) routes() {
	s.mux.HandleFunc("GET /api/health", s.handleHealth)
	s.mux.HandleFunc("POST /api/bootstrap", s.handleBootstrap)
	s.mux.HandleFunc("POST /api/login", s.handleLogin)
	s.mux.HandleFunc("POST /api/invite/{code}", s.handleInviteRegister)
	s.mux.HandleFunc("GET /api/me", s.handleMe)
	s.mux.HandleFunc("GET /ws", s.hub.ServeWS)
}

// ------------------------------------------------------------------
// 启动初始化
// ------------------------------------------------------------------

// BootstrapInfo 是首次启动时向 stdout 输出的敏感一次性凭据。
// 只在这里出现一次：库里存的是 token 的 sha256 与邀请码原文，重启不会重放 token。
type BootstrapInfo struct {
	Token      string // 仅当库中还没有成员时非空
	InviteCode string // 仅当库中没有未使用邀请码时非空
}

// EnsureFirstRun 在启动时保证「可以进得去」：没有成员就发一个 bootstrap token，
// 没有可用邀请码就发一个邀请码。幂等：已初始化且已有邀请码时什么都不返回。
func (s *Server) EnsureFirstRun(ctx context.Context) (BootstrapInfo, error) {
	var info BootstrapInfo

	count, err := s.store.MemberCount(ctx)
	if err != nil {
		return info, err
	}
	if count == 0 {
		token, err := auth.NewToken()
		if err != nil {
			return info, err
		}
		expires := time.Now().Add(s.bootstrapTTL).UnixMilli()
		if err := s.store.SetSetting(ctx, "bootstrap_token_hash", auth.HashToken(token)); err != nil {
			return info, err
		}
		if err := s.store.SetSetting(ctx, "bootstrap_token_expires_ms", fmt.Sprint(expires)); err != nil {
			return info, err
		}
		info.Token = token
	}

	if _, err := s.store.FirstUnusedInvite(ctx); errors.Is(err, store.ErrNotFound) {
		code, err := s.newInvite(ctx, "")
		if err != nil {
			return info, err
		}
		info.InviteCode = code
	} else if err != nil {
		return info, err
	}
	return info, nil
}

// newInvite 生成一个邀请码并落库，冲突时重试。
func (s *Server) newInvite(ctx context.Context, createdBy string) (string, error) {
	for attempt := 0; attempt < 5; attempt++ {
		code, err := auth.NewInviteCode()
		if err != nil {
			return "", err
		}
		_, err = s.store.CreateInvite(ctx, code, createdBy)
		if errors.Is(err, store.ErrConflict) {
			continue
		}
		if err != nil {
			return "", err
		}
		return code, nil
	}
	return "", errors.New("could not allocate a unique invite code")
}

// ------------------------------------------------------------------
// HTTP 处理器
// ------------------------------------------------------------------

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":      true,
		"name":    s.name,
		"version": s.version,
	})
}

type bootstrapRequest struct {
	Token    string `json:"token"`
	Username string `json:"username"`
	Password string `json:"password"`
}

func (s *Server) handleBootstrap(w http.ResponseWriter, r *http.Request) {
	var req bootstrapRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}

	ctx := r.Context()
	hash, err := s.store.GetSetting(ctx, "bootstrap_token_hash")
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusForbidden, protocol.CodeForbidden, "server already bootstrapped")
		return
	}
	if err != nil {
		s.internalError(w, "read bootstrap token", err)
		return
	}
	if subtle.ConstantTimeCompare([]byte(hash), []byte(auth.HashToken(req.Token))) != 1 {
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "invalid bootstrap token")
		return
	}
	if expRaw, err := s.store.GetSetting(ctx, "bootstrap_token_expires_ms"); err == nil {
		var exp int64
		if _, err := fmt.Sscan(expRaw, &exp); err == nil && protocol.NowMS() > exp {
			writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "bootstrap token expired")
			return
		}
	}
	if !auth.ValidName(req.Username) {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument,
			fmt.Sprintf("username must be %d-%d chars of letters/digits/underscore/hyphen/CJK", auth.MinNameLen, auth.MaxNameLen))
		return
	}
	if !auth.ValidPassword(req.Password) {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument,
			fmt.Sprintf("password must be %d-%d bytes", auth.MinPasswordLen, auth.MaxPasswordLen))
		return
	}

	count, err := s.store.MemberCount(ctx)
	if err != nil {
		s.internalError(w, "count members", err)
		return
	}
	if count > 0 {
		writeError(w, http.StatusConflict, protocol.CodeConflict, "server already bootstrapped")
		return
	}

	pwHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	member, err := s.store.CreateMember(ctx, req.Username, pwHash, true)
	if errors.Is(err, store.ErrConflict) {
		writeError(w, http.StatusConflict, protocol.CodeConflict, "username already taken")
		return
	}
	if err != nil {
		s.internalError(w, "create admin", err)
		return
	}
	// 一次性凭据用完即焚。
	_ = s.store.DeleteSetting(ctx, "bootstrap_token_hash")
	_ = s.store.DeleteSetting(ctx, "bootstrap_token_expires_ms")

	s.respondSession(w, http.StatusCreated, member)
}

type loginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req loginRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	ctx := r.Context()

	rec, err := s.store.MemberByName(ctx, req.Username)
	if errors.Is(err, store.ErrNotFound) {
		// 不区分「用户不存在」与「密码错误」，避免账号枚举。
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "invalid username or password")
		return
	}
	if err != nil {
		s.internalError(w, "lookup member", err)
		return
	}
	ok, err := auth.VerifyPassword(rec.PasswordHash, req.Password)
	if err != nil {
		s.internalError(w, "verify password", err)
		return
	}
	if !ok {
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "invalid username or password")
		return
	}
	s.respondSession(w, http.StatusOK, rec.Member)
}

func (s *Server) handleInviteRegister(w http.ResponseWriter, r *http.Request) {
	code := auth.NormalizeInviteCode(r.PathValue("code"))
	if code == "" {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite code not found")
		return
	}
	var req loginRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if !auth.ValidName(req.Username) {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "invalid username")
		return
	}
	if !auth.ValidPassword(req.Password) {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "invalid password")
		return
	}

	pwHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	member, err := s.store.RegisterWithInvite(r.Context(), code, req.Username, pwHash)
	switch {
	case errors.Is(err, store.ErrNotFound):
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite code not found")
		return
	case errors.Is(err, store.ErrInviteUsed):
		writeError(w, http.StatusConflict, protocol.CodeConflict, "invite code already used")
		return
	case errors.Is(err, store.ErrNameTaken):
		writeError(w, http.StatusConflict, protocol.CodeConflict, "username already taken")
		return
	case err != nil:
		s.internalError(w, "register with invite", err)
		return
	}
	s.respondSession(w, http.StatusCreated, member)
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	member, ok := s.authenticate(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"member": toProtocolMember(member)})
}

// ------------------------------------------------------------------
// 内部工具
// ------------------------------------------------------------------

// respondSession 为成员签发一个会话 token 并返回 { member, token }。
func (s *Server) respondSession(w http.ResponseWriter, status int, member store.Member) {
	token, err := auth.NewToken()
	if err != nil {
		s.internalError(w, "generate token", err)
		return
	}
	// 先落库再回包：回包时 token 一定可用。
	//
	// 注意这里不能用 r.Context()：客户端可能已断开，但 token 已经签发出去，
	// 落库失败会让用户拿到一个不可用的 token。用独立 context 并设超时。
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := s.store.CreateSession(ctx, member.ID, auth.HashToken(token)); err != nil {
		s.internalError(w, "create session", err)
		return
	}
	writeJSON(w, status, map[string]any{
		"member": toProtocolMember(member),
		"token":  token,
	})
}

// authenticate 解析 Authorization: Bearer <token>，返回成员。
func (s *Server) authenticate(r *http.Request) (store.Member, bool) {
	const prefix = "bearer "
	header := r.Header.Get("Authorization")
	if len(header) < len(prefix) || !strings.EqualFold(header[:len(prefix)], prefix) {
		return store.Member{}, false
	}
	token := strings.TrimSpace(header[len(prefix):])
	if token == "" {
		return store.Member{}, false
	}
	member, err := s.store.MemberBySession(r.Context(), auth.HashToken(token))
	if err != nil {
		return store.Member{}, false
	}
	return member, true
}

// internalError 记录详细原因，对外只回 INTERNAL，避免把 SQL 细节泄给客户端。
func (s *Server) internalError(w http.ResponseWriter, what string, err error) {
	s.log.Error("request failed", "what", what, "err", err)
	writeError(w, http.StatusInternalServerError, protocol.CodeInternal, "internal server error")
}

// decodeJSON 从请求体解析 JSON（上限 64 KiB），不做 UnknownFields 拒绝——
// 客户端版本可能比服务端新，多出来的字段应当被忽略。
func decodeJSON(w http.ResponseWriter, r *http.Request, out any) error {
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(out); err != nil {
		return fmt.Errorf("invalid json body: %w", err)
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, protocol.ErrorPayload{Code: code, Message: message})
}

// withRequestLog 打印每个 HTTP 请求一行（方法、路径、状态、耗时）。
func (s *Server) withRequestLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		s.log.Info("http",
			"method", r.Method,
			"path", r.URL.Path,
			"status", rec.status,
			"dur_ms", time.Since(start).Milliseconds(),
		)
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(status int) {
	r.status = status
	r.ResponseWriter.WriteHeader(status)
}

// Hijack 转发给底层 ResponseWriter：WebSocket 升级需要它。
func (r *statusRecorder) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	h, ok := r.ResponseWriter.(http.Hijacker)
	if !ok {
		return nil, nil, errors.New("response writer does not support hijacking")
	}
	return h.Hijack()
}

// ------------------------------------------------------------------
// 领域对象 → 协议对象
// ------------------------------------------------------------------

func toProtocolMember(m store.Member) protocol.Member {
	return protocol.Member{ID: m.ID, Name: m.Name, Admin: m.Admin}
}

func toProtocolChannel(ch store.Channel) protocol.Channel {
	return protocol.Channel{
		ID:     ch.ID,
		Name:   ch.Name,
		Voice:  []protocol.VoiceMember{},
		Shares: []protocol.Share{},
	}
}

func toProtocolMessage(m store.Message) protocol.TextMessage {
	return protocol.TextMessage{
		ChannelID: m.ChannelID,
		ID:        m.ID,
		Seq:       m.Seq,
		MemberID:  m.MemberID,
		Body:      m.Body,
		TS:        m.TS,
	}
}
