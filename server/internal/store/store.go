// Package store 是 PartyUp 服务端的持久层（SQLite）。
//
// 设计取舍（见 docs/ARCHITECTURE.md §6）：单二进制 + 单文件库，备份 = 停服拷贝
// data/。驱动用 modernc.org/sqlite（纯 Go，无 cgo），这样 WSL 里 go build 与
// 交叉编译到 Windows 都不需要 C 工具链。
//
// 并发模型：database/sql 连接池固定为 1 条连接。SQLite 的写者本来就是串行的，
// 单连接把「锁竞争」提前变成 Go 侧的排队，行为可预测；ASCII 小规模（2–12 人）
// 下性能完全够用。所有查询都带 context，便于测试与优雅停机时取消。
package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/oklog/ulid/v2"
	sqlite3 "modernc.org/sqlite"
)

// 领域错误。上层据此映射协议错误码：ErrNotFound → NOT_FOUND，
// ErrConflict → CONFLICT，其余 → INTERNAL。
var (
	ErrNotFound = errors.New("store: not found")
	ErrConflict = errors.New("store: conflict")

	// 邀请码注册的两个具体冲突原因，给客户端更准确的提示。
	ErrInviteUsed = errors.New("store: invite already used")
	ErrNameTaken  = errors.New("store: name already taken")
)

// 领域对象。字段与 protocol 包里的同名结构一一对应，转换在 server 层做，
// 这样持久层不依赖协议（也不需要为了一个 JSON tag 去 import protocol）。
type Member struct {
	ID   string
	Name string
	// DisplayName 是展示用名称，独立于唯一的登录名 Name。旧库/旧行在迁移时
	// 回填为 Name；读取时仍以 COALESCE 兜底，保证任何行都不会拿到空显示名。
	DisplayName string
	// Role 是权限的唯一真源（2026-11-07，身份/权限第一批）：owner / admin / user。
	// 旧的 is_admin 列保留双写，仅为降级兼容；读取只认 Role。
	Role Role
	// Admin 是给旧客户端/旧协议字段的兼容位，恒等于 Role.IsAdmin()。
	Admin     bool
	CreatedAt int64
}

// MemberRecord 是带密码哈希的成员记录，只在登录/注册路径出现。
type MemberRecord struct {
	Member
	PasswordHash string
}

// Invite 是邀请码。
type Invite struct {
	Code      string
	CreatedBy string
	CreatedAt int64
	UsedBy    string
	UsedAt    int64
}

// Channel 是频道。骨架只持久化 id/name/created_at，三区内容在内存里（语音/共享为空）。
// 2026-11-07 增加 description/position（管理页可改）；旧库迁移时 position 按创建顺序回填。
type Channel struct {
	ID          string
	Name        string
	Description string
	Position    int
	CreatedAt   int64
}

// Message 是一条文字消息。MemberName 由 History 联表带出（实时广播时由 hub 填
// 当前连接的身份），这样历史消息里的离线成员也能显示名字。
type Message struct {
	ID                string
	ChannelID         string
	Seq               int64
	MemberID          string
	MemberName        string
	MemberDisplayName string
	Body              string
	TS                int64
}

// Store 是库句柄。
type Store struct {
	db   *sql.DB
	path string
}

