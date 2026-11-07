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
	"math"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/sfu"
	"partyup/internal/store"
)

// Options 是构造 Server 的可选参数。
type Options struct {
	Name         string        // 服务端展示名，auth.ok.server.name
	Version      string        // 二进制版本，auth.ok.server.version
	BootstrapTTL time.Duration // bootstrap token 有效期，默认 24h
	Logger       *slog.Logger  // 默认 slog.Default()
	// VoiceMode 是服务端**语音**传输策略："relay"（服务端 SFU 中继，默认，见 ADR-0002）
	// 或 "direct"（P2P 直连）。客户端无权选择，只消费结果。
	VoiceMode string
	// ScreenMode 是服务端**屏幕共享**传输策略，与 VoiceMode 相互独立
	// （ADR-0002：三类媒体各自决定路径）。"relay" = 走 SFU 视频通道；
	// "direct" = 成员间 P2P（只支持 1v1 观看）。空值由 NormalizeScreenMode 归一到 relay。
	ScreenMode string
	// StunURLs 是内建 STUN responder 的地址（`stun:host:port`），由 server.info 下发。
	// 空表示没开 STUN：客户端仍可直连，但没有 srflx 候选（跨 NAT 通常连不上）。
	StunURLs []string
	// SessionTTL 是会话 token 的有效期（PROTOCOL.md §1.1），默认 30 天。
	// 采用滑动续期：WS 鉴权成功与 GET /api/me 命中会把过期时间往后推。
	SessionTTL time.Duration
	// LoginRateLimit 是登录失败限速（按 IP + 用户名），零值表示关闭。
	// CLI --login-rate-limit 由 main 解析成结构体传入，默认 10 次 / 15 分钟。
	LoginRateLimit LoginRateLimit
	// TrustProxy 表示部署在**已终止 TLS 的反向代理**之后：此时才信任
	// X-Forwarded-For，并且 bootstrap 的「回环或 TLS」限制改为信任代理连接。
	// 默认 false：非回环明文连接一律拒绝 bootstrap（规格 §7.4）。
	TrustProxy bool
	// ICE 是 SFU 媒体 ICE 的端口范围与对外 IP 配置（--ice-port-range /
	// --ice-public-ip）。零值 = 不限制端口、不改写候选（与旧行为一致）。
	// 容器（Docker bridge）部署靠它把固定 UDP 区间映射出来，并把 host 候选
	// 改写成宿主机/公网 IP，从而不必使用 --network host。
	ICE sfu.ICEConfig
}

// sessionRenewGranularity 是滑动续期的**写节流粒度**（PROTOCOL.md §1.1）：
// 只有剩余有效期比 TTL 少 5 分钟以上时才写库。活跃客户端每 5 分钟最多一次
// 单行 UPDATE，而不是每次请求/每条消息都写；代价是实际失效时刻最多比最后
// 一次活动晚 5 分钟（对 30 天量级完全可接受）。
const sessionRenewGranularity = 5 * time.Minute

