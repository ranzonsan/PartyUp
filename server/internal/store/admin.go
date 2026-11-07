package store

// 身份/权限第一批（2026-11-07）：角色、所有者回填、审计日志、封禁、成员/频道/邀请
// 的管理写操作。规格见 `.docs/identity-and-onboarding-20261107.md` §1/§4/§5。
//
// 这一层只做持久化与事务边界；权限判断（谁能调）全在 server 的 requireRole 中间件，
// 但「owner 不可降级/不可封禁」这类**数据不变量**在这里兜底，避免绕过 HTTP 层写坏库。

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"
)

// Role 是服务端权限的唯一真源：owner / admin / user。
// `is_admin` 列保留双写只为旧二进制降级兼容，任何读取都只认 role。
type Role string

const (
	RoleOwner Role = "owner"
	RoleAdmin Role = "admin"
	RoleUser  Role = "user"
)

// 角色相关的领域错误（server 层映射成 403 owner_only / 409 conflict）。
var (
	// ErrOwnerProtected：目标成员是 owner，不允许降级/踢下线/封禁。
	ErrOwnerProtected = errors.New("store: owner is protected")
	// ErrInvalidRole：角色值不在 owner/admin/user 之内。
	ErrInvalidRole = errors.New("store: invalid role")
)

// Valid 报告角色值是否合法。
func (r Role) Valid() bool {
	switch r {
	case RoleOwner, RoleAdmin, RoleUser:
		return true
	}
	return false
}

// IsAdmin 报告角色是否具备管理员及以上权限（owner 也是管理员）。
func (r Role) IsAdmin() bool { return r == RoleOwner || r == RoleAdmin }

// ParseRole 解析外部传入的角色字符串。
func ParseRole(s string) (Role, bool) {
	r := Role(s)
	return r, r.Valid()
}

// EnsureOwner 保证「全服恰好一个 owner」的迁移前提（2026-11-07，规格 §1）。
//
// 顺序（每步都必须在这个次序，否则唯一索引会在旧库上建失败）：
//  1. 若没有任何 role='owner'，把最早创建的成员回填为 owner（旧库升级）；
//  2. 旧库里其它 is_admin=1 的行降为 admin（沿用旧管理员语义，但只保留一个 owner）；
//  3. 建 members_single_owner 部分唯一索引，把「至多一个 owner」变成数据库不变量。
//
// 返回被回填为 owner 的成员名（没有回填时为空串）；main 用它打印 OWNER_FALLBACK。
func (s *Store) EnsureOwner(ctx context.Context) (string, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", fmt.Errorf("store: begin owner migration: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	var owners int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM members WHERE role = 'owner'`).Scan(&owners); err != nil {
		return "", fmt.Errorf("store: count owners: %w", err)
	}
	fallback := ""
	if owners == 0 {
		var id, name string
		err := tx.QueryRowContext(ctx,
			`SELECT id, name FROM members ORDER BY created_at ASC, id ASC LIMIT 1`).Scan(&id, &name)
		switch {
		case errors.Is(err, sql.ErrNoRows):
			// 空库：首次 bootstrap 会创建 owner，这里没有可回填的。
		case err != nil:
			return "", fmt.Errorf("store: pick first member: %w", err)
		default:
			if _, err := tx.ExecContext(ctx,
				`UPDATE members SET role = 'owner', is_admin = 1 WHERE id = ?`, id); err != nil {
				return "", fmt.Errorf("store: backfill owner: %w", err)
			}
			fallback = name
		}
	}
	// 旧库的其它管理员位降为 admin：保留权限、去掉「多个 owner」来源。
	if _, err := tx.ExecContext(ctx,
		`UPDATE members SET role = 'admin' WHERE is_admin = 1 AND role = 'user'`); err != nil {
		return "", fmt.Errorf("store: backfill admins: %w", err)
	}
	if _, err := tx.ExecContext(ctx,
		`CREATE UNIQUE INDEX IF NOT EXISTS members_single_owner ON members(role) WHERE role = 'owner'`); err != nil {
		return "", fmt.Errorf("store: create single-owner index: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return "", fmt.Errorf("store: commit owner migration: %w", err)
	}
	return fallback, nil
}

// MemberByID 按 id 取成员。不存在返回 ErrNotFound。
func (s *Store) MemberByID(ctx context.Context, id string) (Member, error) {
	var m Member
	var role string
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, COALESCE(NULLIF(display_name, ''), name), role, created_at
		   FROM members WHERE id = ?`, id).
		Scan(&m.ID, &m.Name, &m.DisplayName, &role, &m.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Member{}, ErrNotFound
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: member by id: %w", err)
	}
	m.Role = Role(role)
	m.Admin = m.Role.IsAdmin()
	return m, nil
}