// schema 是建表语句集合。用 IF NOT EXISTS，启动时幂等执行。v1 不引入通用
// schema_version 迁移框架：只有明确的加法式列迁移，见 migrate。
var schema = []string{
	`CREATE TABLE IF NOT EXISTS settings (
		key   TEXT PRIMARY KEY,
		value TEXT NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS members (
		id            TEXT PRIMARY KEY,
		name          TEXT NOT NULL COLLATE NOCASE UNIQUE,
		display_name  TEXT NOT NULL DEFAULT '',
		password_hash TEXT NOT NULL,
		is_admin      INTEGER NOT NULL DEFAULT 0,
		role          TEXT NOT NULL DEFAULT 'user',
		created_at    INTEGER NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS sessions (
		token_hash TEXT PRIMARY KEY,
		member_id  TEXT NOT NULL REFERENCES members(id) ON DELETE CASCADE,
		created_at INTEGER NOT NULL,
		expires_at INTEGER NOT NULL,
		revoked_at INTEGER
	)`,
	`CREATE INDEX IF NOT EXISTS sessions_member ON sessions(member_id)`,
	`CREATE TABLE IF NOT EXISTS invites (
		code       TEXT PRIMARY KEY,
		created_by TEXT REFERENCES members(id),
		created_at INTEGER NOT NULL,
		used_by    TEXT REFERENCES members(id),
		used_at    INTEGER
	)`,
	`CREATE TABLE IF NOT EXISTS channels (
		id          TEXT PRIMARY KEY,
		name        TEXT NOT NULL COLLATE NOCASE UNIQUE,
		description TEXT NOT NULL DEFAULT '',
		position    INTEGER NOT NULL DEFAULT 0,
		created_at  INTEGER NOT NULL
	)`,
	`CREATE TABLE IF NOT EXISTS messages (
		id         TEXT PRIMARY KEY,
		channel_id TEXT NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
		seq        INTEGER NOT NULL,
		member_id  TEXT NOT NULL REFERENCES members(id),
		body       TEXT NOT NULL,
		ts         INTEGER NOT NULL,
		UNIQUE (channel_id, seq)
	)`,
	`CREATE INDEX IF NOT EXISTS messages_channel_seq ON messages(channel_id, seq)`,
	// 审计日志（2026-11-07）：业务事务提交后写入，失败只 warn，不影响主操作。
	// detail 是 JSON 摘要，禁止放密码/token；actor 是操作者快照（改名后仍可追溯）。
	`CREATE TABLE IF NOT EXISTS audit_events (
		id         INTEGER PRIMARY KEY AUTOINCREMENT,
		at_ms      INTEGER NOT NULL,
		actor_id   TEXT NOT NULL,
		actor_name TEXT NOT NULL,
		action     TEXT NOT NULL,
		target     TEXT,
		detail     TEXT,
		ip         TEXT
	)`,
	`CREATE INDEX IF NOT EXISTS audit_events_at ON audit_events(at_ms DESC)`,
	// 永久封禁（2026-11-07）：封禁 = 撤销全部 session + 关 WS + 登录/WS/me 一律
	// 403 MEMBER_BANNED；解封即删行。owner 不可被封禁（服务层保证）。
	`CREATE TABLE IF NOT EXISTS bans (
		member_id TEXT PRIMARY KEY REFERENCES members(id) ON DELETE CASCADE,
		banned_at INTEGER NOT NULL,
		banned_by TEXT NOT NULL,
		reason    TEXT
	)`,
}

// Open 打开（必要时创建）数据库并应用 schema。dir 不存在时递归创建。
func Open(dir string) (*Store, error) {
	if dir == "" {
		return nil, errors.New("store: empty data dir")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("store: create data dir: %w", err)
	}
	path := filepath.Join(dir, "partyup.db")

	// pragma 通过 DSN 注入：WAL 让读不阻塞写，busy_timeout 吸收短时锁竞争。
	dsn := "file:" + path + "?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)&_pragma=foreign_keys(1)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("store: open %s: %w", path, err)
	}
	db.SetMaxOpenConns(1)
	db.SetConnMaxLifetime(0)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := db.PingContext(ctx); err != nil {
		db.Close()
		return nil, fmt.Errorf("store: ping %s: %w", path, err)
	}
	for _, stmt := range schema {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			db.Close()
			return nil, fmt.Errorf("store: apply schema: %w", err)
		}
	}
	if err := migrate(ctx, db); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db, path: path}, nil
}

