package server

// 角色 / 管理 API / 审计 / 封禁 / 匿名 server-info / bootstrap 加固
// （2026-11-07 身份与权限第一批；规格 `.docs/identity-and-onboarding-20261107.md`
// §1/§2/§3/§4/§5/§7.3/§7.4/§9）。
//
// 权限判断的唯一入口是 requireRole：所有管理端点在**服务端**校验角色，绝不依赖
// 客户端隐藏按钮。owner 是「唯一 + 不可被管理」的特殊角色：任免管理员、危险设置、
// 转让所有权只有 owner 能做；admin 不能对 owner 做任何写操作（403 OWNER_ONLY）。

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"time"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/store"
)

// 角色层级：user < admin < owner。
func roleRank(r store.Role) int {
	switch r {
	case store.RoleOwner:
		return 3
	case store.RoleAdmin:
		return 2
	default:
		return 1
	}
}

// currentMember 解析 Bearer token、检查封禁，返回成员与 token 哈希。
// 错误区分「没登录/无效 token」与「被封禁」，供 HTTP 层映射 401 / 403。
var (
	errNoAuth = errors.New("no valid session")
	errBanned = errors.New("member is banned")
)

func (s *Server) currentMember(r *http.Request) (store.Member, string, error) {
	token, ok := bearerToken(r)
	if !ok {
		return store.Member{}, "", errNoAuth
	}
	tokenHash := auth.HashToken(token)
	member, err := s.store.MemberBySession(r.Context(), tokenHash)
	if err != nil {
		return store.Member{}, "", errNoAuth
	}
	banned, err := s.store.IsBanned(r.Context(), member.ID)
	if err != nil {
		return store.Member{}, "", err
	}
	if banned {
		return member, tokenHash, errBanned
	}
	return member, tokenHash, nil
}

// requireRole 是管理端点的统一守卫：认证 + 封禁检查 + 角色检查。
// 返回 false 时已经写好响应，调用方直接 return。
func (s *Server) requireRole(w http.ResponseWriter, r *http.Request, min store.Role) (store.Member, bool) {
	member, _, err := s.currentMember(r)
	switch {
	case errors.Is(err, errBanned):
		writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
		return store.Member{}, false
	case err != nil:
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return store.Member{}, false
	}
	if roleRank(member.Role) < roleRank(min) {
		code := protocol.CodeAdminOnly
		if min == store.RoleOwner {
			code = protocol.CodeOwnerOnly
		}
		writeError(w, http.StatusForbidden, code, "insufficient permissions")
		return store.Member{}, false
	}
	return member, true
}