// SetMemberDisplayName 修改展示名（登录名不变）。
func (s *Store) SetMemberDisplayName(ctx context.Context, id, displayName string) error {
	res, err := s.db.ExecContext(ctx, `UPDATE members SET display_name = ? WHERE id = ?`, displayName, id)
	if err != nil {
		return fmt.Errorf("store: set display name: %w", err)
	}
	return requireRows(res, "member")
}

// SetMemberPassword 直接覆盖密码哈希（重置密码 / 用户自助改密由 server 校验旧密码）。
func (s *Store) SetMemberPassword(ctx context.Context, id, passwordHash string) error {
	res, err := s.db.ExecContext(ctx, `UPDATE members SET password_hash = ? WHERE id = ?`, passwordHash, id)
	if err != nil {
		return fmt.Errorf("store: set password: %w", err)
	}
	return requireRows(res, "member")
}

// SetMemberRole 把成员在 user ↔ admin 之间调整。owner 必须走 TransferOwner，
// 因此这里对 owner 返回 ErrOwnerProtected（数据层兜底，不依赖调用方先查）。
func (s *Store) SetMemberRole(ctx context.Context, id string, role Role) error {
	if role != RoleUser && role != RoleAdmin {
		return ErrInvalidRole
	}
	res, err := s.db.ExecContext(ctx,
		`UPDATE members SET role = ?, is_admin = ? WHERE id = ? AND role <> 'owner'`,
		string(role), boolInt(role.IsAdmin()), id)
	if err != nil {
		return fmt.Errorf("store: set member role: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: set member role rows: %w", err)
	}
	if n == 0 {
		// 行不存在或目标是 owner：上层需要区分，先查一次。
		if _, err := s.MemberByID(ctx, id); errors.Is(err, ErrNotFound) {
			return ErrNotFound
		}
		return ErrOwnerProtected
	}
	return nil
}

// TransferOwner 把所有权从 currentOwnerID 转给 newOwnerID（规格 §1）。
//
// 必须一个事务、且**先降旧 owner 再升新 owner**：部分唯一索引
// members_single_owner 不允许事务中间出现两个 owner 行。
// 旧 owner 转让后自动降为 admin（不是 user）——保留管理能力、去掉唯一 owner 位。
func (s *Store) TransferOwner(ctx context.Context, currentOwnerID, newOwnerID string) error {
	if currentOwnerID == newOwnerID {
		return ErrConflict
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("store: begin owner transfer: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	res, err := tx.ExecContext(ctx,
		`UPDATE members SET role = 'admin', is_admin = 1 WHERE id = ? AND role = 'owner'`, currentOwnerID)
	if err != nil {
		return fmt.Errorf("store: demote owner: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: demote owner rows: %w", err)
	}
	if n == 0 {
		return ErrOwnerProtected
	}
	res, err = tx.ExecContext(ctx,
		`UPDATE members SET role = 'owner', is_admin = 1 WHERE id = ?`, newOwnerID)
	if err != nil {
		return fmt.Errorf("store: promote owner: %w", err)
	}
	if err := requireRows(res, "member"); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("store: commit owner transfer: %w", err)
	}
	return nil
}