// migrate 执行加法式、幂等的列迁移。
//
// 旧库（v1 之前）的 members 表没有 display_name：CREATE TABLE IF NOT EXISTS 不会
// 改已存在的表，所以必须显式 ADD COLUMN，并把旧行回填为登录名。重复启动时列已
// 存在、回填无匹配行，两步都无副作用。
//
// 2026-11-07 加入 sessions.expires_at（会话过期，见 PROTOCOL.md §1.1）：
// 旧库的行没有过期概念，迁移时按「created_at + 默认 TTL」回填，保证升级后既不会
// 把老 token 全部踢掉、也不会留下永不过期的记录。迁移发生在读取 CLI 之前，所以
// 只能用 DefaultSessionTTL；运维改 --session-ttl 只影响之后签发的会话。
func migrate(ctx context.Context, db *sql.DB) error {
	var n int
	if err := db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM pragma_table_info('members') WHERE name = 'display_name'`).Scan(&n); err != nil {
		return fmt.Errorf("store: inspect members schema: %w", err)
	}
	if n == 0 {
		if _, err := db.ExecContext(ctx,
			`ALTER TABLE members ADD COLUMN display_name TEXT NOT NULL DEFAULT ''`); err != nil {
			return fmt.Errorf("store: add members.display_name: %w", err)
		}
	}
	if _, err := db.ExecContext(ctx,
		`UPDATE members SET display_name = name WHERE display_name = ''`); err != nil {
		return fmt.Errorf("store: backfill members.display_name: %w", err)
	}

	// SQLite 的 ADD COLUMN 要求 NOT NULL 列带常量默认值，所以先用 0 占位，
	// 再回填成真实的过期时间；回填条件是 expires_at = 0，重复执行无副作用。
	if err := db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM pragma_table_info('sessions') WHERE name = 'expires_at'`).Scan(&n); err != nil {
		return fmt.Errorf("store: inspect sessions schema: %w", err)
	}
	if n == 0 {
		if _, err := db.ExecContext(ctx,
			`ALTER TABLE sessions ADD COLUMN expires_at INTEGER NOT NULL DEFAULT 0`); err != nil {
			return fmt.Errorf("store: add sessions.expires_at: %w", err)
		}
	}
	if _, err := db.ExecContext(ctx,
		`UPDATE sessions SET expires_at = created_at + ? WHERE expires_at = 0`,
		DefaultSessionTTL.Milliseconds()); err != nil {
		return fmt.Errorf("store: backfill sessions.expires_at: %w", err)
	}

	// 角色迁移（2026-11-07）：先加列（默认 'user'），真实回填在 EnsureOwner 里做——
	// 那里才知道「谁是第一个成员」以及旧库里的 is_admin 行，而且要在建唯一索引之前。
	if err := db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM pragma_table_info('members') WHERE name = 'role'`).Scan(&n); err != nil {
		return fmt.Errorf("store: inspect members.role: %w", err)
	}
	if n == 0 {
		if _, err := db.ExecContext(ctx,
			`ALTER TABLE members ADD COLUMN role TEXT NOT NULL DEFAULT 'user'`); err != nil {
			return fmt.Errorf("store: add members.role: %w", err)
		}
	}

	// 频道管理（2026-11-07）：说明与排序。position 用旧行的创建顺序回填，保证
	// 升级后频道列表顺序不变（旧客户端不依赖它，新管理页按它排序）。
	for _, col := range []struct{ name, ddl string }{
		{"description", `ALTER TABLE channels ADD COLUMN description TEXT NOT NULL DEFAULT ''`},
		{"position", `ALTER TABLE channels ADD COLUMN position INTEGER NOT NULL DEFAULT 0`},
	} {
		if err := db.QueryRowContext(ctx,
			`SELECT COUNT(*) FROM pragma_table_info('channels') WHERE name = ?`, col.name).Scan(&n); err != nil {
			return fmt.Errorf("store: inspect channels.%s: %w", col.name, err)
		}
		if n == 0 {
			if _, err := db.ExecContext(ctx, col.ddl); err != nil {
				return fmt.Errorf("store: add channels.%s: %w", col.name, err)
			}
		}
	}
	// 只给 position 仍为 0 的行按创建顺序编号，重复执行无副作用。
	if _, err := db.ExecContext(ctx,
		`UPDATE channels SET position = (
		    SELECT COUNT(*) FROM channels c2
		     WHERE c2.created_at < channels.created_at
		        OR (c2.created_at = channels.created_at AND c2.id < channels.id))
		  WHERE position = 0`); err != nil {
		return fmt.Errorf("store: backfill channels.position: %w", err)
	}
	return nil
}

// Close 关闭库。
func (s *Store) Close() error { return s.db.Close() }

// Path 返回数据库文件路径（测试与运维输出用）。
func (s *Store) Path() string { return s.path }

// isConstraint 判断错误是否是唯一约束冲突（成员名/频道名/邀请码重复）。
//
// SQLITE_CONSTRAINT_UNIQUE = 2067、SQLITE_CONSTRAINT_PRIMARYKEY = 1555，
// 见 https://www.sqlite.org/rescode.html#constraint_unique。现代驱动把它们
// 包成 *sqlite.Error，用 errors.As 取原始返回码，而不是去匹配错误字符串。
func isConstraint(err error) bool {
	var se *sqlite3.Error
	if errors.As(err, &se) {
		switch se.Code() {
		case 2067, 1555: // UNIQUE / PRIMARY KEY
			return true
		}
	}
	return false
}

func newID() string { return ulid.Make().String() }

// ------------------------------------------------------------------
// settings（bootstrap token 等内部状态）
// ------------------------------------------------------------------

// GetSetting 读一个内部设置。不存在返回 ErrNotFound。
func (s *Store) GetSetting(ctx context.Context, key string) (string, error) {
	var v string
	err := s.db.QueryRowContext(ctx, `SELECT value FROM settings WHERE key = ?`, key).Scan(&v)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", fmt.Errorf("store: get setting %q: %w", key, err)
	}
	return v, nil
}

// SetSetting 写入一个内部设置。
func (s *Store) SetSetting(ctx context.Context, key, value string) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO settings (key, value) VALUES (?, ?)
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, value)
	if err != nil {
		return fmt.Errorf("store: set setting %q: %w", key, err)
	}
	return nil
}

// DeleteSetting 删除一个内部设置（不存在不报错）。
func (s *Store) DeleteSetting(ctx context.Context, key string) error {
	if _, err := s.db.ExecContext(ctx, `DELETE FROM settings WHERE key = ?`, key); err != nil {
		return fmt.Errorf("store: delete setting %q: %w", key, err)
	}
	return nil
}

// ------------------------------------------------------------------
// members
// ------------------------------------------------------------------

// MemberCount 返回成员总数（bootstrap 用：0 表示还没初始化）。
func (s *Store) MemberCount(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM members`).Scan(&n); err != nil {
		return 0, fmt.Errorf("store: count members: %w", err)
	}
	return n, nil
}