// Server 持有依赖与路由。一个进程一个 Server。
type Server struct {
	store        *store.Store
	hub          *Hub
	log          *slog.Logger
	name         string
	version      string
	bootstrapTTL time.Duration
	sessionTTL   time.Duration
	// voiceModeDefault / screenModeDefault 是首次启动写入 DB 的出厂默认值；
	// 之后以 DB 里的设置为准（管理页可改），命令行不再覆盖管理员的选择。
	voiceModeDefault  string
	screenModeDefault string
	trustProxy        bool
	// loginLimiter 为 nil 表示限速关闭（--login-rate-limit=0）。
	loginLimiter *loginLimiter
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
	if opts.SessionTTL <= 0 {
		opts.SessionTTL = store.DefaultSessionTTL
	}
	if opts.Logger == nil {
		opts.Logger = slog.Default()
	}
	opts.VoiceMode = NormalizeVoiceMode(opts.VoiceMode)
	opts.ScreenMode = NormalizeScreenMode(opts.ScreenMode)
	s := &Server{
		store:             st,
		log:               opts.Logger,
		name:              opts.Name,
		version:           opts.Version,
		bootstrapTTL:      opts.BootstrapTTL,
		sessionTTL:        opts.SessionTTL,
		voiceModeDefault:  opts.VoiceMode,
		screenModeDefault: opts.ScreenMode,
		trustProxy:        opts.TrustProxy,
	}
	if opts.LoginRateLimit.Enabled() {
		s.loginLimiter = newLoginLimiter(opts.LoginRateLimit)
	}
	// 设置落库：首次启动写入出厂默认值；之后以 DB 为准（管理页可改）。
	s.ensureSettings()
	if settings, err := s.settings(context.Background()); err == nil {
		opts.VoiceMode, opts.ScreenMode = settings.VoiceMode, settings.ScreenMode
	} else {
		s.log.Warn("load settings", "err", err)
	}
	s.hub = NewHub(st, s.log, s.name, s.version, opts.VoiceMode, opts.ScreenMode, opts.SessionTTL, opts.StunURLs, opts.ICE)
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
	// 匿名探测：客户端「先添加服务器」流程用（规格 §7.3）。走登录限速器。
	s.mux.HandleFunc("GET /api/server-info", s.handleServerInfo)
	s.mux.HandleFunc("POST /api/bootstrap", s.handleBootstrap)
	s.mux.HandleFunc("POST /api/login", s.handleLogin)
	// 邀请码预检：POST + JSON body，不用 GET 路径——邀请码不能进访问日志。
	s.mux.HandleFunc("POST /api/invite/validate", s.handleInviteValidate)
	s.mux.HandleFunc("POST /api/invite/{code}", s.handleInviteRegister)
	// 开放注册（registration=open 且未关闭时可用；2026-11-07 补）。
	s.mux.HandleFunc("POST /api/register", s.handleRegister)
	s.mux.HandleFunc("GET /api/members", s.handleMembers)
	s.mux.HandleFunc("GET /api/me", s.handleMe)
	// 自助修改（§2：所有角色都能改自己的显示名/密码）。
	s.mux.HandleFunc("PATCH /api/me", s.handleUpdateMe)
	s.mux.HandleFunc("POST /api/me/password", s.handleChangePassword)
	// 退出登录（2026-11-07）：撤销当前 token 并关闭它的 WS 连接；幂等，成功 204 无 body。
	s.mux.HandleFunc("POST /api/logout", s.handleLogout)

	// 服务器管理（规格 §9）：全部服务端校验角色，不依赖客户端隐藏按钮。
	s.mux.HandleFunc("GET /api/admin/overview", s.handleAdminOverview)
	s.mux.HandleFunc("GET /api/admin/settings", s.handleAdminGetSettings)
	s.mux.HandleFunc("PATCH /api/admin/settings", s.handleAdminPatchSettings)
	s.mux.HandleFunc("GET /api/admin/invites", s.handleAdminInvites)
	s.mux.HandleFunc("POST /api/admin/invites", s.handleAdminInviteCreate)
	s.mux.HandleFunc("DELETE /api/admin/invites", s.handleAdminInviteRevoke)
	s.mux.HandleFunc("GET /api/admin/members", s.handleAdminMembers)
	s.mux.HandleFunc("POST /api/admin/members/{id}/kick", s.handleAdminMemberKick)
	s.mux.HandleFunc("POST /api/admin/members/{id}/ban", s.handleAdminMemberBan)
	s.mux.HandleFunc("POST /api/admin/members/{id}/unban", s.handleAdminMemberUnban)
	s.mux.HandleFunc("POST /api/admin/members/{id}/password", s.handleAdminMemberPassword)
	s.mux.HandleFunc("POST /api/admin/members/{id}/role", s.handleAdminMemberRole)
	s.mux.HandleFunc("POST /api/admin/owner/transfer", s.handleAdminOwnerTransfer)
	s.mux.HandleFunc("GET /api/admin/channels", s.handleAdminChannels)
	s.mux.HandleFunc("POST /api/admin/channels", s.handleAdminChannelCreate)
	s.mux.HandleFunc("PATCH /api/admin/channels", s.handleAdminChannelPatch)
	s.mux.HandleFunc("DELETE /api/admin/channels", s.handleAdminChannelDelete)
	s.mux.HandleFunc("POST /api/admin/messages/delete", s.handleAdminMessageDelete)
	s.mux.HandleFunc("GET /api/admin/audit", s.handleAdminAudit)

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
	// DisplayName 可选；未提供/空白时回退到 username（见 resolveDisplayName）。
	DisplayName string `json:"display_name"`
}