// audit 追加审计日志：业务事务提交后调用，失败只 warn（规格 §4）。
// detail 只放非敏感摘要；密码/token 一律不得进入。
func (s *Server) audit(r *http.Request, actor store.Member, action, target string, detail map[string]any) {
	var raw string
	if len(detail) > 0 {
		if b, err := json.Marshal(detail); err == nil {
			raw = string(b)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	ev := store.AuditEvent{
		ActorID:   actor.ID,
		ActorName: actor.Name,
		Action:    action,
		Target:    target,
		Detail:    raw,
		IP:        clientIP(r),
	}
	if err := s.store.AppendAudit(ctx, ev); err != nil {
		s.log.Warn("append audit failed", "action", action, "err", err)
	}
}

// ------------------------------------------------------------------
// 匿名探测 / bootstrap 安全边界（规格 §7.3/§7.4）
// ------------------------------------------------------------------

// allowAnonProbe 让匿名端点走同一个登录限速器（按 IP 计数）：
// 防止公网扫 initialized 状态或暴力尝试 bootstrap token。成功不重置（匿名端点没有
// 「成功」语义），额度按窗口回填。
func (s *Server) allowAnonProbe(w http.ResponseWriter, r *http.Request, scope string) bool {
	if s.loginLimiter == nil {
		return true
	}
	allowed, retryAfter := s.loginLimiter.allow(loginRateKey(clientIP(r), scope))
	if allowed {
		return true
	}
	secs := int(retryAfter.Seconds() + 0.999)
	if secs < 1 {
		secs = 1
	}
	w.Header().Set("Retry-After", strconv.Itoa(secs))
	writeError(w, http.StatusTooManyRequests, protocol.CodeRateLimited, "too many requests, retry later")
	return false
}

// bootstrapTrusted 判定「这条连接是否可以执行 bootstrap」（规格 §7.4 第 4 道检查）。
//
// 规则：回环连接或 TLS 连接可信；否则必须显式 --trust-proxy（部署者声明反向代理
// 已终止 TLS，且 X-Forwarded-For 可被信任）。返回 ok=false 时 reason 用于日志。
func (s *Server) bootstrapTrusted(r *http.Request) (bool, string) {
	if r.TLS != nil {
		return true, ""
	}
	ip := clientIP(r)
	if s.trustProxy {
		// 代理部署：信任代理提供的客户端地址；代理侧负责终止 TLS。
		if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
			ip = clientIPFromXFF(xff)
		}
		return true, ""
	}
	if parseIP(ip).IsLoopback() {
		return true, ""
	}
	return false, fmt.Sprintf("plaintext non-loopback connection from %s (use loopback, TLS, or --trust-proxy)", ip)
}

// parseIP 解析 IP；失败返回 nil（nil.IsLoopback() 为 false）。
func parseIP(s string) net.IP { return net.ParseIP(s) }

// clientIPFromXFF 取 X-Forwarded-For 最左（原始客户端）地址。
// 只在 --trust-proxy 下调用；不信任该头时一律用 RemoteAddr（见 clientIP）。
func clientIPFromXFF(xff string) string {
	for i := 0; i < len(xff); i++ {
		if xff[i] == ',' {
			return trimSpace(xff[:i])
		}
	}
	return trimSpace(xff)
}

func trimSpace(s string) string {
	start, end := 0, len(s)
	for start < end && (s[start] == ' ' || s[start] == '\t') {
		start++
	}
	for end > start && (s[end-1] == ' ' || s[end-1] == '\t') {
		end--
	}
	return s[start:end]
}

// handleServerInfo 是匿名探测端点（客户端「先添加服务器」流程用）。
// 只返回 UI 需要的状态；initialized 每次重新查库，不做进程内缓存。
func (s *Server) handleServerInfo(w http.ResponseWriter, r *http.Request) {
	if !s.allowAnonProbe(w, r, "server-info") {
		return
	}
	ctx := r.Context()
	count, err := s.store.MemberCount(ctx)
	if err != nil {
		s.internalError(w, "count members", err)
		return
	}
	st, err := s.settings(ctx)
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	trusted, _ := s.bootstrapTrusted(r)
	writeJSON(w, http.StatusOK, protocol.ServerInfoPublic{
		Name:             st.DisplayName,
		Version:          s.version,
		ProtocolVersion:  protocol.Version,
		Initialized:      count > 0,
		Registration:     st.Registration,
		VoiceMode:        st.VoiceMode,
		ScreenMode:       st.ScreenMode,
		BootstrapAllowed: count == 0 && trusted,
	})
}

// ------------------------------------------------------------------
// 概览
// ------------------------------------------------------------------

func (s *Server) handleAdminOverview(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	_ = actor
	ctx := r.Context()
	settings, err := s.settings(ctx)
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	members, err := s.store.MemberCount(ctx)
	if err != nil {
		s.internalError(w, "count members", err)
		return
	}
	channels, err := s.store.ListChannels(ctx)
	if err != nil {
		s.internalError(w, "list channels", err)
		return
	}
	stats := s.hub.Stats()
	writeJSON(w, http.StatusOK, map[string]any{
		"name":             settings.DisplayName,
		"version":          s.version,
		"protocol_version": protocol.Version,
		"initialized":      members > 0,
		"member_count":     members,
		"online_count":     stats.Online,
		"channel_count":    len(channels),
		"voice_rooms":      stats.VoiceRooms,
		"active_shares":    stats.ActiveShares,
		"voice_mode":       settings.VoiceMode,
		"screen_mode":      settings.ScreenMode,
		"registration":     settings.Registration,
	})
}

// ------------------------------------------------------------------
// 设置
// ------------------------------------------------------------------

func (s *Server) handleAdminGetSettings(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.requireRole(w, r, store.RoleAdmin); !ok {
		return
	}
	st, err := s.settings(r.Context())
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	writeJSON(w, http.StatusOK, st)
}

// handleAdminPatchSettings 按字段白名单合并设置。
// 非危险项 admin 可改；危险项（语音/共享模式、最大共享路数）仅 owner，
// 由 role 检查在写入前拦截（不靠客户端隐藏按钮）。
func (s *Server) handleAdminPatchSettings(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	var patch settingsPatch
	if err := decodeJSON(w, r, &patch); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if len(patch) == 0 {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "empty settings patch")
		return
	}
	// 危险项先判权限：即使同一请求里混了普通项，也整体拒绝，避免「部分成功」。
	if actor.Role != store.RoleOwner {
		for key := range patch {
			if dangerousSettingKeys[key] {
				writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly,
					"dangerous setting "+key+" can only be changed by the owner")
				return
			}
		}
	}
	ctx := r.Context()
	st, err := s.settings(ctx)
	if err != nil {
		s.internalError(w, "load settings", err)
		return
	}
	before := st
	changed, err := s.applySettingsPatch(&st, patch)
	if err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if len(changed) == 0 {
		writeJSON(w, http.StatusOK, st)
		return
	}
	if err := s.saveSettings(ctx, st); err != nil {
		s.internalError(w, "save settings", err)
		return
	}
	modeChanged := before.VoiceMode != st.VoiceMode || before.ScreenMode != st.ScreenMode
	if modeChanged {
		s.hub.SetModes(st.VoiceMode, st.ScreenMode)
		// 危险设置单独一条审计（规格 §4：便于追责）。
		s.audit(r, actor, "server.mode_change", "", map[string]any{
			"voice_mode":  map[string]string{"from": before.VoiceMode, "to": st.VoiceMode},
			"screen_mode": map[string]string{"from": before.ScreenMode, "to": st.ScreenMode},
		})
	}
	s.audit(r, actor, "settings.update", "", map[string]any{"changed": changed})
	writeJSON(w, http.StatusOK, st)
}