// CreateMember 创建成员（role 由 admin 布尔推导，兼容旧代码/旧测试）。
// 新代码要指定角色请用 CreateMemberWithRole。
func (s *Store) CreateMember(ctx context.Context, name, displayName, passwordHash string, admin bool) (Member, error) {
	role := RoleUser
	if admin {
		role = RoleAdmin
	}
	return s.CreateMemberWithRole(ctx, name, displayName, passwordHash, role)
}

// CreateMemberWithRole 创建成员并指定角色。名字重复返回 ErrConflict。
// is_admin 与 role 双写：旧二进制/旧查询仍能读到管理员位（读取只认 role）。
func (s *Store) CreateMemberWithRole(ctx context.Context, name, displayName, passwordHash string, role Role) (Member, error) {
	if !role.Valid() {
		return Member{}, fmt.Errorf("store: invalid role %q", role)
	}
	displayName = strings.TrimSpace(displayName)
	if displayName == "" {
		displayName = name
	}
	m := Member{ID: newID(), Name: name, DisplayName: displayName, Role: role, Admin: role.IsAdmin(), CreatedAt: time.Now().UnixMilli()}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO members (id, name, display_name, password_hash, is_admin, role, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
		m.ID, m.Name, m.DisplayName, passwordHash, boolInt(m.Admin), string(role), m.CreatedAt)
	if isConstraint(err) {
		return Member{}, ErrConflict
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: create member: %w", err)
	}
	return m, nil
}

// MemberByName 按名字取成员及其密码哈希（登录用）。不存在返回 ErrNotFound。
func (s *Store) MemberByName(ctx context.Context, name string) (MemberRecord, error) {
	var r MemberRecord
	var role string
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, COALESCE(NULLIF(display_name, ''), name), password_hash, role, created_at
		   FROM members WHERE name = ?`, name).
		Scan(&r.ID, &r.Name, &r.DisplayName, &r.PasswordHash, &role, &r.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return MemberRecord{}, ErrNotFound
	}
	if err != nil {
		return MemberRecord{}, fmt.Errorf("store: member by name: %w", err)
	}
	r.Role = Role(role)
	r.Admin = r.Role.IsAdmin()
	return r, nil
}

// ListMembers 按创建顺序列出全部成员（含离线）。只返回展示所需字段，
// 不含密码哈希或任何会话 token。
func (s *Store) ListMembers(ctx context.Context) ([]Member, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, name, COALESCE(NULLIF(display_name, ''), name), role, created_at
		   FROM members ORDER BY created_at ASC, id ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list members: %w", err)
	}
	defer rows.Close()

	members := make([]Member, 0, 8)
	for rows.Next() {
		var m Member
		var role string
		if err := rows.Scan(&m.ID, &m.Name, &m.DisplayName, &role, &m.CreatedAt); err != nil {
			return nil, fmt.Errorf("store: scan member: %w", err)
		}
		m.Role = Role(role)
		m.Admin = m.Role.IsAdmin()
		members = append(members, m)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate members: %w", err)
	}
	return members, nil
}

// ------------------------------------------------------------------
// sessions
// ------------------------------------------------------------------

// DefaultSessionTTL 是会话的默认有效期（30 天）。
//
// 两个用途：
//   - 命令行 --session-ttl 的缺省值（main 侧解析，server.New 也会兜底）；
//   - 旧库迁移时回填 expires_at 的唯一依据——迁移发生在读取 CLI 参数之前，
//     所以只能用这个编译期常量。
const DefaultSessionTTL = 30 * 24 * time.Hour

// SessionState 是一个 token 的会话状态快照，**只用于把鉴权失败映射成更准确的
// 协议错误码**（WS auth.err 的 SESSION_REPLACED 与 UNAUTHENTICATED），不对外暴露。
//
// ExpiresAt（毫秒 epoch）也给滑动续期的回归测试用：断言「何时写、写到什么值」。
type SessionState struct {
	Exists    bool  // sessions 表里是否有这个 token hash
	Revoked   bool  // 是否已被撤销（登录替换或退出登录）
	Expired   bool  // expires_at 是否已过
	ExpiresAt int64 // 过期时刻（毫秒 epoch）；Exists=false 时无意义
}

// CreateSession 记录一个会话 token 的哈希。明文 token 只出现在响应里。
//
// expiresAt 是毫秒 epoch。单独签发（不撤销别的会话）只用于测试与内部工具；
// 所有面向用户的登录入口都必须走 ReplaceSessions（单端登录）。
func (s *Store) CreateSession(ctx context.Context, memberID, tokenHash string, expiresAt int64) error {
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO sessions (token_hash, member_id, created_at, expires_at) VALUES (?, ?, ?, ?)`,
		tokenHash, memberID, time.Now().UnixMilli(), expiresAt)
	if err != nil {
		return fmt.Errorf("store: create session: %w", err)
	}
	return nil
}