func (s *Server) handleBootstrap(w http.ResponseWriter, r *http.Request) {
	var req bootstrapRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}

	// 安全边界（规格 §7.4）：bootstrap 只允许回环或 TLS 连接，避免公网直接抢
	// owner 位；同时走登录限速器，防暴力尝试 token。必须在任何查库之前。
	var bootstrapRateKey string
	if s.loginLimiter != nil {
		bootstrapRateKey = loginRateKey(clientIP(r), "bootstrap")
		if allowed, retryAfter := s.loginLimiter.allow(bootstrapRateKey); !allowed {
			secs := int(math.Ceil(retryAfter.Seconds()))
			if secs < 1 {
				secs = 1
			}
			w.Header().Set("Retry-After", strconv.Itoa(secs))
			writeError(w, http.StatusTooManyRequests, protocol.CodeRateLimited, "too many bootstrap attempts, retry later")
			return
		}
	}
	if trusted, why := s.bootstrapTrusted(r); !trusted {
		s.log.Warn("bootstrap rejected", "reason", why, "remote", r.RemoteAddr, "tls", r.TLS != nil)
		writeError(w, http.StatusForbidden, protocol.CodeForbidden,
			"bootstrap is only allowed from a loopback or TLS connection")
		return
	}

	ctx := r.Context()
	hash, err := s.store.GetSetting(ctx, "bootstrap_token_hash")
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusConflict, protocol.CodeAlreadyBootstrapped, "server already bootstrapped")
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
	displayName, ok := resolveDisplayName(req.DisplayName, req.Username)
	if !ok {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, invalidDisplayNameMessage)
		return
	}

	count, err := s.store.MemberCount(ctx)
	if err != nil {
		s.internalError(w, "count members", err)
		return
	}
	if count > 0 {
		writeError(w, http.StatusConflict, protocol.CodeAlreadyBootstrapped, "server already bootstrapped")
		return
	}

	pwHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	// 首个账号就是 owner（规格 §1）：owner 只能通过 bootstrap 产生，之后只能转让。
	member, err := s.store.CreateMemberWithRole(ctx, req.Username, displayName, pwHash, store.RoleOwner)
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
	if s.loginLimiter != nil {
		s.loginLimiter.reset(bootstrapRateKey) // 成功 bootstrap 不算失败尝试
	}

	s.respondSession(w, http.StatusCreated, member)
}

type loginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// registerRequest 是邀请码注册的请求体（在 loginRequest 之上多一个可选显示名）。
type registerRequest struct {
	Username    string `json:"username"`
	Password    string `json:"password"`
	DisplayName string `json:"display_name"`
}

// invalidDisplayNameMessage 是显示名校验失败的统一文案。
const invalidDisplayNameMessage = "display_name must be 1-32 printable Unicode characters"