// ------------------------------------------------------------------
// 成员
// ------------------------------------------------------------------

type adminMemberDTO struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	DisplayName string `json:"display_name"`
	Role        string `json:"role"`
	Admin       bool   `json:"admin"`
	CreatedAt   int64  `json:"created_at"`
	Banned      bool   `json:"banned"`
	BannedAt    int64  `json:"banned_at,omitempty"`
	BanReason   string `json:"ban_reason,omitempty"`
}

func (s *Server) handleAdminMembers(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.requireRole(w, r, store.RoleAdmin); !ok {
		return
	}
	rows, err := s.store.ListMembersAdmin(r.Context())
	if err != nil {
		s.internalError(w, "list members", err)
		return
	}
	out := make([]adminMemberDTO, 0, len(rows))
	for _, m := range rows {
		out = append(out, adminMemberDTO{
			ID: m.ID, Name: m.Name, DisplayName: m.DisplayName,
			Role: string(m.Role), Admin: m.Admin, CreatedAt: m.CreatedAt,
			Banned: m.Banned, BannedAt: m.BannedAt, BanReason: m.BanReason,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"members": out})
}

// adminTarget 读取路径里的成员 id 并做「admin 不能操作 owner」的通用检查。
func (s *Server) adminTarget(w http.ResponseWriter, r *http.Request, actor store.Member) (store.Member, bool) {
	id := r.PathValue("id")
	target, err := s.store.MemberByID(r.Context(), id)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		return store.Member{}, false
	}
	if err != nil {
		s.internalError(w, "load member", err)
		return store.Member{}, false
	}
	// admin 不能对 owner 做任何写操作；owner 也不能被自己踢/封（数据层同样兜底）。
	if target.Role == store.RoleOwner && actor.Role != store.RoleOwner {
		writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "cannot modify the owner")
		return store.Member{}, false
	}
	return target, true
}

func (s *Server) handleAdminMemberKick(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	target, ok := s.adminTarget(w, r, actor)
	if !ok {
		return
	}
	if target.Role == store.RoleOwner {
		writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "the owner cannot be kicked")
		return
	}
	revoked, err := s.store.RevokeAllSessions(r.Context(), target.ID)
	if err != nil {
		s.internalError(w, "revoke sessions", err)
		return
	}
	closed := s.hub.closeMemberConns(target.ID, protocol.SessionReasonKicked)
	s.audit(r, actor, "member.kick", target.ID, map[string]any{
		"member_name": target.Name, "revoked_sessions": revoked, "closed_conns": closed,
	})
	writeJSON(w, http.StatusOK, map[string]any{"revoked_sessions": revoked, "closed_conns": closed})
}