// ReplaceSessions 在**同一事务**里撤销该成员所有仍有效的会话，再写入新会话，
// 返回被撤销的条数。这是「同一账号同时只允许一端登录」的原子底座。
//
// 为什么必须是一个事务：如果先撤销、再插入分两步，两个并发登录会交错成
// 「A 撤销旧会话 → B 撤销旧会话 → A 插入 → B 插入」，最终两条新会话都有效，
// 单端登录被击穿。放进一个事务后，配合本包固定 1 条连接的连接池，两次登录严格
// 串行：后提交者会把先提交者的 token 也撤销掉，任一时刻最多一条有效会话。
//
// 注意事务内**不做网络/回调**；关闭旧 WS 连接由 server 层在提交成功后做
// （DB 提交才是「谁赢了」的裁决点，先关连接再提交会出现「关掉了赢家」）。
func (s *Store) ReplaceSessions(ctx context.Context, memberID, newTokenHash string, expiresAt int64) (int, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("store: begin replace sessions: %w", err)
	}
	defer func() { _ = tx.Rollback() }() // Commit 成功后 Rollback 是空操作

	now := time.Now().UnixMilli()
	res, err := tx.ExecContext(ctx,
		`UPDATE sessions SET revoked_at = ? WHERE member_id = ? AND revoked_at IS NULL`, now, memberID)
	if err != nil {
		return 0, fmt.Errorf("store: revoke old sessions: %w", err)
	}
	revoked, err := res.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("store: revoke old sessions rows: %w", err)
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO sessions (token_hash, member_id, created_at, expires_at) VALUES (?, ?, ?, ?)`,
		newTokenHash, memberID, now, expiresAt); err != nil {
		return 0, fmt.Errorf("store: insert replaced session: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("store: commit replace sessions: %w", err)
	}
	return int(revoked), nil
}

// MemberBySession 用 token 哈希换成员；token 不存在、已撤销或已过期都返回 ErrNotFound。
//
// 过期判定在 SQL 里做（expires_at > 当前时间），不依赖调用方时钟换算；
// 撤销与过期对「能不能用」是同一件事，只有 WS 鉴权需要区分错误码时才会再查
// SessionState。
func (s *Store) MemberBySession(ctx context.Context, tokenHash string) (Member, error) {
	var m Member
	var role string
	err := s.db.QueryRowContext(ctx,
		`SELECT m.id, m.name, COALESCE(NULLIF(m.display_name, ''), m.name), m.role, m.created_at
		   FROM sessions s JOIN members m ON m.id = s.member_id
		  WHERE s.token_hash = ? AND s.revoked_at IS NULL AND s.expires_at > ?`,
		tokenHash, time.Now().UnixMilli()).
		Scan(&m.ID, &m.Name, &m.DisplayName, &role, &m.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Member{}, ErrNotFound
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: member by session: %w", err)
	}
	m.Role = Role(role)
	m.Admin = m.Role.IsAdmin()
	return m, nil
}

// RevokeSession 撤销一个 token（退出登录 / 踢下线）。
//
// 幂等：token 行存在但已经是撤销状态时返回 nil（重复调用 POST /api/logout 应
// 继续 204）；token 行完全不存在返回 ErrNotFound（上层映射 401）。区分这两者
// 靠 RowsAffected + 一次存在性查询，而不是把「已撤销」当错误。
func (s *Store) RevokeSession(ctx context.Context, tokenHash string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE sessions SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL`,
		time.Now().UnixMilli(), tokenHash)
	if err != nil {
		return fmt.Errorf("store: revoke session: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: revoke session rows: %w", err)
	}
	if n > 0 {
		return nil
	}
	var exists int
	if err := s.db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM sessions WHERE token_hash = ?`, tokenHash).Scan(&exists); err != nil {
		return fmt.Errorf("store: check session exists: %w", err)
	}
	if exists == 0 {
		return ErrNotFound
	}
	return nil // 已撤销：幂等成功
}

// TouchSession 滑动续期：把 expires_at 推到 now+ttl。
//
// 节流（PROTOCOL.md §1.1）：只有「剩余有效期 < ttl - granularity」时才写，
// 即最多每 granularity 写一次。默认 TTL 30 天、粒度 5 分钟时，活跃连接每 5 分钟
// 至多一次单行 UPDATE——相对每条消息/每次 /api/me 都写，写放大降了几个数量级；
// 代价是 token 的实际失效时刻最多比最后一次活动晚 granularity，可接受。
//
// 过期/已撤销的行不会被「复活」：WHERE 里带 expires_at > now 与 revoked_at IS NULL。
// 返回 nil 只代表「没有报错」，不保证写了行（正常节流路径就是 0 行）。
func (s *Store) TouchSession(ctx context.Context, tokenHash string, now int64, ttl, granularity time.Duration) error {
	if ttl <= 0 {
		return nil
	}
	newExpiry := now + ttl.Milliseconds()
	threshold := newExpiry - granularity.Milliseconds()
	if _, err := s.db.ExecContext(ctx,
		`UPDATE sessions SET expires_at = ?
		  WHERE token_hash = ? AND revoked_at IS NULL AND expires_at > ? AND expires_at < ?`,
		newExpiry, tokenHash, now, threshold); err != nil {
		return fmt.Errorf("store: touch session: %w", err)
	}
	return nil
}

// SessionInfo 是一条会话的**运维视图**：只给 token 哈希前缀，不返回 token 原文，
// 供 `partyup sessions` 子命令核对「每个成员是否只剩一条有效会话」。
type SessionInfo struct {
	TokenHashPrefix string
	MemberID        string
	MemberName      string
	CreatedAt       int64
	ExpiresAt       int64
	RevokedAt       int64 // 0 表示未撤销
	Active          bool  // 未撤销且未过期
}

// ListSessions 按创建时间倒序列出全部会话（含已撤销/已过期）。
func (s *Store) ListSessions(ctx context.Context) ([]SessionInfo, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT substr(s.token_hash, 1, 12), s.member_id, m.name,
		       s.created_at, s.expires_at, COALESCE(s.revoked_at, 0)
		  FROM sessions s JOIN members m ON m.id = s.member_id
		 ORDER BY s.created_at DESC, s.token_hash ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list sessions: %w", err)
	}
	defer rows.Close()

	now := time.Now().UnixMilli()
	out := make([]SessionInfo, 0, 8)
	for rows.Next() {
		var info SessionInfo
		if err := rows.Scan(&info.TokenHashPrefix, &info.MemberID, &info.MemberName,
			&info.CreatedAt, &info.ExpiresAt, &info.RevokedAt); err != nil {
			return nil, fmt.Errorf("store: scan session: %w", err)
		}
		info.Active = info.RevokedAt == 0 && info.ExpiresAt > now
		out = append(out, info)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate sessions: %w", err)
	}
	return out, nil
}

// SessionState 查一个 token 的会话状态；不存在时返回零值（Exists=false）。
// 只在鉴权失败路径调用一次，用来区分「被新登录替换」与「无效/过期」。
func (s *Store) SessionState(ctx context.Context, tokenHash string) (SessionState, error) {
	var revokedAt sql.NullInt64
	var expiresAt int64
	err := s.db.QueryRowContext(ctx,
		`SELECT revoked_at, expires_at FROM sessions WHERE token_hash = ?`, tokenHash).
		Scan(&revokedAt, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		return SessionState{}, nil
	}
	if err != nil {
		return SessionState{}, fmt.Errorf("store: session state: %w", err)
	}
	return SessionState{
		Exists:    true,
		Revoked:   revokedAt.Valid,
		Expired:   expiresAt <= time.Now().UnixMilli(),
		ExpiresAt: expiresAt,
	}, nil
}

// ------------------------------------------------------------------
// invites
// ------------------------------------------------------------------

// CreateInvite 创建邀请码。code 重复返回 ErrConflict（调用方重试即可）。
func (s *Store) CreateInvite(ctx context.Context, code, createdBy string) (Invite, error) {
	inv := Invite{Code: code, CreatedBy: createdBy, CreatedAt: time.Now().UnixMilli()}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO invites (code, created_by, created_at) VALUES (?, ?, ?)`,
		inv.Code, nullableString(createdBy), inv.CreatedAt)
	if isConstraint(err) {
		return Invite{}, ErrConflict
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: create invite: %w", err)
	}
	return inv, nil
}