// RevokeAllSessions 撤销某成员的全部会话（踢下线 / 封禁 / 所有权转让后的清理）。
// 返回被撤销的条数；WS 连接由 server 层在提交后关闭。
func (s *Store) RevokeAllSessions(ctx context.Context, memberID string) (int, error) {
	res, err := s.db.ExecContext(ctx,
		`UPDATE sessions SET revoked_at = ? WHERE member_id = ? AND revoked_at IS NULL`,
		time.Now().UnixMilli(), memberID)
	if err != nil {
		return 0, fmt.Errorf("store: revoke all sessions: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("store: revoke all sessions rows: %w", err)
	}
	return int(n), nil
}

// ------------------------------------------------------------------
// 审计日志（规格 §4）
// ------------------------------------------------------------------

// AuditEvent 是一条审计记录。Detail 是 JSON 摘要，**禁止放密码/token**。
type AuditEvent struct {
	ID        int64
	AtMS      int64
	ActorID   string
	ActorName string
	Action    string
	Target    string
	Detail    string
	IP        string
}

// AppendAudit 追加一条审计记录。
//
// 约定（规格 §4）：在业务事务**提交之后**调用，避免回滚产生幻影记录；
// 调用方写失败只记 warn，不影响主操作。本函数不做权限判断。
func (s *Store) AppendAudit(ctx context.Context, ev AuditEvent) error {
	if ev.AtMS == 0 {
		ev.AtMS = time.Now().UnixMilli()
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO audit_events (at_ms, actor_id, actor_name, action, target, detail, ip)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		ev.AtMS, ev.ActorID, ev.ActorName, ev.Action, nullString(ev.Target), nullString(ev.Detail), nullString(ev.IP))
	if err != nil {
		return fmt.Errorf("store: append audit: %w", err)
	}
	return nil
}

// ListAudit 按 id 倒序（最新在前）翻页。beforeID > 0 表示只取 id < beforeID 的。
func (s *Store) ListAudit(ctx context.Context, limit int, beforeID int64) ([]AuditEvent, error) {
	if limit <= 0 || limit > 200 {
		limit = 100
	}
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, at_ms, actor_id, actor_name, action,
		        COALESCE(target, ''), COALESCE(detail, ''), COALESCE(ip, '')
		   FROM audit_events
		  WHERE (? = 0 OR id < ?)
		  ORDER BY id DESC LIMIT ?`,
		beforeID, beforeID, limit)
	if err != nil {
		return nil, fmt.Errorf("store: list audit: %w", err)
	}
	defer rows.Close()

	out := make([]AuditEvent, 0, limit)
	for rows.Next() {
		var ev AuditEvent
		if err := rows.Scan(&ev.ID, &ev.AtMS, &ev.ActorID, &ev.ActorName, &ev.Action,
			&ev.Target, &ev.Detail, &ev.IP); err != nil {
			return nil, fmt.Errorf("store: scan audit: %w", err)
		}
		out = append(out, ev)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate audit: %w", err)
	}
	return out, nil
}

// ------------------------------------------------------------------
// 封禁（规格 §5）
// ------------------------------------------------------------------

// Ban 是一条封禁记录。
type Ban struct {
	MemberID string
	BannedAt int64
	BannedBy string
	Reason   string
}

// BanMember 永久封禁并撤销该成员全部会话（同一事务）。
// owner 不可被封禁 → ErrOwnerProtected；重复封禁覆盖原因（幂等）。
func (s *Store) BanMember(ctx context.Context, memberID, bannedBy, reason string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("store: begin ban: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	var role string
	err = tx.QueryRowContext(ctx, `SELECT role FROM members WHERE id = ?`, memberID).Scan(&role)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("store: ban lookup: %w", err)
	}
	if Role(role) == RoleOwner {
		return ErrOwnerProtected
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO bans (member_id, banned_at, banned_by, reason) VALUES (?, ?, ?, ?)
		 ON CONFLICT(member_id) DO UPDATE SET banned_at = excluded.banned_at,
		   banned_by = excluded.banned_by, reason = excluded.reason`,
		memberID, time.Now().UnixMilli(), bannedBy, reason); err != nil {
		return fmt.Errorf("store: insert ban: %w", err)
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE sessions SET revoked_at = ? WHERE member_id = ? AND revoked_at IS NULL`,
		time.Now().UnixMilli(), memberID); err != nil {
		return fmt.Errorf("store: revoke banned sessions: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("store: commit ban: %w", err)
	}
	return nil
}

// UnbanMember 解封；没有封禁记录不算错误（幂等）。
func (s *Store) UnbanMember(ctx context.Context, memberID string) error {
	if _, err := s.db.ExecContext(ctx, `DELETE FROM bans WHERE member_id = ?`, memberID); err != nil {
		return fmt.Errorf("store: unban: %w", err)
	}
	return nil
}

// IsBanned 报告成员是否被封禁。未知成员返回 false（登录路径随后会走 401）。
func (s *Store) IsBanned(ctx context.Context, memberID string) (bool, error) {
	var n int
	if err := s.db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM bans WHERE member_id = ?`, memberID).Scan(&n); err != nil {
		return false, fmt.Errorf("store: is banned: %w", err)
	}
	return n > 0, nil
}

// MemberAdminView 是管理页的成员行：成员 + 封禁状态。
type MemberAdminView struct {
	Member
	Banned    bool
	BannedAt  int64
	BanReason string
}

// ListMembersAdmin 列出成员及封禁状态（owner/admin 管理页）。
func (s *Store) ListMembersAdmin(ctx context.Context) ([]MemberAdminView, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT m.id, m.name, COALESCE(NULLIF(m.display_name, ''), m.name), m.role, m.created_at,
		        b.banned_at, COALESCE(b.reason, '')
		   FROM members m LEFT JOIN bans b ON b.member_id = m.id
		  ORDER BY m.created_at ASC, m.id ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list members admin: %w", err)
	}
	defer rows.Close()

	out := make([]MemberAdminView, 0, 8)
	for rows.Next() {
		var v MemberAdminView
		var role string
		var bannedAt sql.NullInt64
		if err := rows.Scan(&v.ID, &v.Name, &v.DisplayName, &role, &v.CreatedAt,
			&bannedAt, &v.BanReason); err != nil {
			return nil, fmt.Errorf("store: scan member admin: %w", err)
		}
		v.Role = Role(role)
		v.Admin = v.Role.IsAdmin()
		v.Banned = bannedAt.Valid
		if bannedAt.Valid {
			v.BannedAt = bannedAt.Int64
		}
		out = append(out, v)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate members admin: %w", err)
	}
	return out, nil
}