// resolveDisplayName 把可选的 display_name 解析成最终展示名：空白/未提供回退登录名。
// 返回 false 表示显式提供了但不符合可打印 Unicode 长度约束。
func resolveDisplayName(raw, username string) (string, bool) {
	displayName := strings.TrimSpace(raw)
	if displayName == "" {
		return username, true
	}
	if !auth.ValidDisplayName(displayName) {
		return "", false
	}
	return displayName, true
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req loginRequest
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	ctx := r.Context()

	// 限速判定必须在查库与 argon2 校验**之前**（见 ratelimit.go）。
	// 键 = IP + 用户名；成功登录 reset，失败则额度已在 allow 里扣掉，
	// 所以这里不存在「先检查后记账」的并发窗口。
	var rateKey string
	if s.loginLimiter != nil {
		rateKey = loginRateKey(clientIP(r), req.Username)
		if allowed, retryAfter := s.loginLimiter.allow(rateKey); !allowed {
			retryAfterSeconds := int(math.Ceil(retryAfter.Seconds()))
			if retryAfterSeconds < 1 {
				retryAfterSeconds = 1
			}
			w.Header().Set("Retry-After", strconv.Itoa(retryAfterSeconds))
			writeError(w, http.StatusTooManyRequests, protocol.CodeRateLimited,
				"too many failed login attempts, retry later")
			return
		}
	}

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
	// 封禁检查放在密码校验之后：既不让被封号的人登录，也不给未持密码者探测
	// 「这个账号是否被封」的机会（规格 §5）。
	if banned, err := s.store.IsBanned(ctx, rec.ID); err != nil {
		s.internalError(w, "check ban", err)
		return
	} else if banned {
		writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
		return
	}
	// 成功登录清零（PROTOCOL.md §1.1）：不让合法用户被自己此前的手误拖累。
	if s.loginLimiter != nil {
		s.loginLimiter.reset(rateKey)
	}
	s.respondSession(w, http.StatusOK, rec.Member)
}