// InviteByCode 查邀请码。不存在返回 ErrNotFound。
func (s *Store) InviteByCode(ctx context.Context, code string) (Invite, error) {
	var inv Invite
	var createdBy, usedBy sql.NullString
	var usedAt sql.NullInt64
	err := s.db.QueryRowContext(ctx,
		`SELECT code, created_by, created_at, used_by, used_at FROM invites WHERE code = ?`, code).
		Scan(&inv.Code, &createdBy, &inv.CreatedAt, &usedBy, &usedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Invite{}, ErrNotFound
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: invite by code: %w", err)
	}
	inv.CreatedBy = createdBy.String
	inv.UsedBy = usedBy.String
	inv.UsedAt = usedAt.Int64
	return inv, nil
}

// ConsumeInvite 把邀请码标记为已用。已被用掉（并发）返回 ErrConflict。
func (s *Store) ConsumeInvite(ctx context.Context, code, memberID string) error {
	res, err := s.db.ExecContext(ctx,
		`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ? AND used_by IS NULL`,
		memberID, time.Now().UnixMilli(), code)
	if err != nil {
		return fmt.Errorf("store: consume invite: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("store: consume invite rows: %w", err)
	}
	if n == 0 {
		return ErrConflict
	}
	return nil
}

// RegisterWithInvite 在一次事务里「校验邀请码未用 → 建成员 → 标记邀请码已用」。
//
// 三步要么全成要么全不成：否则会出现「邀请码被烧掉但成员没建成」或反过来的半截状态。
// displayName 为空时回退登录名。返回 ErrNotFound（邀请码不存在）、ErrInviteUsed、ErrNameTaken。
func (s *Store) RegisterWithInvite(ctx context.Context, code, name, displayName, passwordHash string) (Member, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Member{}, fmt.Errorf("store: begin invite register: %w", err)
	}
	defer func() { _ = tx.Rollback() }() // Commit 成功后 Rollback 是空操作

	var usedBy sql.NullString
	err = tx.QueryRowContext(ctx, `SELECT used_by FROM invites WHERE code = ?`, code).Scan(&usedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return Member{}, ErrNotFound
	}
	if err != nil {
		return Member{}, fmt.Errorf("store: check invite: %w", err)
	}
	if usedBy.Valid && usedBy.String != "" {
		return Member{}, ErrInviteUsed
	}

	displayName = strings.TrimSpace(displayName)
	if displayName == "" {
		displayName = name
	}
	m := Member{ID: newID(), Name: name, DisplayName: displayName, Role: RoleUser, Admin: false, CreatedAt: time.Now().UnixMilli()}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO members (id, name, display_name, password_hash, is_admin, role, created_at) VALUES (?, ?, ?, ?, 0, ?, ?)`,
		m.ID, m.Name, m.DisplayName, passwordHash, string(RoleUser), m.CreatedAt); err != nil {
		if isConstraint(err) {
			return Member{}, ErrNameTaken
		}
		return Member{}, fmt.Errorf("store: register member: %w", err)
	}

	res, err := tx.ExecContext(ctx,
		`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ? AND used_by IS NULL`,
		m.ID, time.Now().UnixMilli(), code)
	if err != nil {
		return Member{}, fmt.Errorf("store: mark invite used: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return Member{}, fmt.Errorf("store: mark invite used rows: %w", err)
	}
	if n == 0 {
		return Member{}, ErrInviteUsed // 并发抢同一个码
	}

	if err := tx.Commit(); err != nil {
		return Member{}, fmt.Errorf("store: commit invite register: %w", err)
	}
	return m, nil
}

// FirstUnusedInvite 返回最早创建且未使用的邀请码；没有则 ErrNotFound。
func (s *Store) FirstUnusedInvite(ctx context.Context) (Invite, error) {
	var inv Invite
	var createdBy sql.NullString
	err := s.db.QueryRowContext(ctx,
		`SELECT code, created_by, created_at FROM invites
		  WHERE used_by IS NULL ORDER BY created_at ASC LIMIT 1`).
		Scan(&inv.Code, &createdBy, &inv.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Invite{}, ErrNotFound
	}
	if err != nil {
		return Invite{}, fmt.Errorf("store: first unused invite: %w", err)
	}
	inv.CreatedBy = createdBy.String
	return inv, nil
}

// ------------------------------------------------------------------
// channels
// ------------------------------------------------------------------

// CreateChannel 创建频道。名字重复（不区分大小写）返回 ErrConflict。
// 新频道排在最后（position = 当前最大值 + 1）。
func (s *Store) CreateChannel(ctx context.Context, name string) (Channel, error) {
	ch := Channel{ID: newID(), Name: name, CreatedAt: time.Now().UnixMilli()}
	if err := s.db.QueryRowContext(ctx,
		`SELECT COALESCE(MAX(position), -1) + 1 FROM channels`).Scan(&ch.Position); err != nil {
		return Channel{}, fmt.Errorf("store: next channel position: %w", err)
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO channels (id, name, description, position, created_at) VALUES (?, ?, '', ?, ?)`,
		ch.ID, ch.Name, ch.Position, ch.CreatedAt)
	if isConstraint(err) {
		return Channel{}, ErrConflict
	}
	if err != nil {
		return Channel{}, fmt.Errorf("store: create channel: %w", err)
	}
	return ch, nil
}