func (s *Server) handleAdminMemberBan(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	target, ok := s.adminTarget(w, r, actor)
	if !ok {
		return
	}
	if target.Role == store.RoleOwner {
		writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "the owner cannot be banned")
		return
	}
	var req struct {
		Reason string `json:"reason"`
	}
	_ = decodeJSON(w, r, &req) // reason 可选；空 body 也接受
	if err := s.store.BanMember(r.Context(), target.ID, actor.ID, req.Reason); err != nil {
		switch {
		case errors.Is(err, store.ErrOwnerProtected):
			writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "the owner cannot be banned")
		case errors.Is(err, store.ErrNotFound):
			writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		default:
			s.internalError(w, "ban member", err)
		}
		return
	}
	closed := s.hub.closeMemberConns(target.ID, protocol.SessionReasonBanned)
	s.audit(r, actor, "member.ban", target.ID, map[string]any{
		"member_name": target.Name, "reason": req.Reason, "closed_conns": closed,
	})
	writeJSON(w, http.StatusOK, map[string]any{"banned": true, "closed_conns": closed})
}

func (s *Server) handleAdminMemberUnban(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	id := r.PathValue("id")
	if _, err := s.store.MemberByID(r.Context(), id); errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		return
	}
	if err := s.store.UnbanMember(r.Context(), id); err != nil {
		s.internalError(w, "unban member", err)
		return
	}
	s.audit(r, actor, "member.unban", id, nil)
	writeJSON(w, http.StatusOK, map[string]any{"banned": false})
}

// handleAdminMemberPassword 重置密码：生成一次性临时密码返回给操作者，
// 并撤销目标成员的全部会话（重置密码通常意味着账号可能已泄露）。
func (s *Server) handleAdminMemberPassword(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	target, ok := s.adminTarget(w, r, actor)
	if !ok {
		return
	}
	// owner 的密码只有 owner 自己能重置。
	if target.Role == store.RoleOwner && actor.ID != target.ID {
		writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "cannot reset the owner's password")
		return
	}
	temp, err := auth.NewToken() // 43 字符 base64url，满足密码长度约束
	if err != nil {
		s.internalError(w, "generate password", err)
		return
	}
	hash, err := auth.HashPassword(temp)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	if err := s.store.SetMemberPassword(r.Context(), target.ID, hash); err != nil {
		s.internalError(w, "set password", err)
		return
	}
	revoked, _ := s.store.RevokeAllSessions(r.Context(), target.ID)
	closed := s.hub.closeMemberConns(target.ID, protocol.SessionReasonKicked)
	s.audit(r, actor, "member.reset_password", target.ID, map[string]any{
		"member_name": target.Name, "revoked_sessions": revoked, "closed_conns": closed,
	})
	writeJSON(w, http.StatusOK, map[string]any{
		"password": temp, "revoked_sessions": revoked, "closed_conns": closed,
	})
}

// handleAdminMemberRole 在 user ↔ admin 之间调整（仅 owner）。
func (s *Server) handleAdminMemberRole(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleOwner)
	if !ok {
		return
	}
	target, ok := s.adminTarget(w, r, actor)
	if !ok {
		return
	}
	var req struct {
		Role string `json:"role"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	role, valid := store.ParseRole(req.Role)
	if !valid || role == store.RoleOwner {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "role must be user or admin")
		return
	}
	if target.Role == store.RoleOwner {
		writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "the owner's role cannot be changed")
		return
	}
	if err := s.store.SetMemberRole(r.Context(), target.ID, role); err != nil {
		switch {
		case errors.Is(err, store.ErrOwnerProtected):
			writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "the owner's role cannot be changed")
		case errors.Is(err, store.ErrNotFound):
			writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		case errors.Is(err, store.ErrInvalidRole):
			writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "invalid role")
		default:
			s.internalError(w, "set role", err)
		}
		return
	}
	s.audit(r, actor, "member.role_change", target.ID, map[string]any{
		"member_name": target.Name, "from": string(target.Role), "to": req.Role,
	})
	s.hub.broadcastMemberState()
	writeJSON(w, http.StatusOK, map[string]any{"id": target.ID, "role": req.Role})
}

// handleAdminOwnerTransfer 转让所有权（仅 owner）。
// 旧 owner 自动降为 admin（store.TransferOwner 内保证先降后升，满足唯一索引）。
func (s *Server) handleAdminOwnerTransfer(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleOwner)
	if !ok {
		return
	}
	var req struct {
		MemberID string `json:"member_id"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if req.MemberID == "" || req.MemberID == actor.ID {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "member_id must be another member")
		return
	}
	target, err := s.store.MemberByID(r.Context(), req.MemberID)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		return
	}
	if err != nil {
		s.internalError(w, "load member", err)
		return
	}
	if err := s.store.TransferOwner(r.Context(), actor.ID, target.ID); err != nil {
		switch {
		case errors.Is(err, store.ErrOwnerProtected):
			writeError(w, http.StatusForbidden, protocol.CodeOwnerOnly, "only the owner can transfer ownership")
		case errors.Is(err, store.ErrNotFound):
			writeError(w, http.StatusNotFound, protocol.CodeNotFound, "member not found")
		default:
			s.internalError(w, "transfer owner", err)
		}
		return
	}
	s.audit(r, actor, "owner.transfer", target.ID, map[string]any{
		"from": actor.Name, "to": target.Name,
	})
	s.hub.broadcastMemberState()
	writeJSON(w, http.StatusOK, map[string]any{"owner_id": target.ID, "owner_name": target.Name})
}