// handleInviteValidate 是**非消费**的邀请码预检（2026-11-06 新增）。
//
// 只回答「这个码现在还能不能用」：200 {valid:true}、404 不存在、409 已被使用。
// 不建成员、不消费邀请码、不返回 CreatedBy/UsedBy 等任何敏感信息。预检成功不等于
// 占用：最终 POST /api/invite/{code} 仍在同一事务里重新校验并消费。
//
// 用 POST + JSON body 而不是 GET 路径，避免邀请码出现在访问日志/代理日志里。
func (s *Server) handleInviteValidate(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Code string `json:"code"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	code := auth.NormalizeInviteCode(req.Code)
	if code == "" {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite code not found")
		return
	}
	inv, err := s.store.InviteByCode(r.Context(), code)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite code not found")
		return
	}
	if err != nil {
		s.internalError(w, "lookup invite", err)
		return
	}
	if inv.UsedBy != "" || inv.UsedAt != 0 {
		writeError(w, http.StatusConflict, protocol.CodeConflict, "invite code already used")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"valid": true})
}

func (s *Server) handleInviteRegister(w http.ResponseWriter, r *http.Request) {
	code := auth.NormalizeInviteCode(r.PathValue("code"))
	if code == "" {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite code not found")
		return
	}
	var req registerRequest
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
	displayName, ok := resolveDisplayName(req.DisplayName, req.Username)
	if !ok {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, invalidDisplayNameMessage)
		return
	}

	// 注册策略与成员上限（规格 §3A）：closed 时拒绝新注册；到上限回 409。
	// 放在密码哈希之前，避免被刷注册打满 CPU。
	settings, err := s.settings(r.Context())
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	if settings.Registration == "closed" {
		writeError(w, http.StatusForbidden, protocol.CodeForbidden, "registration is closed")
		return
	}
	if settings.MemberLimit > 0 {
		count, err := s.store.MemberCount(r.Context())
		if err != nil {
			s.internalError(w, "count members", err)
			return
		}
		if count >= settings.MemberLimit {
			writeError(w, http.StatusConflict, protocol.CodeConflict, "server member limit reached")
			return
		}
	}

	pwHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	member, err := s.store.RegisterWithInvite(r.Context(), code, req.Username, displayName, pwHash)
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

// handleRegister 是「开放注册」入口（registration=open，2026-11-07 补）。
//
// 与邀请注册的唯一区别是不需要邀请码；其余规则一致：注册策略、成员上限
// member_limit、用户名/密码/显示名校验、登录限速（匿名端点按 IP 计数，成功清零）。
// 新成员固定 role=user——owner 只能由 bootstrap 产生，admin 只能由 owner 任免。
func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	// 匿名端点：先限速再解析 body，避免被刷注册打满 CPU。
	var rateKey string
	if s.loginLimiter != nil {
		rateKey = loginRateKey(clientIP(r), "register")
		if allowed, retryAfter := s.loginLimiter.allow(rateKey); !allowed {
			secs := int(math.Ceil(retryAfter.Seconds()))
			if secs < 1 {
				secs = 1
			}
			w.Header().Set("Retry-After", strconv.Itoa(secs))
			writeError(w, http.StatusTooManyRequests, protocol.CodeRateLimited, "too many registration attempts, retry later")
			return
		}
	}

	var req registerRequest
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
	displayName, ok := resolveDisplayName(req.DisplayName, req.Username)
	if !ok {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, invalidDisplayNameMessage)
		return
	}

	ctx := r.Context()
	settings, err := s.settings(ctx)
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	switch settings.Registration {
	case "open":
		// 继续。
	case "invite":
		writeError(w, http.StatusForbidden, protocol.CodeForbidden, "registration requires an invite code")
		return
	default: // closed
		writeError(w, http.StatusForbidden, protocol.CodeForbidden, "registration is closed")
		return
	}
	if settings.MemberLimit > 0 {
		count, err := s.store.MemberCount(ctx)
		if err != nil {
			s.internalError(w, "count members", err)
			return
		}
		if count >= settings.MemberLimit {
			writeError(w, http.StatusConflict, protocol.CodeConflict, "server member limit reached")
			return
		}
	}

	pwHash, err := auth.HashPassword(req.Password)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	member, err := s.store.CreateMemberWithRole(ctx, req.Username, displayName, pwHash, store.RoleUser)
	switch {
	case errors.Is(err, store.ErrConflict):
		writeError(w, http.StatusConflict, protocol.CodeConflict, "username already taken")
		return
	case err != nil:
		s.internalError(w, "register", err)
		return
	}
	if s.loginLimiter != nil {
		s.loginLimiter.reset(rateKey) // 成功注册不算失败尝试
	}
	s.respondSession(w, http.StatusCreated, member)
}

// handleMembers 返回全服务器名册（含离线），供 UI 一次性拉取初始状态。
// 之后的增量由 WS 的 members.state 推送；只读、不产生任何写操作。
func (s *Server) handleMembers(w http.ResponseWriter, r *http.Request) {
	if _, _, err := s.currentMember(r); err != nil {
		if errors.Is(err, errBanned) {
			writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
			return
		}
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return
	}
	members, err := s.hub.MemberRoster(r.Context())
	if err != nil {
		s.internalError(w, "list member roster", err)
		return
	}
	writeJSON(w, http.StatusOK, protocol.MemberListOK{Members: members})
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	member, tokenHash, err := s.currentMember(r)
	if err != nil {
		if errors.Is(err, errBanned) {
			writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
			return
		}
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return
	}
	// 滑动续期（PROTOCOL.md §1.1）：/api/me 是客户端定期校验 token 的端点，在这里
	// 续期能让「只挂着不操作」的会话保持有效。续期失败不影响本次响应。
	s.touchSession(tokenHash)
	writeJSON(w, http.StatusOK, map[string]any{"member": toProtocolMember(member)})
}

// ------------------------------------------------------------------
// 内部工具
// ------------------------------------------------------------------

// respondSession 为成员签发一个会话 token 并返回 { member, token }。
//
// 单端登录（2026-11-07）：在**同一事务**里先撤销该成员所有其他会话，再插入新
// 会话；提交成功后关闭旧会话的 WS 连接并给它们发 session.replaced 通知。因此
// 任一时刻每个成员最多一条有效 session。bootstrap / login / invite 注册三条入口
// 都调用本函数，行为一致。
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
	tokenHash := auth.HashToken(token)
	revoked, err := s.store.ReplaceSessions(ctx, member.ID, tokenHash,
		time.Now().Add(s.sessionTTL).UnixMilli())
	if err != nil {
		s.internalError(w, "replace sessions", err)
		return
	}
	if revoked > 0 {
		// 固定包含 "session replaced" 的日志行：真机验收/排障靠它确认旧端是被顶掉，
		// 而不是普通网络断线。
		s.log.Info("session replaced",
			"member_id", member.ID, "member_name", member.Name,
			"revoked", revoked, "reason", protocol.SessionReasonLoginElsewhere)
		// 只在 DB 提交成功后关旧连接：提交才是「谁赢了」的裁决点，
		// 先关连接再提交可能关掉并发登录里真正的赢家。
		s.hub.closeMemberConns(member.ID, protocol.SessionReasonLoginElsewhere)
	}
	writeJSON(w, status, map[string]any{
		"member": toProtocolMember(member),
		"token":  token,
	})
}

// handleLogout 处理 POST /api/logout：撤销当前 token 并关闭它的 WS 连接。
//
// 语义（PROTOCOL.md §1.1）：
//   - 成功 204 且无 body；**幂等**——同一 token 重复调用仍 204；
//   - token 不在 sessions 表里 → 401 UNAUTHENTICATED；
//   - 已撤销/已过期的 token 仍 204：撤销本身是幂等的，过期 token 本来也不能用，
//     让它「退出成功」比报 401 更不容易把客户端卡在退出流程里。
//
// 刻意不复用 authenticate：authenticate 会拒绝已撤销 token，而 logout 必须接受
// 「已经撤销」的情况。
func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing bearer token")
		return
	}
	// 与 respondSession 同理：撤销必须在客户端断开后仍完成，所以用独立 ctx。
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	tokenHash := auth.HashToken(token)
	if err := s.store.RevokeSession(ctx, tokenHash); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "invalid token")
			return
		}
		s.internalError(w, "revoke session", err)
		return
	}
	closed := s.hub.closeConnsByTokenHash(tokenHash, protocol.SessionReasonLogout)
	s.log.Info("session revoked", "reason", protocol.SessionReasonLogout, "closed_conns", closed)
	w.WriteHeader(http.StatusNoContent)
}

// touchSession 对 token 做滑动续期；节流逻辑在 store.TouchSession。
// 失败只记日志：续期是附带动作，不能因为它让 /api/me 也失败。
func (s *Server) touchSession(tokenHash string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := s.store.TouchSession(ctx, tokenHash,
		time.Now().UnixMilli(), s.sessionTTL, sessionRenewGranularity); err != nil {
		s.log.Warn("renew session", "err", err)
	}
}

// authenticate 解析 Authorization: Bearer <token>，返回成员与 token 哈希。
// 兼容旧调用点；新代码请用 currentMember（它会额外检查封禁）。
func (s *Server) authenticate(r *http.Request) (store.Member, string, bool) {
	member, tokenHash, err := s.currentMember(r)
	if err != nil {
		return store.Member{}, "", false
	}
	return member, tokenHash, true
}

// bearerToken 从 Authorization 头取 Bearer token；缺失或格式不对返回 false。
func bearerToken(r *http.Request) (string, bool) {
	const prefix = "bearer "
	header := r.Header.Get("Authorization")
	if len(header) < len(prefix) || !strings.EqualFold(header[:len(prefix)], prefix) {
		return "", false
	}
	token := strings.TrimSpace(header[len(prefix):])
	if token == "" {
		return "", false
	}
	return token, true
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
	return protocol.Member{
		ID:          m.ID,
		Name:        m.Name,
		DisplayName: m.DisplayName,
		Role:        string(m.Role),
		Admin:       m.Role.IsAdmin(),
	}
}

func toProtocolChannel(ch store.Channel) protocol.Channel {
	return protocol.Channel{
		ID:     ch.ID,
		Name:   ch.Name,
		Voice:  []protocol.VoiceMember{},
		Shares: []protocol.ShareState{},
	}
}

func toProtocolMessage(m store.Message) protocol.TextMessage {
	return protocol.TextMessage{
		ChannelID:         m.ChannelID,
		ID:                m.ID,
		Seq:               m.Seq,
		MemberID:          m.MemberID,
		MemberName:        m.MemberName,
		MemberDisplayName: m.MemberDisplayName,
		Body:              m.Body,
		TS:                m.TS,
	}
}