// ListChannels 按管理排序（position）列出全部频道。
func (s *Store) ListChannels(ctx context.Context) ([]Channel, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, name, description, position, created_at
		   FROM channels ORDER BY position ASC, created_at ASC, id ASC`)
	if err != nil {
		return nil, fmt.Errorf("store: list channels: %w", err)
	}
	defer rows.Close()

	channels := make([]Channel, 0, 8)
	for rows.Next() {
		var ch Channel
		if err := rows.Scan(&ch.ID, &ch.Name, &ch.Description, &ch.Position, &ch.CreatedAt); err != nil {
			return nil, fmt.Errorf("store: scan channel: %w", err)
		}
		channels = append(channels, ch)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate channels: %w", err)
	}
	return channels, nil
}

// ChannelByID 取单个频道；不存在返回 ErrNotFound。
func (s *Store) ChannelByID(ctx context.Context, id string) (Channel, error) {
	var ch Channel
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, description, position, created_at FROM channels WHERE id = ?`, id).
		Scan(&ch.ID, &ch.Name, &ch.Description, &ch.Position, &ch.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Channel{}, ErrNotFound
	}
	if err != nil {
		return Channel{}, fmt.Errorf("store: channel by id: %w", err)
	}
	return ch, nil
}

// ------------------------------------------------------------------
// messages
// ------------------------------------------------------------------