// ------------------------------------------------------------------
// 邀请码管理（规格 §9）
// ------------------------------------------------------------------

// ListInvites 列出全部邀请码（管理页）。
func (s *Store) ListInvites(ctx context.Context) ([]Invite, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT code, COALESCE(created_by, ''), created_at,
		        COALESCE(used_by, ''), COALESCE(used_at, 0)
		   FROM invites ORDER BY created_at DESC, code ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list invites: %w", err)
	}
	defer rows.Close()

	out := make([]Invite, 0, 8)
	for rows.Next() {
		var inv Invite
		if err := rows.Scan(&inv.Code, &inv.CreatedBy, &inv.CreatedAt, &inv.UsedBy, &inv.UsedAt); err != nil {
			return nil, fmt.Errorf("store: scan invite: %w", err)
		}
		out = append(out, inv)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate invites: %w", err)
	}
	return out, nil
}

// DeleteInvite 吊销邀请码（已使用的码也允许删除，历史记录靠审计日志追溯）。
func (s *Store) DeleteInvite(ctx context.Context, code string) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM invites WHERE code = ?`, code)
	if err != nil {
		return fmt.Errorf("store: delete invite: %w", err)
	}
	return requireRows(res, "invite")
}

// ------------------------------------------------------------------
// 频道管理（规格 §3A/§9）
// ------------------------------------------------------------------

// RenameChannel 改名；重名返回 ErrConflict。
func (s *Store) RenameChannel(ctx context.Context, id, name string) error {
	res, err := s.db.ExecContext(ctx, `UPDATE channels SET name = ? WHERE id = ?`, name, id)
	if isConstraint(err) {
		return ErrConflict
	}
	if err != nil {
		return fmt.Errorf("store: rename channel: %w", err)
	}
	return requireRows(res, "channel")
}

// SetChannelDescription 改频道说明。
func (s *Store) SetChannelDescription(ctx context.Context, id, description string) error {
	res, err := s.db.ExecContext(ctx, `UPDATE channels SET description = ? WHERE id = ?`, description, id)
	if err != nil {
		return fmt.Errorf("store: set channel description: %w", err)
	}
	return requireRows(res, "channel")
}

// DeleteChannel 删除频道；频道内的消息/语音/共享由外键与内存状态负责清理。
func (s *Store) DeleteChannel(ctx context.Context, id string) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM channels WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("store: delete channel: %w", err)
	}
	return requireRows(res, "channel")
}

// ReorderChannels 按给定顺序重写 position（一个事务；ids 里的未知 id 忽略）。
func (s *Store) ReorderChannels(ctx context.Context, ids []string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("store: begin reorder channels: %w", err)
	}
	defer func() { _ = tx.Rollback() }()
	for i, id := range ids {
		if _, err := tx.ExecContext(ctx, `UPDATE channels SET position = ? WHERE id = ?`, i, id); err != nil {
			return fmt.Errorf("store: reorder channel %q: %w", id, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("store: commit reorder channels: %w", err)
	}
	return nil
}

// DeleteMessage 删除一条消息并返回它（供审计记录频道/作者）。
// 消息不存在返回 ErrNotFound；channelID 必须匹配，避免跨频道误删。
func (s *Store) DeleteMessage(ctx context.Context, channelID, messageID string) (Message, error) {
	var m Message
	err := s.db.QueryRowContext(ctx,
		`SELECT id, channel_id, seq, member_id, body, ts
		   FROM messages WHERE id = ? AND channel_id = ?`, messageID, channelID).
		Scan(&m.ID, &m.ChannelID, &m.Seq, &m.MemberID, &m.Body, &m.TS)
	if errors.Is(err, sql.ErrNoRows) {
		return Message{}, ErrNotFound
	}
	if err != nil {
		return Message{}, fmt.Errorf("store: find message: %w", err)
	}
	if _, err := s.db.ExecContext(ctx,
		`DELETE FROM messages WHERE id = ? AND channel_id = ?`, messageID, channelID); err != nil {
		return Message{}, fmt.Errorf("store: delete message: %w", err)
	}
	return m, nil
}

// ------------------------------------------------------------------
// 小工具
// ------------------------------------------------------------------

func requireRows(res sql.Result, what string) error {
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: %s rows: %w", what, err)
	}
	if n == 0 {
		return ErrNotFound
	}
	return nil
}

func nullString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