// ------------------------------------------------------------------
// 邀请码
// ------------------------------------------------------------------

type adminInviteDTO struct {
	Code      string `json:"code"`
	CreatedBy string `json:"created_by,omitempty"`
	CreatedAt int64  `json:"created_at"`
	UsedBy    string `json:"used_by,omitempty"`
	UsedAt    int64  `json:"used_at,omitempty"`
	Used      bool   `json:"used"`
}

func (s *Server) handleAdminInvites(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.requireRole(w, r, store.RoleAdmin); !ok {
		return
	}
	invites, err := s.store.ListInvites(r.Context())
	if err != nil {
		s.internalError(w, "list invites", err)
		return
	}
	out := make([]adminInviteDTO, 0, len(invites))
	for _, inv := range invites {
		out = append(out, adminInviteDTO{
			Code: inv.Code, CreatedBy: inv.CreatedBy, CreatedAt: inv.CreatedAt,
			UsedBy: inv.UsedBy, UsedAt: inv.UsedAt, Used: inv.UsedBy != "" || inv.UsedAt != 0,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"invites": out})
}

func (s *Server) handleAdminInviteCreate(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	code, err := s.newInvite(r.Context(), actor.ID)
	if err != nil {
		s.internalError(w, "create invite", err)
		return
	}
	s.audit(r, actor, "invite.create", code, nil)
	writeJSON(w, http.StatusCreated, map[string]any{"invite": adminInviteDTO{
		Code: code, CreatedBy: actor.ID, CreatedAt: protocol.NowMS(),
	}})
}

func (s *Server) handleAdminInviteRevoke(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	code := auth.NormalizeInviteCode(r.URL.Query().Get("code"))
	if code == "" {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "missing invite code")
		return
	}
	if err := s.store.DeleteInvite(r.Context(), code); err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeError(w, http.StatusNotFound, protocol.CodeNotFound, "invite not found")
			return
		}
		s.internalError(w, "delete invite", err)
		return
	}
	s.audit(r, actor, "invite.revoke", code, nil)
	w.WriteHeader(http.StatusNoContent)
}

// ------------------------------------------------------------------
// 频道管理
// ------------------------------------------------------------------

type adminChannelDTO struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Description string `json:"description"`
	Position    int    `json:"position"`
	CreatedAt   int64  `json:"created_at"`
}

func (s *Server) handleAdminChannels(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.requireRole(w, r, store.RoleAdmin); !ok {
		return
	}
	channels, err := s.store.ListChannels(r.Context())
	if err != nil {
		s.internalError(w, "list channels", err)
		return
	}
	out := make([]adminChannelDTO, 0, len(channels))
	for _, c := range channels {
		out = append(out, adminChannelDTO{ID: c.ID, Name: c.Name, Description: c.Description, Position: c.Position, CreatedAt: c.CreatedAt})
	}
	writeJSON(w, http.StatusOK, map[string]any{"channels": out})
}