// maxSeqAttempts 是 seq 分配冲突时的重试次数。同进程内单连接池下正常不会冲突，
// 这里只是兜底（例如另一个进程/进程内两条通路同时写库）。
const maxSeqAttempts = 4

// AppendMessage 追加一条消息并分配每频道单调递增的 seq。
//
// seq 用单条 SQL 分配，保证「读 max + 写入」原子：
//
//	INSERT ... SELECT COALESCE(MAX(seq),0)+1 ... RETURNING seq
//
// 不用「先 SELECT 再 INSERT」的两步式写法，那在并发下会撞 UNIQUE(channel_id, seq)。
func (s *Store) AppendMessage(ctx context.Context, channelID, memberID, body string) (Message, error) {
	msg := Message{
		ID:        newID(),
		ChannelID: channelID,
		MemberID:  memberID,
		Body:      body,
		TS:        time.Now().UnixMilli(),
	}

	var lastErr error
	for attempt := 0; attempt < maxSeqAttempts; attempt++ {
		err := s.db.QueryRowContext(ctx,
			`INSERT INTO messages (id, channel_id, seq, member_id, body, ts)
			 SELECT ?, ?, COALESCE(MAX(seq), 0) + 1, ?, ?, ?
			   FROM messages WHERE channel_id = ?
			 RETURNING seq`,
			msg.ID, msg.ChannelID, msg.MemberID, msg.Body, msg.TS, msg.ChannelID).
			Scan(&msg.Seq)
		if err == nil {
			return msg, nil
		}
		if !isConstraint(err) {
			return Message{}, fmt.Errorf("store: append message: %w", err)
		}
		lastErr = err
	}
	return Message{}, fmt.Errorf("store: append message: seq contention: %w", lastErr)
}

// History 返回某频道 seq 升序的消息。beforeSeq > 0 时只返回 seq < beforeSeq 的，
// 用于向前翻页；limit <= 0 或超上限时按上限截断。
func (s *Store) History(ctx context.Context, channelID string, beforeSeq int64, limit int) ([]Message, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	var (
		rows *sql.Rows
		err  error
	)
	if beforeSeq > 0 {
		rows, err = s.db.QueryContext(ctx,
			`SELECT m.id, m.channel_id, m.seq, m.member_id,
			        COALESCE(mb.name, ''), COALESCE(NULLIF(mb.display_name, ''), mb.name, ''), m.body, m.ts
			   FROM messages m LEFT JOIN members mb ON mb.id = m.member_id
			  WHERE m.channel_id = ? AND m.seq < ? ORDER BY m.seq DESC LIMIT ?`,
			channelID, beforeSeq, limit)
	} else {
		rows, err = s.db.QueryContext(ctx,
			`SELECT m.id, m.channel_id, m.seq, m.member_id,
			        COALESCE(mb.name, ''), COALESCE(NULLIF(mb.display_name, ''), mb.name, ''), m.body, m.ts
			   FROM messages m LEFT JOIN members mb ON mb.id = m.member_id
			  WHERE m.channel_id = ? ORDER BY m.seq DESC LIMIT ?`,
			channelID, limit)
	}
	if err != nil {
		return nil, fmt.Errorf("store: history: %w", err)
	}
	defer rows.Close()

	// 倒序取、正序还：这样 LIMIT 截的是「最近的 N 条」，对外仍按 seq 升序。
	desc := make([]Message, 0, limit)
	for rows.Next() {
		var m Message
		if err := rows.Scan(&m.ID, &m.ChannelID, &m.Seq, &m.MemberID,
			&m.MemberName, &m.MemberDisplayName, &m.Body, &m.TS); err != nil {
			return nil, fmt.Errorf("store: scan message: %w", err)
		}
		desc = append(desc, m)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("store: iterate messages: %w", err)
	}
	for i, j := 0, len(desc)-1; i < j; i, j = i+1, j-1 {
		desc[i], desc[j] = desc[j], desc[i]
	}
	return desc, nil
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func nullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