func (s *Server) handleAdminChannelCreate(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	var req struct {
		Name        string `json:"name"`
		Description string `json:"description"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if req.Name == "" || len([]rune(req.Name)) > 64 {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "channel name must be 1-64 characters")
		return
	}
	ch, err := s.store.CreateChannel(r.Context(), req.Name)
	if errors.Is(err, store.ErrConflict) {
		writeError(w, http.StatusConflict, protocol.CodeConflict, "channel name already taken")
		return
	}
	if err != nil {
		s.internalError(w, "create channel", err)
		return
	}
	if req.Description != "" {
		if err := s.store.SetChannelDescription(r.Context(), ch.ID, req.Description); err == nil {
			ch.Description = req.Description
		}
	}
	s.audit(r, actor, "channel.create", ch.ID, map[string]any{"name": ch.Name})
	s.hub.broadcastMemberState() // 名册 DTO 不含频道；保留调用以兼容未来频道列表推送
	writeJSON(w, http.StatusCreated, adminChannelDTO{ID: ch.ID, Name: ch.Name, Description: ch.Description, Position: ch.Position, CreatedAt: ch.CreatedAt})
}

func (s *Server) handleAdminChannelPatch(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	var req struct {
		ID          string   `json:"id"`
		Name        *string  `json:"name"`
		Description *string  `json:"description"`
		Order       []string `json:"order"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	ctx := r.Context()
	if len(req.Order) > 0 {
		if err := s.store.ReorderChannels(ctx, req.Order); err != nil {
			s.internalError(w, "reorder channels", err)
			return
		}
		s.audit(r, actor, "channel.reorder", "", map[string]any{"order": req.Order})
		writeJSON(w, http.StatusOK, map[string]any{"order": req.Order})
		return
	}
	if req.ID == "" {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "missing channel id")
		return
	}
	if _, err := s.store.ChannelByID(ctx, req.ID); errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "channel not found")
		return
	}
	if req.Name != nil {
		name := *req.Name
		if name == "" || len([]rune(name)) > 64 {
			writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "channel name must be 1-64 characters")
			return
		}
		if err := s.store.RenameChannel(ctx, req.ID, name); err != nil {
			if errors.Is(err, store.ErrConflict) {
				writeError(w, http.StatusConflict, protocol.CodeConflict, "channel name already taken")
				return
			}
			if errors.Is(err, store.ErrNotFound) {
				writeError(w, http.StatusNotFound, protocol.CodeNotFound, "channel not found")
				return
			}
			s.internalError(w, "rename channel", err)
			return
		}
		s.audit(r, actor, "channel.rename", req.ID, map[string]any{"name": name})
	}
	if req.Description != nil {
		if err := s.store.SetChannelDescription(ctx, req.ID, *req.Description); err != nil {
			s.internalError(w, "set channel description", err)
			return
		}
	}
	ch, err := s.store.ChannelByID(ctx, req.ID)
	if err != nil {
		s.internalError(w, "load channel", err)
		return
	}
	writeJSON(w, http.StatusOK, adminChannelDTO{ID: ch.ID, Name: ch.Name, Description: ch.Description, Position: ch.Position, CreatedAt: ch.CreatedAt})
}

func (s *Server) handleAdminChannelDelete(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	id := r.URL.Query().Get("id")
	if id == "" {
		id = r.PathValue("id")
	}
	if id == "" {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "missing channel id")
		return
	}
	ctx := r.Context()
	ch, err := s.store.ChannelByID(ctx, id)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "channel not found")
		return
	}
	if err != nil {
		s.internalError(w, "load channel", err)
		return
	}
	// 有在线订阅/语音/共享的频道不允许删：先让人离开，避免悬空状态。
	if s.hub.ChannelBusy(id) {
		writeError(w, http.StatusConflict, protocol.CodeConflict, "channel is in use")
		return
	}
	if err := s.store.DeleteChannel(ctx, id); err != nil {
		s.internalError(w, "delete channel", err)
		return
	}
	s.audit(r, actor, "channel.delete", id, map[string]any{"name": ch.Name})
	w.WriteHeader(http.StatusNoContent)
}

// ------------------------------------------------------------------
// 消息删除（管理）
// ------------------------------------------------------------------

func (s *Server) handleAdminMessageDelete(w http.ResponseWriter, r *http.Request) {
	actor, ok := s.requireRole(w, r, store.RoleAdmin)
	if !ok {
		return
	}
	var req struct {
		ChannelID string `json:"channel_id"`
		MessageID string `json:"message_id"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	if req.ChannelID == "" || req.MessageID == "" {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "channel_id and message_id are required")
		return
	}
	msg, err := s.store.DeleteMessage(r.Context(), req.ChannelID, req.MessageID)
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, protocol.CodeNotFound, "message not found")
		return
	}
	if err != nil {
		s.internalError(w, "delete message", err)
		return
	}
	s.audit(r, actor, "message.delete", req.MessageID, map[string]any{
		"channel_id": req.ChannelID, "member_id": msg.MemberID,
	})
	w.WriteHeader(http.StatusNoContent)
}

// ------------------------------------------------------------------
// 审计查询
// ------------------------------------------------------------------

func (s *Server) handleAdminAudit(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.requireRole(w, r, store.RoleAdmin); !ok {
		return
	}
	limit := 100
	if raw := r.URL.Query().Get("limit"); raw != "" {
		if v, err := strconv.Atoi(raw); err == nil {
			limit = v
		}
	}
	var before int64
	if raw := r.URL.Query().Get("before"); raw != "" {
		if v, err := strconv.ParseInt(raw, 10, 64); err == nil {
			before = v
		}
	}
	events, err := s.store.ListAudit(r.Context(), limit, before)
	if err != nil {
		s.internalError(w, "list audit", err)
		return
	}
	out := make([]map[string]any, 0, len(events))
	for _, ev := range events {
		out = append(out, map[string]any{
			"id": ev.ID, "at_ms": ev.AtMS, "actor_id": ev.ActorID, "actor_name": ev.ActorName,
			"action": ev.Action, "target": ev.Target, "detail": ev.Detail, "ip": ev.IP,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"events": out})
}

// ------------------------------------------------------------------
// 自助修改（§2：所有角色都能改自己的显示名/密码）
// ------------------------------------------------------------------

func (s *Server) handleUpdateMe(w http.ResponseWriter, r *http.Request) {
	member, _, err := s.currentMember(r)
	switch {
	case errors.Is(err, errBanned):
		writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
		return
	case err != nil:
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return
	}
	var req struct {
		DisplayName string `json:"display_name"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	displayName, ok := resolveDisplayName(req.DisplayName, member.Name)
	if !ok {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, invalidDisplayNameMessage)
		return
	}
	if err := s.store.SetMemberDisplayName(r.Context(), member.ID, displayName); err != nil {
		s.internalError(w, "set display name", err)
		return
	}
	s.audit(r, member, "member.rename", member.ID, map[string]any{
		"self": true, "display_name": displayName,
	})
	member.DisplayName = displayName
	s.hub.broadcastMemberState()
	writeJSON(w, http.StatusOK, map[string]any{"member": toProtocolMember(member)})
}

func (s *Server) handleChangePassword(w http.ResponseWriter, r *http.Request) {
	member, _, err := s.currentMember(r)
	switch {
	case errors.Is(err, errBanned):
		writeError(w, http.StatusForbidden, protocol.CodeMemberBanned, "this account is banned")
		return
	case err != nil:
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "missing or invalid bearer token")
		return
	}
	var req struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := decodeJSON(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, err.Error())
		return
	}
	rec, err := s.store.MemberByName(r.Context(), member.Name)
	if err != nil {
		s.internalError(w, "load member", err)
		return
	}
	ok, err := auth.VerifyPassword(rec.PasswordHash, req.OldPassword)
	if err != nil {
		s.internalError(w, "verify password", err)
		return
	}
	if !ok {
		writeError(w, http.StatusUnauthorized, protocol.CodeUnauthenticated, "old password is incorrect")
		return
	}
	if !auth.ValidPassword(req.NewPassword) {
		writeError(w, http.StatusBadRequest, protocol.CodeInvalidArgument, "invalid new password")
		return
	}
	hash, err := auth.HashPassword(req.NewPassword)
	if err != nil {
		s.internalError(w, "hash password", err)
		return
	}
	if err := s.store.SetMemberPassword(r.Context(), member.ID, hash); err != nil {
		s.internalError(w, "set password", err)
		return
	}
	s.audit(r, member, "member.reset_password", member.ID, map[string]any{"self": true})
	w.WriteHeader(http.StatusNoContent)
}

// toAdminChannelDTO 把 store.Channel 转成管理页 DTO（供测试/后续复用）。
func toAdminChannelDTO(c store.Channel) adminChannelDTO {
	return adminChannelDTO{ID: c.ID, Name: c.Name, Description: c.Description, Position: c.Position, CreatedAt: c.CreatedAt}
}

var _ = toAdminChannelDTO
var _ = fmt.Sprintf
